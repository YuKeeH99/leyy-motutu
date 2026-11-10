#!/usr/bin/env python3
"""Minimal native NFC identity monitor for the two ARCS-MINI V2.1 boards.

The fixed Reader (``6b3bc01b1634cfbd``) is the only board polled after
discovery.  A rise in its durable ``seen`` counter immediately produces one
native success popup; no Target status is required.
"""

from __future__ import annotations

import argparse
import os
import queue
import re
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from typing import Callable, Sequence


READER_ID = "6b3bc01b1634cfbd"
SUCCESS_TEXT = "识别成功：狗子A 已识别 狗子B"

# ADB is a USB request/response transport, not a memory-mapped status socket.
# Starting a shell every 50 ms can backlog the board's shell task and make a
# real NFC event appear late.  Four samples per second is plenty for a human
# proximity event and leaves the transport responsive.
DEFAULT_POLL_SECONDS = 0.25
DEFAULT_REARM_SECONDS = 1.0
DISCOVERY_RETRY_SECONDS = 0.35
ADB_DISCOVERY_TIMEOUT_SECONDS = 0.7
ADB_STATUS_TIMEOUT_SECONDS = 0.5
SUCCESS_POPUP_MILLISECONDS = 2200

ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
STATUS_RE = re.compile(
    r"nfc peer:\s+enabled=(?P<enabled>[01])\s+"
    r"role=(?P<role>\w+)\s+local=(?P<local>[0-9a-fA-F]{16})\s+"
    r"present=(?P<present>[01])\s+"
    r"peer=(?P<peer>none|[0-9a-fA-F]{16})\s+"
    r"age=(?P<age>n/a|\d+)\s+seen=(?P<seen>\d+)\s+"
    r"enter=(?P<enter>\d+)\s+leave=(?P<leave>\d+)\s+"
    r"advertised=(?P<advertised>\d+)\s+"
    r"last_error=(?P<last_error>-?\d+)"
)


@dataclass(frozen=True)
class AdbDevice:
    """One online ADB transport.

    ``transport_id`` is used for every command because the two boards may
    intentionally expose the same serial number.  ``usb`` is the stable hint
    used to follow the Reader when ADB assigns a new transport after replug.
    """

    transport_id: int
    serial: str
    usb: str


@dataclass(frozen=True)
class PeerStatus:
    local_id: str
    seen: int
    leave: int


def parse_adb_devices(output: str) -> list[AdbDevice]:
    """Parse online transports from ``adb devices -l`` output."""

    devices: list[AdbDevice] = []
    for raw_line in output.splitlines():
        fields = raw_line.strip().split()
        if len(fields) < 2 or fields[0] == "List" or fields[1] != "device":
            continue

        attributes: dict[str, str] = {}
        for field in fields[2:]:
            if ":" in field:
                key, value = field.split(":", 1)
                attributes[key] = value
        try:
            transport_id = int(attributes["transport_id"])
        except (KeyError, ValueError):
            continue

        devices.append(
            AdbDevice(
                transport_id=transport_id,
                serial=fields[0],
                usb=attributes.get("usb", "unknown"),
            )
        )
    return sorted(devices, key=lambda item: (item.usb, item.transport_id))


def parse_peer_status(output: str) -> PeerStatus | None:
    """Extract only the fields needed by the identity monitor."""

    clean_output = ANSI_RE.sub("", output).replace("\r", "")
    match = STATUS_RE.search(clean_output)
    if not match:
        return None
    values = match.groupdict()
    return PeerStatus(
        local_id=values["local"].lower(),
        seen=int(values["seen"]),
        leave=int(values["leave"]),
    )


