#!/usr/bin/env python3
"""ARCS-MINI NFC peer discovery dashboard.

The two V2.1 boards expose the same ADB serial number, so this tool discovers
them by ADB transport ID on every poll instead of using ``adb -s``.  It has no
third-party dependencies: run it with Python 3 and open the printed local URL.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import threading
import time
import webbrowser
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any


STATUS_RE = re.compile(
    r"nfc peer:\s+enabled=(?P<enabled>[01])\s+"
    r"role=(?P<role>\w+)\s+local=(?P<local>[0-9a-fA-F]{16})\s+"
    r"present=(?P<present>[01])\s+peer=(?P<peer>none|[0-9a-fA-F]{16})\s+"
    r"age=(?P<age>n/a|\d+)\s+seen=(?P<seen>\d+)\s+"
    r"enter=(?P<enter>\d+)\s+leave=(?P<leave>\d+)\s+"
    r"advertised=(?P<advertised>\d+)\s+last_error=(?P<last_error>-?\d+)"
)
CARD_SCAN_MACHINE_RE = re.compile(r"NFC_CARD_SCAN\s+([^\r\n]+)")
CHIP_ID_RE = re.compile(r"Chip ID:\s*(?P<id>[0-9a-fA-F]{16})")
ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")

KNOWN_BOARDS = {
    "6b3bc01b1634cfbd": ("狗子A · 绿色板 1634", "green", "狗子A"),
    "6b3bc01b1d41cfbd": ("狗子B · 蓝色板 1d41", "blue", "狗子B"),
}

FIXED_ROLES = {
    "6b3bc01b1634cfbd": "reader",
    "6b3bc01b1d41cfbd": "target",
}
PEER_READER_ID = "6b3bc01b1634cfbd"
PEER_TARGET_ID = "6b3bc01b1d41cfbd"

CARD_TARGETS = {
    "card_a": {"name": "CARD-A", "uid": "08A10001"},
    "card_b": {"name": "CARD-B", "uid": "08B20002"},
}

# ADB briefly drops duplicate-serial devices while transports are refreshed.
# Keep the last valid status visible through those short gaps instead of
# reporting both boards offline immediately.
ONLINE_GRACE_SECONDS = 3.5
ADB_DISCOVERY_TIMEOUT_SECONDS = 0.8
ADB_RUNTIME_STATUS_TIMEOUT_SECONDS = 0.65
ADB_SETUP_STATUS_TIMEOUT_SECONDS = 1.5
DEFAULT_POLL_INTERVAL_SECONDS = 0.25
PEER_UI_REARM_AGE_MS = 1000
ADB_COMMAND_LOCK = threading.Lock()


@dataclass(frozen=True)
class AdbDevice:
    transport_id: int
    serial: str
    usb: str


def run_command(argv: list[str], timeout: float) -> str:
    # Both boards intentionally expose the same ADB serial.  Serializing all
    # host-side ADB commands avoids two simultaneous `adb -t` requests racing
    # the same server/transport table.
    with ADB_COMMAND_LOCK:
        completed = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
    return completed.stdout


def run_timed_command(
    argv: list[str], timeout: float
) -> tuple[str, float, int]:
    """Run one exclusive ADB operation and measure its actual command time."""
    with ADB_COMMAND_LOCK:
        started = time.monotonic()
        completed = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
        elapsed_ms = (time.monotonic() - started) * 1000.0
    return completed.stdout, elapsed_ms, completed.returncode


def discover_devices(
    adb: str, timeout: float = ADB_DISCOVERY_TIMEOUT_SECONDS
) -> list[AdbDevice]:
    output = run_command([adb, "devices", "-l"], timeout=timeout)
    devices: list[AdbDevice] = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("List of devices"):
            continue
        fields = line.split()
        if len(fields) < 2 or fields[1] != "device":
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
    return sorted(devices, key=lambda item: item.usb)


def query_device(
    adb: str,
    device: AdbDevice,
    timeout: float = ADB_RUNTIME_STATUS_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    base: dict[str, Any] = {
        "online": False,
        "transport_id": device.transport_id,
        "serial": device.serial,
        "usb": device.usb,
    }
    try:
        output = run_command(
            [adb, "-t", str(device.transport_id), "shell",
             "v2hw nfc peer status"],
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        base.update(online=False, error=str(error))
        return base

    clean_output = ANSI_RE.sub("", output).replace("\r", "")
    match = STATUS_RE.search(clean_output)
    if not match:
        if ("nfc peer: status=-2" not in clean_output and
                "v2hw command failed: -2" not in clean_output):
            base.update(error="未收到 NFC peer 状态", raw=clean_output[-500:])
            return base
        # A newly manufactured board has a valid PN532/card-read path before
        # its chip ID is assigned a fixed peer role. In that state the peer
        # status command returns -2, so fall back to the generic chip-ID
        # command and keep the board selectable in card timing mode.
        try:
            id_output = run_command(
                [adb, "-t", str(device.transport_id), "shell",
                 "device get_device_id"],
                timeout=timeout,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            base.update(error=str(error), raw=clean_output[-500:])
            return base
        clean_id_output = ANSI_RE.sub("", id_output).replace("\r", "")
        id_match = CHIP_ID_RE.search(clean_id_output)
        if not id_match:
            base.update(error="未收到开发板芯片 ID", raw=clean_output[-500:])
            return base

        local_id = id_match.group("id").lower()
        label, color, dog_name = KNOWN_BOARDS.get(
            local_id,
            (f"新板 {local_id[-4:]} · 卡片测速", "neutral",
             f"狗子 {local_id[-4:]}"),
        )
        base.update(
            online=True,
            label=label,
            color=color,
            dog_name=dog_name,
            enabled=False,
            role="idle",
            configured_role="idle",
            local_id=local_id,
            present=False,
            peer_id=None,
            age_ms=None,
            seen=0,
            enter=0,
            leave=0,
            advertised=0,
            last_error=0,
            peer_available=False,
        )
        return base

    values = match.groupdict()
    local_id = values["local"].lower()
    label, color, dog_name = KNOWN_BOARDS.get(
        local_id,
        (f"开发板 {local_id[-4:]}", "neutral", f"狗子 {local_id[-4:]}"),
    )
    base.update(
        online=True,
        label=label,
        color=color,
        dog_name=dog_name,
        enabled=values["enabled"] == "1",
        role=values["role"],
        configured_role=FIXED_ROLES.get(local_id, values["role"]),
        local_id=local_id,
        present=values["present"] == "1",
        peer_id=None if values["peer"] == "none" else values["peer"].lower(),
        age_ms=None if values["age"] == "n/a" else int(values["age"]),
        seen=int(values["seen"]),
        enter=int(values["enter"]),
        leave=int(values["leave"]),
        advertised=int(values["advertised"]),
        last_error=int(values["last_error"]),
        peer_available=True,
    )
    return base


class StateStore:
    def __init__(self, online_grace: float = ONLINE_GRACE_SECONDS) -> None:
        self.lock = threading.Lock()
        self.online_grace = max(3.0, online_grace)
        self.snapshot: dict[str, Any] = {
            "timestamp": time.time(), "devices": [], "events": [],
            "poll_error": None,
        }
        self.previous: dict[str, dict[str, Any]] = {}
        self.last_good: dict[str, dict[str, Any]] = {}
        self.local_by_usb: dict[str, str] = {}
        self.failure_since: dict[str, float] = {}
        self.events: list[dict[str, Any]] = []
        self.event_sequence = 0

    @staticmethod
    def _identity(local_id: str) -> dict[str, str]:
        label, color, dog_name = KNOWN_BOARDS.get(
            local_id,
            (f"开发板 {local_id[-4:]}", "neutral", f"狗子 {local_id[-4:]}"),
        )
        return {
            "local_id": local_id,
            "label": label,
            "color": color,
            "dog_name": dog_name,
        }

    def _infer_peer_id(self, local_id: str, peer_id: str | None) -> str | None:
        if peer_id:
            return peer_id
        # The two production boards have fixed, known 64-bit identities.  An
        # enter counter can be observed after the firmware has already cleared
        # `peer=none`; preserve the successful pair instead of losing it.
        if local_id in KNOWN_BOARDS:
            return next(
                (candidate for candidate in KNOWN_BOARDS if candidate != local_id),
                None,
            )
        return next(
            (candidate for candidate in self.last_good if candidate != local_id),
            None,
        )

    def _append_event(
        self,
        *,
        now: float,
        event_type: str,
        device: dict[str, Any],
        peer_id: str | None,
        count: int,
    ) -> None:
        local_id = device["local_id"]
        resolved_peer_id = self._infer_peer_id(local_id, peer_id)
        identities = [self._identity(local_id)]
        if resolved_peer_id and resolved_peer_id != local_id:
            identities.append(self._identity(resolved_peer_id))
        self.event_sequence += 1
        self.events.insert(0, {
            "event_id": self.event_sequence,
            "time": now,
            "type": event_type,
            "board": device.get("label", local_id[-4:]),
            "local_id": local_id,
            "peer_id": resolved_peer_id,
            "identities": identities,
            "count": count,
        })

    def update(
        self,
        devices: list[dict[str, Any]],
        poll_error: str | None = None,
    ) -> None:
        now = time.time()
        with self.lock:
            displayed: dict[str, dict[str, Any]] = {}
            failures_by_local: dict[str, dict[str, Any]] = {}
            anonymous_failures: list[dict[str, Any]] = []

            for raw_device in devices:
                device = dict(raw_device)
                local_id = device.get("local_id")
                if device.get("online") and local_id:
                    device.update(stale=False, stale_ms=0)
                    previous = self.previous.get(local_id)
                    if previous:
                        enter_delta = (
                            device.get("enter", 0) - previous.get("enter", 0)
                        )
                        leave_delta = (
                            device.get("leave", 0) - previous.get("leave", 0)
                        )
                        if enter_delta > 0:
                            self._append_event(
                                now=now,
                                event_type="enter",
                                device=device,
                                peer_id=device.get("peer_id"),
                                count=enter_delta,
                            )
                        if leave_delta > 0:
                            self._append_event(
                                now=now,
                                event_type="leave",
                                device=device,
                                peer_id=previous.get("peer_id"),
                                count=leave_delta,
                            )
                    self.previous[local_id] = dict(device)
                    self.last_good[local_id] = dict(device)
                    usb = device.get("usb")
                    if usb:
                        self.local_by_usb[usb] = local_id
                    self.failure_since.pop(local_id, None)
                    displayed[local_id] = device
                    continue

                mapped_local = local_id
                if not mapped_local and device.get("usb"):
                    mapped_local = self.local_by_usb.get(device["usb"])
                if mapped_local:
                    failures_by_local[mapped_local] = device
                else:
                    anonymous_failures.append(device)

            # Retain every previously identified board.  A first failed poll
            # starts a >=3 second grace period; success clears the timer.
            for local_id, last_good in self.last_good.items():
                if local_id in displayed:
                    continue
                failed = failures_by_local.get(local_id, {})
                failed_at = self.failure_since.setdefault(local_id, now)
                stale_seconds = max(0.0, now - failed_at)
                retained = dict(last_good)
                for key in ("transport_id", "serial", "usb"):
                    if failed.get(key) is not None:
                        retained[key] = failed[key]
                retained.update(
                    online=stale_seconds <= self.online_grace,
                    stale=True,
                    stale_ms=int(stale_seconds * 1000),
                    # Both boards intentionally use the same ADB serial.  One
                    # transport query can lose a race while USB and the last
                    # good sample are still valid.  Keep that grace-period
                    # sample quiet; show red only after a sustained failure.
                    error=(
                        None
                        if stale_seconds <= self.online_grace
                        else failed.get("error", "ADB 设备超时未恢复")
                    ),
                )
                displayed[local_id] = retained

            output_devices = sorted(
                displayed.values(), key=lambda item: item.get("usb", "")
            )
            # Before a board has ever returned its ID, retain actionable ADB
            # errors.  Once known boards exist, global/anonymous failures are
            # exposed via poll_error without creating a fake third card.
            if not output_devices:
                output_devices.extend(anonymous_failures)

            self.events = self.events[:30]
            self.snapshot = {
                "timestamp": now,
                "devices": output_devices,
                "events": list(self.events),
                "poll_error": poll_error,
            }

    def get(self) -> dict[str, Any]:
        with self.lock:
            return dict(self.snapshot)

    def order_devices_for_poll(
        self, devices: list[AdbDevice]
    ) -> list[AdbDevice]:
        """Put the known Reader first without parallel duplicate-serial ADB."""
        with self.lock:
            local_by_usb = dict(self.local_by_usb)

        def priority(device: AdbDevice) -> tuple[int, str]:
            local_id = local_by_usb.get(device.usb)
            if local_id == PEER_READER_ID:
                rank = 0
            elif local_id == PEER_TARGET_ID:
                rank = 1
            else:
                rank = 2
            return rank, device.usb

        return sorted(devices, key=priority)


class Poller(threading.Thread):
    def __init__(
        self,
        adb: str,
        interval: float,
        store: StateStore,
        pause_provider: Any = None,
    ) -> None:
        super().__init__(name="nfc-dashboard-poller", daemon=True)
        self.adb = adb
        self.interval = interval
        self.store = store
        self.pause_provider = pause_provider
        self.stop_event = threading.Event()

    def poll_once(self) -> None:
        discovered = self.store.order_devices_for_poll(
            discover_devices(self.adb)
        )
        if not discovered:
            self.store.update([])
            return

        statuses: list[dict[str, Any]] = []
        # Keep duplicate-serial ADB commands serialized, but publish each
        # result immediately. A slow Target query must not hold a successful
        # Reader identity sample hostage until the entire round finishes.
        for device in discovered:
            try:
                status = query_device(self.adb, device)
            except Exception as error:  # keep dashboard alive
                status = {
                    "online": False,
                    "transport_id": device.transport_id,
                    "serial": device.serial,
                    "usb": device.usb,
                    "error": str(error),
                }
            statuses.append(status)
            self.store.update(list(statuses))

    def run(self) -> None:
        while not self.stop_event.is_set():
            # Card timing owns one PN532 and repeatedly runs a bounded shell
            # command.  Pause peer polling entirely so duplicate-serial ADB
            # transports cannot steal its timing window or inflate latency.
            if self.pause_provider and self.pause_provider():
                self.stop_event.wait(self.interval)
                continue
            started = time.monotonic()
            try:
                self.poll_once()
            except Exception as error:  # adb missing or server temporarily down
                self.store.update([], poll_error=str(error))
            elapsed = time.monotonic() - started
            self.stop_event.wait(max(0.05, self.interval - elapsed))


class CardTestController:
    """Continuously time known NFC cards using one selected ADB transport."""

    def __init__(
        self,
        adb: str,
        store: StateStore,
        scan_timeout_ms: int = 1000,
    ) -> None:
        self.adb = adb
        self.store = store
        self.scan_timeout_ms = min(5000, max(100, scan_timeout_ms))
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.worker: threading.Thread | None = None
        self.state = self._new_state()

    @staticmethod
    def _empty_target_stats() -> dict[str, Any]:
        return {
            "success_count": 0,
            "timeout_count": 0,
            "mismatch_count": 0,
            "first_ms": None,
            "latest_ms": None,
            "best_ms": None,
        }

    def _new_state(self) -> dict[str, Any]:
        return {
            "active": False,
            "phase": "stopped",
            "selected_local_id": None,
            "selected_label": None,
            "selected_usb": None,
            "target_key": "card_a",
            "attempt_started": None,
            "last_uid": None,
            "last_card": None,
            "last_error": None,
            "error_count": 0,
            "success_sequence": 0,
            "last_success_time": None,
            "last_host_ms": None,
            "targets": {
                key: self._empty_target_stats() for key in CARD_TARGETS
            },
        }

    @staticmethod
    def _parse_machine_sample(output: str) -> dict[str, str] | None:
        clean_output = ANSI_RE.sub("", output).replace("\r", "")
        samples: list[dict[str, str]] = []
        for match in CARD_SCAN_MACHINE_RE.finditer(clean_output):
            fields: dict[str, str] = {}
            for token in match.group(1).split():
                if "=" in token:
                    key, value = token.split("=", 1)
                    fields[key] = value
            if fields:
                samples.append(fields)
        return next(
            (sample for sample in reversed(samples)
             if sample.get("event") != "armed"),
            samples[-1] if samples else None,
        )

    def get(self) -> dict[str, Any]:
        with self.lock:
            snapshot = dict(self.state)
            snapshot["targets"] = {
                key: dict(value)
                for key, value in self.state["targets"].items()
            }
        target = CARD_TARGETS[snapshot["target_key"]]
        snapshot["target_name"] = target["name"]
        snapshot["target_uid"] = target["uid"]
        snapshot["scan_timeout_ms"] = self.scan_timeout_ms
        return snapshot

    def is_active(self) -> bool:
        with self.lock:
            return bool(self.state["active"])

    def _selected_device(self) -> dict[str, Any] | None:
        with self.lock:
            local_id = self.state["selected_local_id"]
        if not local_id:
            return None
        devices = self.store.get().get("devices", [])
        return next(
            (
                device for device in devices
                if device.get("local_id") == local_id
                and device.get("online")
                and not device.get("stale")
                and device.get("transport_id") is not None
            ),
            None,
        )

    def _set_error(self, message: str, *, stop: bool = False) -> None:
        with self.lock:
            self.state["phase"] = "error"
            self.state["last_error"] = message
            self.state["error_count"] += 1

    def start(self, local_id: str, target_key: str) -> dict[str, Any]:
        if target_key not in CARD_TARGETS:
            raise ValueError("未知卡片目标")
        if not self.stop(wait_seconds=10.0):
            raise RuntimeError("上一次卡片测速尚未停止")

        device = next(
            (
                item for item in self.store.get().get("devices", [])
                if item.get("local_id") == local_id and item.get("online")
            ),
            None,
        )
        if not device:
            raise ValueError("选中的开发板当前不在线")

        state = self._new_state()
        state.update(
            active=True,
            phase="starting",
            selected_local_id=local_id,
            selected_label=device.get("label", local_id[-4:]),
            selected_usb=device.get("usb"),
            target_key=target_key,
        )
        with self.lock:
            self.state = state
            self.stop_event = threading.Event()
            self.worker = threading.Thread(
                target=self._run,
                name="nfc-card-timing",
                daemon=True,
            )
            self.worker.start()
        return self.get()

    def select_target(self, target_key: str) -> dict[str, Any]:
        if target_key not in CARD_TARGETS:
            raise ValueError("未知卡片目标")
        with self.lock:
            self.state["target_key"] = target_key
            self.state["phase"] = (
                "scanning" if self.state["active"] else "stopped"
            )
            self.state["attempt_started"] = (
                time.time() if self.state["active"] else None
            )
            self.state["last_uid"] = None
            self.state["last_card"] = None
            self.state["last_error"] = None
        return self.get()

    def reset_stats(self) -> dict[str, Any]:
        with self.lock:
            self.state["targets"] = {
                key: self._empty_target_stats() for key in CARD_TARGETS
            }
            self.state["last_uid"] = None
            self.state["last_card"] = None
            self.state["last_error"] = None
            self.state["error_count"] = 0
            self.state["last_host_ms"] = None
            self.state["last_success_time"] = None
        return self.get()

    def stop(self, wait_seconds: float = 9.0) -> bool:
        with self.lock:
            worker = self.worker
            stop_event = self.stop_event
        if not worker or not worker.is_alive():
            with self.lock:
                self.state["active"] = False
                if self.state["phase"] != "error":
                    self.state["phase"] = "stopped"


            return True
        stop_event.set()
        if worker is not threading.current_thread():
            worker.join(timeout=wait_seconds)
        return not worker.is_alive()

    def _adb_shell(
        self, transport_id: int, command: str, timeout: float
    ) -> tuple[str, float, int]:
        return run_timed_command(
            [self.adb, "-t", str(transport_id), "shell", command],
            timeout=timeout,
        )

    def _run(self) -> None:
        restore_transport: int | None = None
        peer_restore_needed = False
        try:
            device = self._selected_device()
            if not device:
                self._set_error("读卡器已离线，无法开始测速", stop=True)
                return
            restore_transport = int(device["transport_id"])
            # Preserve the state that existed before timing. Unknown new
            # boards and an explicitly stopped peer service need no stop or
            # restore. If an enabled peer's stop command times out after
            # reaching the board, the original intent still requires restore.
            if device.get("peer_available") and device.get("enabled"):
                peer_restore_needed = True
                output, _, _ = self._adb_shell(
                    restore_transport, "v2hw nfc peer stop", timeout=8.5
                )
                clean_output = ANSI_RE.sub("", output)
                if "nfc peer discovery stopped" not in clean_output:
                    self._set_error(
                        "无法停止双板互认服务，PN532 未交给卡片测速模式",
                        stop=True,
                    )
                    return

            with self.lock:
                self.state["phase"] = "scanning"
                self.state["attempt_started"] = time.time()
                self.state["last_error"] = None

            while not self.stop_event.is_set():
                device = self._selected_device()
                if not device:
                    self._set_error("读卡器 ADB 暂时离线，正在等待恢复")
                    self.stop_event.wait(0.5)
                    continue
                transport_id = int(device["transport_id"])
                restore_transport = transport_id
                with self.lock:
                    self.state["attempt_started"] = time.time()
                    sample_target_key = self.state["target_key"]

                try:
                    output, host_ms, _ = self._adb_shell(
                        transport_id,
                        f"v2hw nfc card-read {self.scan_timeout_ms}",
                        timeout=self.scan_timeout_ms / 1000.0 + 4.0,
                    )
                except subprocess.TimeoutExpired:
                    self._set_error("ADB 等待读卡命令超时")
                    self.stop_event.wait(0.5)
                    continue
                except OSError as error:
                    self._set_error(f"无法执行 ADB：{error}")
                    self.stop_event.wait(0.5)
                    continue

                sample = self._parse_machine_sample(output)
                if not sample:
                    if "card-read" not in output and "v2hw" in output:
                        self._set_error(
                            "当前固件不支持 v2hw nfc card-read，请先烧录最新固件",
                            stop=True,
                        )
                        return
                    self._set_error("读卡命令未返回 NFC_CARD_SCAN 结果")
                    self.stop_event.wait(0.5)
                    continue

                event = sample.get("event")
                try:
                    device_ms = int(sample.get("elapsed_ms", "0"))
                except ValueError:
                    device_ms = 0
                uid = sample.get("uid", "NONE").upper()
                card_name = sample.get("card", "UNKNOWN")

                with self.lock:
                    # A target button can be pressed while the bounded board
                    # command is in flight.  Discard that old target's result;
                    # the next scan uses the newly selected UID.
                    if self.state["target_key"] != sample_target_key:
                        continue
                    target_key = self.state["target_key"]
                    target = CARD_TARGETS[target_key]
                    stats = self.state["targets"][target_key]
                    phase = self.state["phase"]
                    self.state["last_host_ms"] = round(host_ms, 1)
                    self.state["last_error"] = None

                    if event == "found":
                        previous_uid = self.state.get("last_uid")
                        self.state["last_uid"] = uid
                        self.state["last_card"] = card_name
                        if uid == target["uid"]:
                            if phase != "remove_card":
                                stats["success_count"] += 1
                                stats["latest_ms"] = device_ms
                                if stats["first_ms"] is None:
                                    stats["first_ms"] = device_ms
                                if (stats["best_ms"] is None or
                                        device_ms < stats["best_ms"]):
                                    stats["best_ms"] = device_ms
                                self.state["success_sequence"] += 1
                                self.state["last_success_time"] = time.time()
                            self.state["phase"] = "remove_card"
                        else:
                            if phase != "wrong_card" or previous_uid != uid:
                                stats["mismatch_count"] += 1
                            self.state["phase"] = "wrong_card"
                    elif event == "timeout":
                        self.state["last_uid"] = None
                        self.state["last_card"] = None
                        if phase == "remove_card" or phase == "wrong_card":
                            self.state["phase"] = "scanning"
                        else:
                            stats["timeout_count"] += 1
                            self.state["phase"] = "scanning"
                    else:
                        status = sample.get("scan_status", "?")
                        restore = sample.get("restore_status", "?")
                        self.state["phase"] = "error"
                        self.state["last_error"] = (
                            f"PN532 扫描错误 {status}，服务恢复状态 {restore}"
                        )
                        self.state["error_count"] += 1

                self.stop_event.wait(0.12 if event == "found" else 0.05)
        except (OSError, subprocess.TimeoutExpired) as error:
            self._set_error(f"卡片测速启动失败：{error}", stop=True)
        finally:
            if peer_restore_needed and restore_transport is not None:
                try:
                    output, _, _ = self._adb_shell(
                        restore_transport,
                        "v2hw nfc peer start",
                        timeout=3.0,
                    )
                    if "nfc peer discovery started" not in ANSI_RE.sub(
                        "", output
                    ):
                        self._set_error("测速已停止，但双板互认服务恢复失败")
                except (OSError, subprocess.TimeoutExpired):
                    self._set_error("测速已停止，但双板互认服务恢复失败")
            with self.lock:
                self.state["active"] = False
                self.state["attempt_started"] = None
                if self.state["phase"] != "error":
                    self.state["phase"] = "stopped"


class PeerTestController:
    """Orchestrate one deterministic fixed-role A-to-B identity test."""

    def __init__(
        self,
        adb: str,
        store: StateStore,
        card_test: CardTestController,
    ) -> None:
        self.adb = adb
        self.store = store
        self.card_test = card_test
        self.lock = threading.Lock()
        self.worker: threading.Thread | None = None
        self.success_sequence = 0
        self.state: dict[str, Any] = {
            "phase": "idle",
            "setup_active": False,
            "reader_id": PEER_READER_ID,
            "target_id": PEER_TARGET_ID,
            "waiting_since": None,
            "recognized_at": None,
            "firmware_seen_at": None,
            "left_at": None,
            "latency_ms": None,
            "target_confirmed": False,
            "target_confirmed_at": None,
            "last_error": None,
            "connection_warning": None,
            "runtime_warning": None,
            "baseline_reader_enter": 0,
            "baseline_reader_seen": 0,
            "baseline_reader_leave": 0,
            "baseline_target_enter": 0,
            "baseline_target_seen": 0,
            "baseline_target_leave": 0,
            "success_sequence": 0,
        }

    def is_setup_active(self) -> bool:
        with self.lock:
            return bool(self.state["setup_active"])

    @staticmethod
    def _find_board(
        devices: list[dict[str, Any]],
        local_id: str,
        *,
        allow_stale: bool = False,
    ) -> dict[str, Any] | None:
        return next(
            (
                item for item in devices
                if item.get("local_id") == local_id
                and item.get("online")
                and (allow_stale or not item.get("stale"))
                and item.get("transport_id") is not None
            ),
            None,
        )

    @staticmethod
    def _adb_device(device: dict[str, Any]) -> AdbDevice:
        return AdbDevice(
            transport_id=int(device["transport_id"]),
            serial=str(device.get("serial", "")),
            usb=str(device.get("usb", "unknown")),
        )

    def _shell(
        self, device: dict[str, Any], command: str, timeout: float
    ) -> str:
        output, _, _ = run_timed_command(
            [
                self.adb,
                "-t",
                str(device["transport_id"]),
                "shell",
                command,
            ],
            timeout=timeout,
        )
        return ANSI_RE.sub("", output).replace("\r", "")

    def _set_phase(self, phase: str, **values: Any) -> None:
        with self.lock:
            self.state["phase"] = phase
            self.state.update(values)

    def start(self) -> dict[str, Any]:
        with self.lock:
            if self.state["setup_active"]:
                raise RuntimeError("一键互认正在启动，请稍候")

        # Card timing owns PN532 exclusively. Return it first so this remains a
        # genuine one-click operation from either dashboard mode.
        if not self.card_test.stop(wait_seconds=10.0):
            raise RuntimeError("卡片测速尚未停止，无法启动双板互认")

        devices = self.store.get().get("devices", [])
        reader = self._find_board(devices, PEER_READER_ID)
        target = self._find_board(devices, PEER_TARGET_ID)
        missing: list[str] = []
        if not reader:
            missing.append("绿色板/狗子A")
        if not target:
            missing.append("蓝色板/狗子B")
        if missing:
            raise ValueError(f"未找到在线设备：{'、'.join(missing)}")

        with self.lock:
            self.state.update(
                phase="preparing",
                setup_active=True,
                waiting_since=None,
                recognized_at=None,
                firmware_seen_at=None,
                left_at=None,
                latency_ms=None,
                target_confirmed=False,
                target_confirmed_at=None,
                last_error=None,
                connection_warning=None,
                runtime_warning=None,
                success_sequence=self.success_sequence,
            )
            self.worker = threading.Thread(
                target=self._run,
                args=(dict(reader), dict(target)),
                name="nfc-peer-one-click",
                daemon=True,
            )
            self.worker.start()
        return self.get()

    def _run(
        self,
        reader: dict[str, Any],
        target: dict[str, Any],
    ) -> None:
        try:
            # Remove any old RF session first. Reader is stopped before Target
            # so no initiator field remains while Target is being reset.
            if reader.get("peer_available") and reader.get("enabled"):
                output = self._shell(
                    reader, "v2hw nfc peer stop", timeout=8.5
                )
                if "nfc peer discovery stopped" not in output:
                    raise RuntimeError("绿色 Reader 停止旧会话失败")
            if target.get("peer_available") and target.get("enabled"):
                output = self._shell(
                    target, "v2hw nfc peer stop", timeout=8.5
                )
                if "nfc peer discovery stopped" not in output:
                    raise RuntimeError("蓝色 Target 停止旧会话失败")

            reader_status = query_device(
                self.adb,
                self._adb_device(reader),
                timeout=ADB_SETUP_STATUS_TIMEOUT_SECONDS,
            )
            if not reader_status.get("online") or not reader_status.get(
                "peer_available"
            ):
                raise RuntimeError("无法读取绿色 Reader 的互认状态")
            target_status = query_device(
                self.adb,
                self._adb_device(target),
                timeout=ADB_SETUP_STATUS_TIMEOUT_SECONDS,
            )
            if not target_status.get("online") or not target_status.get(
                "peer_available"
            ):
                raise RuntimeError("无法读取蓝色 Target 的互认状态")

            self._set_phase(
                "starting_target",
                baseline_reader_enter=int(reader_status.get("enter", 0)),
                baseline_reader_seen=int(reader_status.get("seen", 0)),
                baseline_reader_leave=int(reader_status.get("leave", 0)),
                baseline_target_enter=int(target_status.get("enter", 0)),
                baseline_target_seen=int(target_status.get("seen", 0)),
                baseline_target_leave=int(target_status.get("leave", 0)),
            )
            output = self._shell(
                target, "v2hw nfc peer start", timeout=3.0
            )
            if "nfc peer discovery started" not in output:
                raise RuntimeError("蓝色 Target 启动失败")

            # Give the fixed Target worker time to enter TgInitAsTarget before
            # enabling the Reader field. No role-selection command is used.
            time.sleep(0.35)
            self._set_phase("starting_reader")
            output = self._shell(
                reader, "v2hw nfc peer start", timeout=3.0
            )
            if "nfc peer discovery started" not in output:
                raise RuntimeError("绿色 Reader 启动失败")

            self._set_phase(
                "waiting",
                setup_active=False,
                waiting_since=time.time(),
                connection_warning=None,
                runtime_warning=None,
            )
        except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
            self._set_phase(
                "error",
                setup_active=False,
                last_error=str(error),
            )

    def _observe(self, devices: list[dict[str, Any]]) -> None:
        # StateStore marks a retained last-good sample stale during the normal
        # duplicate-serial ADB grace window. It remains online and is safe for
        # observation; only an expired/offline sample should fail the test.
        reader = self._find_board(
            devices, PEER_READER_ID, allow_stale=True
        )
        target = self._find_board(
            devices, PEER_TARGET_ID, allow_stale=True
        )
        with self.lock:
            phase = self.state["phase"]
            if self.state["setup_active"] or phase not in (
                "waiting", "recognized", "left"
            ):
                return
            if not reader:
                self.state["connection_warning"] = (
                    "Reader ADB 状态短暂中断，正在自动重连"
                )
                return

            # Reader is the authoritative side for the first user-visible
            # result: its seen counter increments only after CRC and the fixed
            # peer ID have both passed. A delayed Target status must not block
            # that popup; it can confirm the same exchange on a later sample.
            self.state["connection_warning"] = (
                "Target 状态暂缺，Reader 身份识别仍在实时监测"
                if not target else None
            )

            runtime_errors = [
                f"{label} {error}"
                for label, device in (
                    (("Reader", reader), ("Target", target))
                    if target else (("Reader", reader),)
                )
                if (error := int(device.get("last_error", 0) or 0)) != 0
            ]
            if runtime_errors:
                has_io_error = any(error.endswith(" -3") for error in runtime_errors)
                suffix = (
                    "；-3 持续时需将该板 Type-C 和电池完全断开 3–5 秒"
                    if has_io_error else "；正在重试"
                )
                self.state["runtime_warning"] = (
                    "PN532 异常（" + "、".join(runtime_errors) + "）" + suffix
                )
            else:
                self.state["runtime_warning"] = None

            now = time.time()
            reader_seen = int(reader.get("seen", 0))
            reader_leave = int(reader.get("leave", 0))
            target_seen = int(target.get("seen", 0)) if target else 0
            target_leave = int(target.get("leave", 0)) if target else 0
            raw_reader_age = reader.get("age_ms")
            reader_age_ms = (
                max(0, int(raw_reader_age))
                if isinstance(raw_reader_age, (int, float)) else None
            )

            # Firmware deliberately keeps `present` for 6 seconds, so waiting
            # for enter/leave prevents a quick remove-and-retap from creating
            # another UI event. Infer a short physical gap when Reader age has
            # stopped resetting, and snapshot the latest counters to suppress
            # all repeated scans from one continuous touch.
            firmware_left = (
                reader_leave > int(self.state["baseline_reader_leave"])
                and not reader.get("present")
            )
            inferred_gap = (
                reader_age_ms is not None
                and reader_age_ms >= PEER_UI_REARM_AGE_MS
            )
            if phase == "recognized" and (firmware_left or inferred_gap):
                self.state.update(
                    phase="left",
                    left_at=now,
                    waiting_since=now,
                    target_confirmed=False,
                    target_confirmed_at=None,
                    baseline_reader_enter=int(reader.get("enter", 0)),
                    baseline_reader_seen=reader_seen,
                    baseline_reader_leave=reader_leave,
                    baseline_target_enter=(
                        int(target.get("enter", 0))
                        if target else self.state["baseline_target_enter"]
                    ),
                    baseline_target_seen=(
                        target_seen
                        if target else self.state["baseline_target_seen"]
                    ),
                    baseline_target_leave=(
                        target_leave
                        if target else self.state["baseline_target_leave"]
                    ),
                )
                phase = "left"

            reader_exchange = (
                reader_seen > int(self.state["baseline_reader_seen"])
            )
            target_exchange = bool(
                target
                and target_seen > int(self.state["baseline_target_seen"])
            )
            reader_identity_valid = bool(
                reader.get("enabled")
                and reader.get("peer_id") in (None, PEER_TARGET_ID)
            )
            target_identity_valid = bool(
                target
                and target.get("enabled")
                and target.get("peer_id") in (None, PEER_READER_ID)
            )

            if (phase in ("waiting", "left") and reader_exchange and
                    reader_identity_valid):
                # `seen` is durable proof that Reader validated the complete
                # CRC-protected identity packet. `age_ms` is the only honest
                # estimate available for board-success-to-dashboard lag;
                # time since test start mostly measures the user's waiting.
                self.success_sequence += 1
                self.state.update(
                    phase="recognized",
                    recognized_at=now,
                    firmware_seen_at=(
                        now - reader_age_ms / 1000.0
                        if reader_age_ms is not None else None
                    ),
                    latency_ms=reader_age_ms,
                    target_confirmed=(
                        target_exchange and target_identity_valid
                    ),
                    target_confirmed_at=(
                        now if target_exchange and target_identity_valid
                        else None
                    ),
                    success_sequence=self.success_sequence,
                )
                phase = "recognized"

            if (phase == "recognized" and
                    not self.state["target_confirmed"] and
                    target_exchange and target_identity_valid):
                self.state.update(
                    target_confirmed=True,
                    target_confirmed_at=now,
                )

    def get(self) -> dict[str, Any]:
        self._observe(self.store.get().get("devices", []))
        with self.lock:
            return dict(self.state)

    def wait_setup(self, timeout: float = 20.0) -> None:
        with self.lock:
            worker = self.worker
        if worker and worker.is_alive():
            worker.join(timeout=timeout)


HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ARCS-MINI NFC 互认</title>
<style>
:root{color-scheme:dark;font-family:Inter,"Noto Sans SC",system-ui,sans-serif;background:#070b14;color:#e8edf7}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 50% -20%,#1b3155 0,#0a1020 40%,#070b14 75%)}
main{width:min(1120px,94vw);margin:auto;padding:36px 0 56px}header{display:flex;justify-content:space-between;align-items:end;gap:20px;margin-bottom:24px}
h1{font-size:clamp(26px,4vw,42px);margin:0;letter-spacing:.02em}.subtitle{color:#8997ad;margin-top:8px}
.live{display:flex;align-items:center;gap:9px;color:#9fb0c8}.dot{width:10px;height:10px;border-radius:50%;background:#6b7280;box-shadow:0 0 0 5px #6b728020}.dot.ok{background:#37e68a;box-shadow:0 0 18px #37e68a}
.cards{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.card{position:relative;overflow:hidden;background:#101827dd;border:1px solid #26334a;border-radius:22px;padding:24px;min-height:338px;box-shadow:0 18px 45px #0006}
.card:before{content:"";position:absolute;inset:0 auto 0 0;width:5px;background:#718096}.card.green:before{background:#2bdc7f}.card.blue:before{background:#4d96ff}
.cardhead{display:flex;justify-content:space-between;align-items:center}.board{font-size:21px;font-weight:750}.usb{font:12px ui-monospace,monospace;color:#75839a;margin-top:5px}
.pill{padding:7px 11px;border-radius:999px;background:#202b3e;color:#aebbd0;font-size:13px}.pill.reader{background:#1f3d68;color:#80b9ff}.pill.target{background:#19412f;color:#75eca7}
.presence{text-align:center;margin:30px 0 24px}.presence strong{display:block;font-size:32px}.presence span{color:#8391a8}.present strong{color:#46ee95;text-shadow:0 0 24px #37e68a66}.waiting strong{color:#cbd5e1}
.id{font:14px ui-monospace,SFMono-Regular,monospace;letter-spacing:.06em;background:#090e19;border:1px solid #222e42;padding:12px;border-radius:11px;overflow-wrap:anywhere}.idlabel{font-size:12px;color:#748299;margin:15px 0 6px}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:18px}.stat{background:#0b111e;border-radius:11px;padding:10px;text-align:center}.stat b{display:block;font-size:19px}.stat span{font-size:11px;color:#748299}
.error{color:#ff8e8e;background:#3b1720;padding:10px;border-radius:10px;margin-top:14px}.events{margin-top:22px;background:#0d1422cc;border:1px solid #26334a;border-radius:18px;padding:20px}
.events h2{margin:0 0 13px;font-size:18px}.event{display:flex;gap:12px;align-items:center;padding:10px 0;border-top:1px solid #202b3c}.event:first-of-type{border:0}.event .time{color:#718097;font:12px ui-monospace,monospace}.enter{color:#48e896}.leave{color:#ffaf69}.empty{color:#718097;padding:12px 0}
.identity-popup{position:fixed;z-index:1000;inset:0;display:grid;place-items:center;padding:24px;background:#020712e8;backdrop-filter:blur(14px);opacity:0;visibility:hidden;transition:opacity .18s ease,visibility .18s ease}
.identity-popup.show{opacity:1;visibility:visible}.identity-dialog{width:min(820px,94vw);text-align:center;border:2px solid #4cff9a;background:linear-gradient(145deg,#102b22,#09130f 65%);border-radius:32px;padding:38px 34px 30px;box-shadow:0 0 0 9px #42ff9214,0 0 90px #26e88070;transform:scale(.82);transition:transform .22s cubic-bezier(.2,1.45,.4,1);animation:successGlow 1s ease-in-out infinite alternate}
.identity-popup.show .identity-dialog{transform:scale(1)}.success-check{width:104px;height:104px;margin:0 auto 15px;border-radius:50%;display:grid;place-items:center;font-size:72px;font-weight:900;color:#062516;background:#45f493;box-shadow:0 0 45px #45f493aa}
.identity-kicker{font-size:15px;letter-spacing:.22em;color:#72efa8}.identity-title{font-size:clamp(38px,7vw,72px);line-height:1.1;margin:9px 0 24px;color:#f3fff8;text-shadow:0 0 24px #5cff9b88}
.dog-pair{display:grid;grid-template-columns:1fr auto 1fr;gap:18px;align-items:center}.dog{padding:19px 14px;border-radius:18px;background:#08140f;border:1px solid #286a48}.dog strong{display:block;font-size:clamp(25px,4vw,42px);margin-bottom:8px}.dog code{font-size:13px;color:#9ccfb2;overflow-wrap:anywhere}.pair-arrow{font-size:44px;color:#5cff9b}.identity-time{margin-top:20px;color:#8db6a0}.identity-close{margin-top:15px;color:#709580;font-size:13px}
.hidden{display:none!important}.modebar{display:flex;gap:10px;margin:0 0 22px}.modebtn,.actionbtn,.targetbtn{border:1px solid #33435e;background:#111b2c;color:#aab8ce;border-radius:12px;padding:11px 17px;font:inherit;cursor:pointer}.modebtn.active,.targetbtn.active{background:#174d7d;border-color:#45a2ff;color:white;box-shadow:0 0 22px #258ee744}.test-panel{background:#0d1524e8;border:1px solid #293b56;border-radius:22px;padding:24px}.test-controls{display:grid;grid-template-columns:minmax(220px,1fr) auto auto;gap:12px;align-items:end}.control-label{display:block;color:#8293ab;font-size:12px;margin-bottom:7px}.reader-select{width:100%;border:1px solid #33435e;background:#080f1c;color:#e9f2ff;border-radius:11px;padding:12px;font:inherit}.actionbtn.start{background:#17643f;border-color:#30c678;color:#eafff3}.actionbtn.stop{background:#5b2730;border-color:#a64959;color:#ffe9ed}.test-targets{display:flex;gap:10px;flex-wrap:wrap;margin:22px 0}.targetbtn{min-width:180px;text-align:left}.targetbtn b{display:block}.targetbtn code{font-size:12px;color:#90a7c5}.scan-hero{text-align:center;padding:30px 18px;border-radius:20px;background:radial-gradient(circle at 50% 20%,#153d6e,#091322 70%);border:1px solid #2d4c72}.scan-kicker{color:#7e9cbe;letter-spacing:.16em;font-size:13px}.scan-target{font-size:clamp(42px,8vw,86px);font-weight:900;margin:5px 0}.scan-uid{font:clamp(18px,3vw,30px) ui-monospace,monospace;letter-spacing:.12em;color:#9fd0ff}.scan-state{font-size:clamp(21px,3vw,34px);font-weight:750;margin-top:20px}.scan-elapsed{color:#8ba2bf;margin-top:7px}.scan-hero.success{border-color:#3ee68b;box-shadow:0 0 45px #2ad57730}.scan-hero.error{border-color:#f66}.timing-grid{display:grid;grid-template-columns:repeat(5,1fr);gap:10px;margin-top:16px}.timing{background:#08101e;border-radius:13px;text-align:center;padding:15px 8px}.timing b{display:block;font-size:clamp(22px,3vw,34px)}.timing span{font-size:12px;color:#7e90a9}.test-note{color:#7789a2;font-size:13px;margin:15px 0 0}.test-error{background:#3e1720;color:#ff9aa9;border-radius:10px;padding:11px;margin-top:14px}.target-summary{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:16px}.target-summary article{background:#09111f;border:1px solid #22314a;border-radius:14px;padding:14px}.target-summary b{font-size:18px}.target-summary span{display:block;color:#8192aa;margin-top:5px}.card-success-dialog{border-color:#55c7ff;background:linear-gradient(145deg,#102a40,#07121e 65%);box-shadow:0 0 0 9px #42c8ff14,0 0 90px #27aeff70}.card-success-dialog .success-check{background:#58cbff;color:#042034}.card-success-dialog .identity-kicker{color:#8cdbff}.card-success-dialog .identity-title{text-shadow:0 0 24px #53caff88}.success-card-name{font-size:clamp(48px,10vw,96px);font-weight:950}.success-card-uid{font:clamp(20px,4vw,36px) ui-monospace,monospace;letter-spacing:.12em;color:#a9dcff}.success-latency{font-size:clamp(34px,7vw,66px);font-weight:900;color:#5bd5ff;margin-top:20px}
.peer-test-panel{display:grid;grid-template-columns:1fr auto;gap:18px;align-items:center;margin-bottom:18px;background:linear-gradient(135deg,#111d31,#0b1423);border:1px solid #2b3d59;border-radius:20px;padding:20px}.fixed-flow{display:flex;align-items:center;gap:12px;flex-wrap:wrap}.fixed-role{background:#09111f;border:1px solid #2b3b54;border-radius:13px;padding:10px 13px}.fixed-role b{display:block}.fixed-role span{font-size:12px;color:#8496af}.flow-arrow{font-size:27px;color:#70b5ff}.peer-test-status{margin-top:13px}.peer-test-status b{font-size:24px}.peer-test-status span{display:block;color:#8ca0bb;margin-top:4px}.peer-test-status.success b{color:#48ed98}.peer-test-status.error b{color:#ff858f}.peer-test-button{min-width:185px;font-weight:800;font-size:17px;padding:15px 20px}.peer-test-button:disabled{opacity:.55;cursor:wait}
@keyframes successGlow{from{box-shadow:0 0 0 9px #42ff9214,0 0 55px #26e88045}to{box-shadow:0 0 0 14px #42ff9220,0 0 110px #26e88090}}
@media(max-width:760px){header{align-items:start;flex-direction:column}.cards{grid-template-columns:1fr}.card{min-height:auto}.stats{grid-template-columns:repeat(2,1fr)}.test-controls{grid-template-columns:1fr 1fr}.test-controls>label{grid-column:1/-1}.timing-grid{grid-template-columns:repeat(2,1fr)}.target-summary{grid-template-columns:1fr}.peer-test-panel{grid-template-columns:1fr}.peer-test-button{width:100%}}
</style></head>
<body><main><header><div><h1>ARCS-MINI NFC 调试台</h1><div class="subtitle">双板身份互认与 PN532 卡片读取测速</div></div><div class="live"><i id="dot" class="dot"></i><span id="summary">正在连接…</span></div></header>
<nav class="modebar"><button id="peerModeBtn" class="modebtn active" onclick="switchMode('peer')">双板互认</button><button id="cardModeBtn" class="modebtn" onclick="switchMode('card')">卡片测速</button></nav>
<section id="peerMode"><section class="peer-test-panel"><div><div class="fixed-flow"><div class="fixed-role"><b>狗子A · 绿色板</b><span>固定 Reader · 1634</span></div><div class="flow-arrow">→</div><div class="fixed-role"><b>狗子B · 蓝色板</b><span>固定 Target · 1d41</span></div></div><div id="peerTestStatus" class="peer-test-status"><b>一键互认测试</b><span>会先启动蓝色 Target，再启动绿色 Reader</span></div></div><button id="peerTestButton" class="actionbtn start peer-test-button" onclick="startPeerTest()">一键开始互认</button></section><section id="cards" class="cards"></section><section class="events"><h2>事件记录</h2><div id="events" class="empty">暂无靠近/离开事件</div></section></section>
<section id="cardMode" class="hidden"><div class="test-panel"><div class="test-controls"><label><span class="control-label">选择读卡器开发板</span><select id="readerSelect" class="reader-select"></select></label><button class="actionbtn start" onclick="startCardTest()">开始连续测速</button><button class="actionbtn stop" onclick="stopCardTest()">停止并恢复互认</button></div><div class="test-targets"><button id="target-card_a" class="targetbtn active" onclick="selectCardTarget('card_a')"><b>CARD-A</b><code>08 A1 00 01</code></button><button id="target-card_b" class="targetbtn" onclick="selectCardTarget('card_b')"><b>CARD-B</b><code>08 B2 00 02</code></button><button class="actionbtn" onclick="resetCardStats()">清空统计</button></div><div id="scanHero" class="scan-hero"><div class="scan-kicker">当前扫描目标</div><div id="scanTarget" class="scan-target">CARD-A</div><div id="scanUid" class="scan-uid">08 A1 00 01</div><div id="scanState" class="scan-state">请选择读卡器并开始</div><div id="scanElapsed" class="scan-elapsed">单次扫描窗口 1000 ms</div></div><div class="timing-grid"><div class="timing"><b id="latestLatency">—</b><span>最新耗时</span></div><div class="timing"><b id="firstLatency">—</b><span>首次耗时</span></div><div class="timing"><b id="bestLatency">—</b><span>最快耗时</span></div><div class="timing"><b id="successCount">0</b><span>读取成功</span></div><div class="timing"><b id="timeoutCount">0</b><span>扫描超时</span></div></div><div id="cardTestError"></div><p class="test-note">读取成功后请先移开卡片，页面恢复“等待靠近”后再次靠近；同一次贴住不会重复计数。耗时来自板端 PN532 扫描计时，不包含 ADB 开销。</p><div id="targetSummary" class="target-summary"></div></div></section></main>
<div id="identityPopup" class="identity-popup" role="alertdialog" aria-live="assertive" onclick="closeIdentityPopup()"><div class="identity-dialog" onclick="event.stopPropagation()"><div class="success-check">✓</div><div class="identity-kicker">NFC IDENTITY VERIFIED</div><div id="identityTitle" class="identity-title">狗子A 已识别 狗子B</div><div id="dogPair" class="dog-pair"></div><div id="identityTime" class="identity-time"></div><div class="identity-close">点击背景关闭 · 5 秒后自动收起</div></div></div>
<div id="cardSuccessPopup" class="identity-popup" role="alertdialog" aria-live="assertive" onclick="closeCardSuccessPopup()"><div class="identity-dialog card-success-dialog" onclick="event.stopPropagation()"><div class="success-check">✓</div><div class="identity-kicker">NFC CARD READ SUCCESS</div><div class="identity-title">卡片读取成功</div><div id="successCardName" class="success-card-name">CARD-A</div><div id="successCardUid" class="success-card-uid"></div><div id="successLatency" class="success-latency"></div><div class="identity-close">点击背景关闭 · 4 秒后自动收起</div></div></div>
<script>
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const role={idle:'空闲',reader:'读卡器',target:'卡片'};
let currentMode='peer',latestDevices=[],selectedTargetKey='card_a';
function switchMode(mode){currentMode=mode;document.getElementById('peerMode').classList.toggle('hidden',mode!=='peer');document.getElementById('cardMode').classList.toggle('hidden',mode!=='card');document.getElementById('peerModeBtn').classList.toggle('active',mode==='peer');document.getElementById('cardModeBtn').classList.toggle('active',mode==='card');if(mode==='card')refreshCardTest();else refreshPeerTest()}
function card(d){const good=d.online&&d.local_id;const age=d.age_ms==null?'—':d.age_ms+' ms';const fixedRole=d.configured_role||d.role;const roleText=good?(d.enabled?(role[fixedRole]||fixedRole):'已停止'):'离线';const waitingText=d.last_error?(Number(d.last_error)===-3?'PN532 I²C 异常 -3；持续时需彻底断电 3–5 秒':`PN532 异常 ${d.last_error}，正在重试`):(fixedRole==='target'?'等待读卡器靠近':'NFC 持续扫描中');return `<article class="card ${esc(d.color||'neutral')}"><div class="cardhead"><div><div class="board">${esc(d.label||'未识别设备')}</div><div class="usb">USB ${esc(d.usb||'—')} · transport ${esc(d.transport_id||'—')}</div></div><span class="pill ${esc(fixedRole||'')}">${esc(roleText)}</span></div><div class="presence ${d.present?'present':'waiting'}"><strong>${d.present?'已识别对方':'等待靠近'}</strong><span>${d.present?'最近通信 '+age:waitingText}</span></div><div class="idlabel">本机 64 位 ID</div><div class="id">${esc(d.local_id||'—')}</div><div class="idlabel">对方 64 位 ID</div><div class="id">${esc(d.peer_id||'未检测')}</div><div class="stats"><div class="stat"><b>${esc(d.seen??0)}</b><span>交换成功</span></div><div class="stat"><b>${esc(d.enter??0)}</b><span>靠近</span></div><div class="stat"><b>${esc(d.leave??0)}</b><span>离开</span></div><div class="stat"><b>${esc(d.last_error??'—')}</b><span>最近错误</span></div></div>${d.error?`<div class="error">${esc(d.error)}</div>`:''}</article>`}
function eventRow(e){const date=new Date(e.time*1000);const type=e.type==='enter'?'靠近':'离开';return `<div class="event"><b class="${e.type}">${type}</b><span>${esc(e.board)}</span><span class="time">${date.toLocaleTimeString()}</span>${e.peer_id?`<span class="time">${esc(e.peer_id)}</span>`:''}</div>`}
let eventsPrimed=false,lastEnterKey=null,popupTimer=null,lastIdentityShownAt=0,peerTestControlsIdentityPopup=false;
function closeIdentityPopup(){document.getElementById('identityPopup').classList.remove('show');if(popupTimer){clearTimeout(popupTimer);popupTimer=null}}
function showIdentityPopup(devices,event){const now=Date.now();if(now-lastIdentityShownAt<1500)return;const identities=new Map();for(const d of event?.identities||[]){if(d.local_id)identities.set(d.local_id,d)}for(const d of devices){if(d.local_id&&!identities.has(d.local_id))identities.set(d.local_id,d)}const boards=[...identities.values()].sort((a,b)=>(a.dog_name||a.label||'').localeCompare(b.dog_name||b.label||''));if(!boards.length)return;lastIdentityShownAt=now;const names=boards.map(d=>d.dog_name||d.label);document.getElementById('identityTitle').textContent=names.includes('狗子A')&&names.includes('狗子B')?'识别成功：狗子A 已识别 狗子B':'识别成功：已识别对方身份';document.getElementById('dogPair').innerHTML=boards.map((d,i)=>`${i?'<div class="pair-arrow">↔</div>':''}<div class="dog"><strong>${esc(d.dog_name||d.label)}</strong><code>${esc(d.local_id)}</code></div>`).join('');const stamp=new Date((event?.time||Date.now()/1000)*1000).toLocaleTimeString();document.getElementById('identityTime').textContent=event?.latency_ms!=null?`Reader 状态采样滞后约 ${event.latency_ms} ms · ${stamp}`:`${stamp} · Reader 身份包验证通过`;const popup=document.getElementById('identityPopup');popup.classList.remove('show');requestAnimationFrame(()=>popup.classList.add('show'));if(popupTimer)clearTimeout(popupTimer);popupTimer=setTimeout(closeIdentityPopup,5000)}
function detectNewIdentity(events,devices){const enter=events.find(e=>e.type==='enter');const key=enter?String(enter.event_id??`${enter.time}-${enter.board}-${enter.peer_id||''}`):null;if(!eventsPrimed){eventsPrimed=true;lastEnterKey=key;if(!peerTestControlsIdentityPopup&&enter&&Date.now()/1000-enter.time<4)showIdentityPopup(devices,enter);return}if(key&&key!==lastEnterKey){lastEnterKey=key;if(!peerTestControlsIdentityPopup)showIdentityPopup(devices,enter)}}
function updateReaderOptions(devices){const select=document.getElementById('readerSelect');const old=select.value;const online=devices.filter(d=>d.online&&d.local_id);select.innerHTML=online.length?online.map(d=>`<option value="${esc(d.local_id)}">${esc(d.label)} · USB ${esc(d.usb)}</option>`).join(''):'<option value="">未找到在线开发板</option>';if(online.some(d=>d.local_id===old))select.value=old;else{const preferred=online.find(d=>(d.configured_role||d.role)==='reader');if(preferred)select.value=preferred.local_id}}
async function postJson(path,payload={}){const response=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const result=await response.json();if(!response.ok)throw new Error(result.error||'操作失败');return result}
async function startPeerTest(){const button=document.getElementById('peerTestButton');button.disabled=true;peerTestControlsIdentityPopup=true;try{renderPeerTest(await postJson('/api/peer-test/start'))}catch(e){const box=document.getElementById('peerTestStatus');box.className='peer-test-status error';box.innerHTML=`<b>启动失败</b><span>${esc(e.message)}</span>`;button.disabled=false}}
let peerTestPrimed=false,lastPeerSuccessSequence=0;
function renderPeerTest(s){
  if(s.phase!=='idle')peerTestControlsIdentityPopup=true;
  const lag=s.latency_ms==null?'':` · 采样滞后约 ${s.latency_ms} ms`;
  const confirmation=s.target_confirmed?'Target 已同步确认':'Target 确认同步中';
  const messages={idle:['一键互认测试','会先启动蓝色 Target，再启动绿色 Reader'],preparing:['正在启动','停止两块板的旧 NFC 会话…'],starting_target:['正在启动 Target','蓝色板/狗子B 进入固定卡片模式…'],starting_reader:['正在启动 Reader','蓝色 Target 已先启动，现在启动绿色板/狗子A…'],waiting:['等待靠近','Reader 首次验证即弹窗，Target 回执稍后补充确认'],recognized:['已识别对方身份',`狗子A 已识别 狗子B${lag} · ${confirmation}`],left:['已重新布防','检测到离开空窗，再次靠近会立即重新弹窗'],error:['互认测试异常',s.last_error||'未知错误']};
  const text=messages[s.phase]||[s.phase,''];
  if(s.connection_warning)text[1]=s.connection_warning;
  else if(s.runtime_warning&&s.phase!=='recognized')text[1]=s.runtime_warning;
  const box=document.getElementById('peerTestStatus');
  box.className=`peer-test-status ${s.phase==='recognized'?'success':s.phase==='error'?'error':''}`;
  box.innerHTML=`<b>${esc(text[0])}</b><span>${esc(text[1])}</span>`;
  const button=document.getElementById('peerTestButton');
  button.disabled=!!s.setup_active;
  button.textContent=s.setup_active?'正在按顺序启动…':(s.phase==='recognized'||s.phase==='left'||s.phase==='error'?'重新开始互认':'一键开始互认');
  const seq=s.success_sequence||0;
  if(!peerTestPrimed){peerTestPrimed=true;lastPeerSuccessSequence=seq;if(s.recognized_at&&Date.now()/1000-s.recognized_at<4)showIdentityPopup(latestDevices,{time:s.recognized_at,latency_ms:s.latency_ms})}
  else if(seq>lastPeerSuccessSequence){lastPeerSuccessSequence=seq;showIdentityPopup(latestDevices,{time:s.recognized_at,latency_ms:s.latency_ms})}
}
async function refreshPeerTest(){try{const response=await fetch('/api/peer-test',{cache:'no-store'});renderPeerTest(await response.json())}catch(e){if(currentMode==='peer'){const box=document.getElementById('peerTestStatus');box.className='peer-test-status error';box.innerHTML='<b>上位机连接中断</b><span>无法读取一键互认状态</span>'}}}
async function startCardTest(){const localId=document.getElementById('readerSelect').value;if(!localId){document.getElementById('cardTestError').innerHTML='<div class="test-error">请先连接并选择一块开发板</div>';return}try{await postJson('/api/card-test/start',{local_id:localId,target_key:selectedTargetKey});await refreshCardTest()}catch(e){document.getElementById('cardTestError').innerHTML=`<div class="test-error">${esc(e.message)}</div>`}}
async function stopCardTest(){try{await postJson('/api/card-test/stop');await refreshCardTest()}catch(e){document.getElementById('cardTestError').innerHTML=`<div class="test-error">${esc(e.message)}</div>`}}
async function resetCardStats(){try{await postJson('/api/card-test/reset');await refreshCardTest()}catch(e){document.getElementById('cardTestError').innerHTML=`<div class="test-error">${esc(e.message)}</div>`}}
async function selectCardTarget(key){selectedTargetKey=key;try{await postJson('/api/card-test/target',{target_key:key});await refreshCardTest()}catch(e){document.getElementById('cardTestError').innerHTML=`<div class="test-error">${esc(e.message)}</div>`}}
const latency=v=>v==null?'—':`${v} ms`,spacedUid=uid=>(String(uid||'').match(/.{1,2}/g)||[]).join(' ');
let cardTestPrimed=false,lastCardSuccessSequence=0,cardPopupTimer=null;
function closeCardSuccessPopup(){document.getElementById('cardSuccessPopup').classList.remove('show');if(cardPopupTimer){clearTimeout(cardPopupTimer);cardPopupTimer=null}}
function showCardSuccessPopup(s,stats){document.getElementById('successCardName').textContent=s.target_name;document.getElementById('successCardUid').textContent=spacedUid(s.target_uid);document.getElementById('successLatency').textContent=`${stats.latest_ms} ms`;const popup=document.getElementById('cardSuccessPopup');popup.classList.remove('show');requestAnimationFrame(()=>popup.classList.add('show'));if(cardPopupTimer)clearTimeout(cardPopupTimer);cardPopupTimer=setTimeout(closeCardSuccessPopup,4000)}
function renderCardTest(s){selectedTargetKey=s.target_key||selectedTargetKey;for(const key of ['card_a','card_b'])document.getElementById(`target-${key}`).classList.toggle('active',key===selectedTargetKey);const stats=s.targets?.[selectedTargetKey]||{};document.getElementById('scanTarget').textContent=s.target_name||'CARD-A';document.getElementById('scanUid').textContent=spacedUid(s.target_uid||'08A10001');const messages={starting:'正在切换到读卡测速模式…',scanning:`等待 ${s.target_name} 靠近`,remove_card:`已读到 ${s.target_name}，请移开卡片`,wrong_card:`读到 ${s.last_card||'其他卡片'}，请换成 ${s.target_name}`,error:'PN532 扫描异常',stopped:'请选择读卡器并开始'};document.getElementById('scanState').textContent=messages[s.phase]||s.phase;const hero=document.getElementById('scanHero');hero.classList.toggle('success',s.phase==='remove_card');hero.classList.toggle('error',s.phase==='error');const elapsed=s.active&&s.attempt_started?Math.max(0,Math.round(Date.now()-s.attempt_started*1000)):null;document.getElementById('scanElapsed').textContent=elapsed==null?`单次扫描窗口 ${s.scan_timeout_ms} ms`:`本轮已等待 ${elapsed} ms · 窗口 ${s.scan_timeout_ms} ms`;document.getElementById('latestLatency').textContent=latency(stats.latest_ms);document.getElementById('firstLatency').textContent=latency(stats.first_ms);document.getElementById('bestLatency').textContent=latency(stats.best_ms);document.getElementById('successCount').textContent=stats.success_count??0;document.getElementById('timeoutCount').textContent=stats.timeout_count??0;document.getElementById('cardTestError').innerHTML=s.last_error?`<div class="test-error">${esc(s.last_error)}</div>`:'';const definitions={card_a:['CARD-A','08 A1 00 01'],card_b:['CARD-B','08 B2 00 02']};document.getElementById('targetSummary').innerHTML=Object.entries(definitions).map(([key,v])=>{const x=s.targets?.[key]||{};return `<article><b>${v[0]} · <code>${v[1]}</code></b><span>成功 ${x.success_count||0} 次 · 超时 ${x.timeout_count||0} 次 · 最快 ${latency(x.best_ms)}</span></article>`}).join('');const seq=s.success_sequence||0;if(!cardTestPrimed){cardTestPrimed=true;lastCardSuccessSequence=seq;if(s.last_success_time&&Date.now()/1000-s.last_success_time<3)showCardSuccessPopup(s,stats)}else if(seq>lastCardSuccessSequence){lastCardSuccessSequence=seq;showCardSuccessPopup(s,stats)}}
async function refreshCardTest(){try{const response=await fetch('/api/card-test',{cache:'no-store'});renderCardTest(await response.json())}catch(e){if(currentMode==='card')document.getElementById('cardTestError').innerHTML='<div class="test-error">卡片测速 API 连接中断</div>'}}
async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});const s=await r.json();const devices=s.devices||[];latestDevices=devices;updateReaderOptions(devices);document.getElementById('cards').innerHTML=devices.length?devices.map(card).join(''):'<article class="card"><div class="presence waiting"><strong>未找到 ADB 设备</strong><span>请检查 Type-C 和 adb devices</span></div></article>';const online=devices.filter(d=>d.online&&d.local_id).length;document.getElementById('dot').className='dot '+(online?'ok':'');document.getElementById('summary').textContent=`${online}/${devices.length||2} 块在线 · ${new Date(s.timestamp*1000).toLocaleTimeString()}`;const events=s.events||[];document.getElementById('events').innerHTML=events.length?events.map(eventRow).join(''):'<div class="empty">暂无靠近/离开事件</div>';detectNewIdentity(events,devices);}catch(e){document.getElementById('summary').textContent='上位机连接中断';document.getElementById('dot').className='dot';}}
refresh();refreshCardTest();refreshPeerTest();setInterval(refresh,800);setInterval(refreshCardTest,300);setInterval(refreshPeerTest,300);
</script></body></html>"""