class RecognitionGate:
    """Turn Reader counters into one event per physical encounter.

    While a board is held against the other one, firmware can increment
    ``seen`` repeatedly.  The first increment fires and all later increments
    remain suppressed.  An explicit leave or a continuous period of valid,
    unchanged Reader samples rearms the gate.
    """

    def __init__(self, rearm_seconds: float = DEFAULT_REARM_SECONDS) -> None:
        self.rearm_seconds = max(0.05, rearm_seconds)
        self.previous_seen: int | None = None
        self.previous_leave: int | None = None
        self.quiet_since: float | None = None
        self.armed = True

    def mark_unavailable(self) -> None:
        """Break a quiet run so an ADB outage cannot rearm the popup."""

        self.quiet_since = None

    def observe(self, status: PeerStatus, now: float) -> bool:
        if self.previous_seen is None or self.previous_leave is None:
            self.previous_seen = status.seen
            self.previous_leave = status.leave
            self.quiet_since = now
            return False

        if (status.seen < self.previous_seen or
                status.leave < self.previous_leave):
            # Firmware restarted and reset its counters.  Establish a clean
            # baseline rather than treating the old-to-new value as an event.
            self.previous_seen = status.seen
            self.previous_leave = status.leave
            self.quiet_since = now
            self.armed = True
            return False

        seen_increased = status.seen > self.previous_seen
        leave_increased = status.leave > self.previous_leave

        if leave_increased:
            self.armed = True
        elif not seen_increased:
            if self.quiet_since is None:
                self.quiet_since = now
            elif now - self.quiet_since >= self.rearm_seconds:
                self.armed = True

        should_notify = seen_increased and self.armed
        if seen_increased:
            self.quiet_since = None
            if should_notify:
                self.armed = False

        self.previous_seen = status.seen
        self.previous_leave = status.leave
        return should_notify


class ReaderLocator:
    """Remember the Reader's USB path while allowing transport IDs to change."""

    def __init__(self) -> None:
        self.reader_usb: str | None = None
        self.identity_by_usb: dict[str, str] = {}

    @staticmethod
    def _usb_key(device: AdbDevice) -> str | None:
        usb = device.usb.strip()
        return usb if usb and usb != "unknown" else None

    def remember(self, device: AdbDevice, local_id: str) -> None:
        usb = self._usb_key(device)
        if usb is None:
            return
        self.identity_by_usb[usb] = local_id
        if local_id == READER_ID:
            self.reader_usb = usb
        elif usb == self.reader_usb:
            # A cable/port swap can put the other duplicate-serial board on
            # the former Reader path.  Do not keep trusting stale placement.
            self.reader_usb = None

    def candidates(self, devices: Sequence[AdbDevice]) -> list[AdbDevice]:
        live_usb = {
            usb for device in devices
            if (usb := self._usb_key(device)) is not None
        }
        for usb in tuple(self.identity_by_usb):
            if usb not in live_usb:
                self.identity_by_usb.pop(usb, None)

        def priority(device: AdbDevice) -> tuple[int, str, int]:
            usb = self._usb_key(device)
            if usb is not None and usb == self.reader_usb:
                rank = 0
            elif usb is None or usb not in self.identity_by_usb:
                rank = 1
            else:
                rank = 2
            return rank, device.usb, device.transport_id

        return sorted(devices, key=priority)


class AdbClient:
    def __init__(self, adb: str = "adb") -> None:
        self.adb = adb

    def _run(self, arguments: Sequence[str], timeout: float) -> str | None:
        environment = os.environ.copy()
        # The two boards intentionally expose the same ADB serial number.
        # The legacy transport backend keeps `adb -t <transport_id>` stable
        # for this setup; make the monitor self-contained instead of relying
        # on the launching terminal to export it.
        environment["ADB_LIBUSB"] = "0"
        try:
            process = subprocess.Popen(
                [self.adb, *arguments],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                start_new_session=True,
                env=environment,
            )
            try:
                output, _ = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                # adb can leave a shell child behind while the USB transport
                # is recovering. Kill the whole process group so the next
                # sample never waits behind an orphaned request.
                try:
                    if os.name == "nt":
                        process.kill()
                    else:
                        os.killpg(process.pid, signal.SIGKILL)
                except (OSError, ProcessLookupError):
                    pass
                process.communicate()
                return None
        except OSError:
            return None
        return output if process.returncode == 0 else None

    def discover(self) -> list[AdbDevice]:
        output = self._run(
            ["devices", "-l"], timeout=ADB_DISCOVERY_TIMEOUT_SECONDS
        )
        return parse_adb_devices(output) if output is not None else []

    def query_peer(self, device: AdbDevice) -> PeerStatus | None:
        arguments = [
            "-t", str(device.transport_id), "shell",
            "v2hw nfc peer status",
        ]
        for _ in range(2):
            output = self._run(
                arguments, timeout=ADB_STATUS_TIMEOUT_SECONDS
            )
            if output is None:
                # A transport can miss one short deadline while USB is busy.
                # Retry immediately before falling back to rediscovery.
                continue
            status = parse_peer_status(output)
            if status is not None:
                return status
            # Firmware logs can occasionally split or displace one shell
            # status line.  Retry the same Reader transport immediately; this
            # is deliberately invisible to the UI.
        return None


class MonitorWorker(threading.Thread):
    """Find the fixed Reader, then poll that transport and no other board."""

    def __init__(
        self,
        client: AdbClient,
        gate: RecognitionGate,
        on_success: Callable[[], None],
        poll_seconds: float = DEFAULT_POLL_SECONDS,
    ) -> None:
        super().__init__(name="nfc-identity-monitor", daemon=True)
        self.client = client
        self.gate = gate
        self.on_success = on_success
        self.poll_seconds = max(0.01, poll_seconds)
        self.locator = ReaderLocator()
        self.reader: AdbDevice | None = None
        self.next_discovery_at = 0.0
        self.stop_event = threading.Event()

    def _observe_reader(self, status: PeerStatus, now: float) -> None:
        if self.gate.observe(status, now):
            self.on_success()

    def _locate_reader(self, now: float) -> None:
        self.next_discovery_at = now + DISCOVERY_RETRY_SECONDS
        for device in self.locator.candidates(self.client.discover()):
            status = self.client.query_peer(device)
            if status is None:
                continue
            self.locator.remember(device, status.local_id)
            if status.local_id == READER_ID:
                self.reader = device
                self._observe_reader(status, now)
                return
        self.gate.mark_unavailable()

    def poll_once(self, now: float | None = None) -> None:
        sample_time = time.monotonic() if now is None else now
        if self.reader is None:
            if sample_time >= self.next_discovery_at:
                self._locate_reader(sample_time)
            return

        status = self.client.query_peer(self.reader)
        if status is None or status.local_id != READER_ID:
            if status is not None:
                self.locator.remember(self.reader, status.local_id)
            self.reader = None
            self.next_discovery_at = 0.0
            self.gate.mark_unavailable()
            return

        self.locator.remember(self.reader, status.local_id)
        self._observe_reader(status, sample_time)

    def run(self) -> None:
        while not self.stop_event.is_set():
            started = time.monotonic()
            try:
                self.poll_once(started)
            except Exception:
                # ADB server/USB transitions are expected.  Keep the UI quiet
                # and force transport rediscovery on the next cycle.
                self.reader = None
                self.next_discovery_at = 0.0
                self.gate.mark_unavailable()
            elapsed = time.monotonic() - started
            self.stop_event.wait(max(0.01, self.poll_seconds - elapsed))

    def stop(self) -> None:
        self.stop_event.set()