def make_handler(
    store: StateStore,
    card_test: CardTestController,
    peer_test: PeerTestController,
) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        def _send_json(self, payload: Any, status: int = 200) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            if self.path.split("?", 1)[0] == "/api/status":
                body = json.dumps(store.get(), ensure_ascii=False).encode("utf-8")
                content_type = "application/json; charset=utf-8"
            elif self.path.split("?", 1)[0] == "/api/card-test":
                self._send_json(card_test.get())
                return
            elif self.path.split("?", 1)[0] == "/api/peer-test":
                self._send_json(peer_test.get())
                return
            elif self.path.split("?", 1)[0] in ("/", "/index.html"):
                body = HTML.encode("utf-8")
                content_type = "text/html; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            path = self.path.split("?", 1)[0]
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length < 0 or length > 16384:
                    raise ValueError("请求内容过大")
                payload = (
                    json.loads(self.rfile.read(length).decode("utf-8"))
                    if length else {}
                )
                if not isinstance(payload, dict):
                    raise ValueError("请求必须是 JSON 对象")

                if path == "/api/card-test/start":
                    result = card_test.start(
                        str(payload.get("local_id", "")),
                        str(payload.get("target_key", "card_a")),
                    )
                elif path == "/api/card-test/target":
                    result = card_test.select_target(
                        str(payload.get("target_key", ""))
                    )
                elif path == "/api/card-test/reset":
                    result = card_test.reset_stats()
                elif path == "/api/card-test/stop":
                    if not card_test.stop():
                        raise RuntimeError("读卡命令尚未停止")
                    result = card_test.get()
                elif path == "/api/peer-test/start":
                    result = peer_test.start()
                else:
                    self.send_error(404)
                    return
                self._send_json(result)
            except (ValueError, RuntimeError) as error:
                self._send_json({"error": str(error)}, status=400)
            except Exception as error:
                self._send_json({"error": str(error)}, status=500)

        def log_message(self, format: str, *args: Any) -> None:
            return

    return Handler


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="ARCS-MINI NFC 双板互认实时网页上位机"
    )
    parser.add_argument("--adb", default="adb", help="adb 可执行文件（默认: adb）")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP 监听地址")
    parser.add_argument("--port", type=int, default=8765, help="HTTP 端口")
    parser.add_argument(
        "--interval",
        type=float,
        default=DEFAULT_POLL_INTERVAL_SECONDS,
        help="ADB 轮询间隔秒数（默认: 0.25）",
    )
    parser.add_argument(
        "--card-timeout-ms",
        type=int,
        default=1000,
        help="卡片测速单次扫描窗口（默认: 1000ms）",
    )
    parser.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    store = StateStore()
    card_test = CardTestController(args.adb, store, args.card_timeout_ms)
    peer_test = PeerTestController(args.adb, store, card_test)
    poller = Poller(
        args.adb,
        max(0.1, args.interval),
        store,
        pause_provider=lambda: (
            card_test.is_active() or peer_test.is_setup_active()
        ),
    )
    poller.start()
    server = ThreadingHTTPServer(
        (args.host, args.port), make_handler(store, card_test, peer_test)
    )
    url = f"http://{args.host}:{args.port}/"
    print(f"ARCS-MINI NFC dashboard: {url}")
    print("按 Ctrl+C 停止")
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        peer_test.wait_setup()
        card_test.stop(wait_seconds=10.5)
        poller.stop_event.set()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