class IdentityMonitorApp:
    """Small Tk window plus a large, short-lived native success popup."""

    def __init__(
        self,
        root: object,
        events: queue.SimpleQueue[str],
        worker: MonitorWorker,
    ) -> None:
        import tkinter as tk

        self.tk = tk
        self.root = root
        self.events = events
        self.worker = worker
        self.popup: object | None = None
        self.popup_timer: str | None = None

        root.title("NFC 身份识别")
        root.configure(bg="#08111f")
        root.geometry("680x260")
        root.minsize(560, 220)

        tk.Label(
            root,
            text="NFC 身份识别",
            bg="#08111f",
            fg="#94a3b8",
            font=("Noto Sans CJK SC", 16),
        ).pack(pady=(38, 18))
        self.status_label = tk.Label(
            root,
            text="等待识别…",
            bg="#08111f",
            fg="#f8fafc",
            font=("Noto Sans CJK SC", 30, "bold"),
        )
        self.status_label.pack(expand=True, padx=24, pady=(0, 42))

        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(15, self._drain_events)

    def _drain_events(self) -> None:
        recognized = False
        while True:
            try:
                self.events.get_nowait()
            except queue.Empty:
                break
            recognized = True
        if recognized:
            self.show_success()
        self.root.after(15, self._drain_events)

    def show_success(self) -> None:
        tk = self.tk
        self.status_label.configure(text=SUCCESS_TEXT, fg="#34d399")

        if self.popup is None or not self.popup.winfo_exists():
            popup = tk.Toplevel(self.root)
            popup.title("识别成功")
            popup.configure(bg="#059669")
            popup.attributes("-topmost", True)
            popup.resizable(False, False)
            tk.Label(
                popup,
                text=SUCCESS_TEXT,
                bg="#059669",
                fg="white",
                font=("Noto Sans CJK SC", 36, "bold"),
                wraplength=820,
                justify="center",
            ).pack(fill="both", expand=True, padx=42, pady=52)
            self.popup = popup
        else:
            self.popup.deiconify()

        self.popup.update_idletasks()
        width, height = 900, 300
        x = max(0, (self.popup.winfo_screenwidth() - width) // 2)
        y = max(0, (self.popup.winfo_screenheight() - height) // 2)
        self.popup.geometry(f"{width}x{height}+{x}+{y}")
        self.popup.lift()

        if self.popup_timer is not None:
            self.root.after_cancel(self.popup_timer)
        self.popup_timer = self.root.after(
            SUCCESS_POPUP_MILLISECONDS, self._hide_success
        )

    def _hide_success(self) -> None:
        self.popup_timer = None
        if self.popup is not None and self.popup.winfo_exists():
            self.popup.destroy()
        self.popup = None
        self.status_label.configure(text="等待识别…", fg="#f8fafc")

    def close(self) -> None:
        self.worker.stop()
        self.root.destroy()


def _milliseconds(value: str) -> float:
    milliseconds = float(value)
    if milliseconds <= 0:
        raise argparse.ArgumentTypeError("必须大于 0")
    return milliseconds


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="极简 ARCS-MINI NFC 身份识别上位机"
    )
    parser.add_argument("--adb", default="adb", help="adb 可执行文件")
    parser.add_argument(
        "--poll-ms", type=_milliseconds,
        default=DEFAULT_POLL_SECONDS * 1000,
        help="Reader 轮询间隔（默认 50ms）",
    )
    parser.add_argument(
        "--rearm-ms", type=_milliseconds,
        default=DEFAULT_REARM_SECONDS * 1000,
        help="无新 seen 后重新布防的时间（默认 1000ms）",
    )
    args = parser.parse_args(argv)

    import tkinter as tk

    events: queue.SimpleQueue[str] = queue.SimpleQueue()
    gate = RecognitionGate(rearm_seconds=args.rearm_ms / 1000.0)
    worker = MonitorWorker(
        AdbClient(args.adb),
        gate,
        lambda: events.put(SUCCESS_TEXT),
        poll_seconds=args.poll_ms / 1000.0,
    )
    root = tk.Tk()
    IdentityMonitorApp(root, events, worker)
    worker.start()
    root.mainloop()
    worker.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
