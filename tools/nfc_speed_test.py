#!/usr/bin/env python3
"""Low-interference two-board NFC recognition timing tool.

The Reader and Target are fixed by their 64-bit chip IDs.  The firmware emits
one ``NFC_PEER_RECOGNIZED`` line for the first exchange and
``NFC_PEER_EXCHANGE`` for subsequent successful exchanges after the identity
packet has passed CRC and peer-ID validation.  This tool keeps a persistent ADB
shell open on each board and listens for those lines; it does not poll
``peer status`` while a test is running, so the displayed latency is the
board-side measurement rather than an ADB/UI round-trip.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import re
import subprocess
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from statistics import median
from typing import Any, Callable, Sequence


READER_ID = "6b3bc01b1634cfbd"
CURRENT_TARGET_ID = "6b3bc01b4050d1bd"
LEGACY_TARGET_ID = "6b3bc01b1d41cfbd"
MAX_SAMPLE_HISTORY = 2048

ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
LOCAL_ID_RE = re.compile(r"\blocal=(?P<id>[0-9a-fA-F]{16})\b")
CHIP_ID_RE = re.compile(r"(?:Chip ID:|did:)\s*(?P<id>[0-9a-fA-F]{16})", re.I)
MACHINE_FIELD_RE = re.compile(r"(?P<key>[a-z_]+)=(?P<value>[^\s]+)")
ERROR_LINE_RE = re.compile(
    r"(?:NFC_PEER_ERROR|PN532.*(?:failed|error|timeout)|"
    r"v2\.nfc\.peer.*(?:failed|error))",
    re.I,
)


def adb_environment() -> dict[str, str]:
    env = os.environ.copy()
    # The libusb backend collapses these two boards because their ADB serials
    # are identical. The native Linux backend preserves both transport IDs.
    env["ADB_LIBUSB"] = "0"
    return env


def clean_line(line: str) -> str:
    return ANSI_RE.sub("", line).replace("\r", "").strip()


def parse_recognized_event(line: str) -> dict[str, Any] | None:
    line = clean_line(line)
    marker_match = re.search(r"NFC_PEER_(?:RECOGNIZED|EXCHANGE)", line)
    if marker_match is None:
        return None
    marker_end = marker_match.end()
    fields = {
        match.group("key"): match.group("value")
        for match in MACHINE_FIELD_RE.finditer(line[marker_end:])
    }
    local_id = fields.get("local", "").lower()
    peer_id = fields.get("peer", "").lower()
    if not re.fullmatch(r"[0-9a-f]{16}", local_id) or not re.fullmatch(
        r"[0-9a-f]{16}", peer_id
    ):
        return None
    try:
        return {
            "local": local_id,
            "peer": peer_id,
            "latency_ms": int(fields["latency_ms"]),
            "count": int(fields["count"]),
            "received_at": time.time(),
        }
    except (KeyError, ValueError):
        return None


def run_adb(adb: str, transport_id: int, command: str, timeout: float = 3.0) -> str:
    completed = subprocess.run(
        [adb, "-t", str(transport_id), "shell", command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
        env=adb_environment(),
    )
    return completed.stdout


def discover_transports(adb: str) -> list[dict[str, Any]]:
    completed = subprocess.run(
        [adb, "devices", "-l"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=2.0,
        check=False,
        env=adb_environment(),
    )
    result: list[dict[str, Any]] = []
    for raw in completed.stdout.splitlines():
        fields = raw.split()
        if len(fields) < 2 or fields[1] != "device":
            continue
        attrs: dict[str, str] = {}
        for field in fields[2:]:
            if ":" in field:
                key, value = field.split(":", 1)
                attrs[key] = value
        try:
            transport_id = int(attrs["transport_id"])
        except (KeyError, ValueError):
            continue
        result.append(
            {
                "transport_id": transport_id,
                "serial": fields[0],
                "usb": attrs.get("usb", "unknown"),
            }
        )
    return sorted(result, key=lambda item: str(item["usb"]))


def restart_adb_server(adb: str) -> None:
    for command in ([adb, "kill-server"], [adb, "start-server"]):
        subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=5.0,
            check=False,
            env=adb_environment(),
        )


def identify_transport(adb: str, device: dict[str, Any]) -> str | None:
    """Read identity once during setup; never call this from the run loop."""
    transport_id = int(device["transport_id"])
    try:
        output = run_adb(adb, transport_id, "v2hw nfc peer status", timeout=2.0)
    except (OSError, subprocess.SubprocessError):
        output = ""
    output = ANSI_RE.sub("", output)
    match = LOCAL_ID_RE.search(output)
    if match:
        return match.group("id").lower()
    try:
        output = run_adb(adb, transport_id, "device get_device_id", timeout=2.0)
    except (OSError, subprocess.SubprocessError):
        return None
    match = CHIP_ID_RE.search(ANSI_RE.sub("", output))
    return match.group("id").lower() if match else None


class EventStream:
    """One long-lived ``adb shell`` reader for a single transport."""

    def __init__(
        self,
        adb: str,
        device: dict[str, Any],
        role: str,
        on_line: Callable[[str, str], None],
    ) -> None:
        self.adb = adb
        self.device = device
        self.role = role
        self.on_line = on_line
        self.process: subprocess.Popen[str] | None = None
        self.thread: threading.Thread | None = None
        self.lock = threading.Lock()
        self.lines: queue.Queue[str] = queue.Queue(maxsize=512)

    def start(self) -> None:
        self.process = subprocess.Popen(
            [
                self.adb,
                "-t",
                str(self.device["transport_id"]),
                "shell",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            bufsize=1,
            env=adb_environment(),
        )
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self) -> None:
        process = self.process
        if not process or not process.stdout:
            return
        try:
            for raw_line in process.stdout:
                line = clean_line(raw_line)
                try:
                    self.lines.put_nowait(line)
                except queue.Full:
                    try:
                        self.lines.get_nowait()
                        self.lines.put_nowait(line)
                    except queue.Empty:
                        pass
                self.on_line(self.role, line)
        except (OSError, ValueError) as error:
            self.on_line(self.role, f"__stream_error__ {error}")
        finally:
            self.on_line(self.role, "__stream_closed__")

    def send(self, command: str) -> None:
        with self.lock:
            process = self.process
            if not process or not process.stdin or process.poll() is not None:
                raise RuntimeError(f"{self.role} ADB shell 已断开")
            process.stdin.write(command.rstrip("\n") + "\n")
            process.stdin.flush()

    def send_and_wait(
        self, command: str, success: str, timeout: float
    ) -> str:
        while True:
            try:
                self.lines.get_nowait()
            except queue.Empty:
                break
        self.send(command)
        deadline = time.monotonic() + timeout
        recent: list[str] = []
        while time.monotonic() < deadline:
            try:
                remaining = max(0.01, deadline - time.monotonic())
                line = self.lines.get(timeout=min(0.25, remaining))
            except queue.Empty:
                continue
            recent.append(line)
            if success in line:
                return line
            if "v2hw command failed:" in line:
                raise RuntimeError(f"{self.role} 执行 {command!r} 失败：{line}")
        tail = " | ".join(recent[-3:])
        raise RuntimeError(
            f"{self.role} 等待 {success!r} 超时" + (f"：{tail}" if tail else "")
        )

    def stop(self) -> None:
        process = self.process
        if not process:
            return
        try:
            # The firmware releases the PN532 worker only after this marker.
            # Waiting for it prevents a quick restart from racing the old
            # worker while it is still unwinding its I2C session.
            self.send_and_wait(
                "v2hw nfc peer stop", "nfc peer discovery stopped", 8.0
            )
        except (OSError, RuntimeError):
            pass
        try:
            self.send("exit")
        except (OSError, RuntimeError):
            pass
        try:
            process.terminate()
            process.wait(timeout=1.5)
        except (OSError, subprocess.TimeoutExpired):
            try:
                process.kill()
            except OSError:
                pass
        self.process = None


class TestController:
    def __init__(
        self,
        adb: str,
        reader_id: str = READER_ID,
        target_id: str | None = None,
        target_transport: int | None = None,
    ) -> None:
        self.adb = adb
        self.reader_id = reader_id.lower()
        if self.reader_id != READER_ID:
            raise ValueError(f"Reader 固定为 {READER_ID}")
        self.requested_target_id = target_id.lower() if target_id else None
        if self.requested_target_id not in (None, CURRENT_TARGET_ID, LEGACY_TARGET_ID):
            raise ValueError("Target 只支持当前 4050d1bd 或旧 1d41cfbd")
        self.target_id = self.requested_target_id or CURRENT_TARGET_ID
        self.target_transport_arg = target_transport
        self.lock = threading.RLock()
        self.subscribers: list[queue.Queue[dict[str, Any]]] = []
        self.streams: dict[str, EventStream] = {}
        self._stopping = False
        self._sample_sum = 0
        self._sample_min: int | None = None
        self._sample_max: int | None = None
        self.state: dict[str, Any] = {
            "phase": "idle",
            "reader_id": self.reader_id,
            "target_id": self.target_id,
            "reader_transport": None,
            "target_transport": None,
            "count": 0,
            "samples": [],
            "latest_ms": None,
            "min_ms": None,
            "max_ms": None,
            "avg_ms": None,
            "median_ms": None,
            "events": [],
            "errors": [],
            "last_error": None,
            "last_line": None,
            "last_event_time": None,
            "reader_connected": False,
            "target_connected": False,
            "measurement_source": "board_event_latency_ms",
            "measurement_note": (
                "板端 NFC_PEER_RECOGNIZED/NFC_PEER_EXCHANGE latency_ms；"
                "不包含 ADB、HTTP 或页面刷新；"
                f"中位数取最近 {MAX_SAMPLE_HISTORY} 个样本"
            ),
            "started_at": None,
        }

    def subscribe(self) -> queue.Queue[dict[str, Any]]:
        channel: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=32)
        with self.lock:
            self.subscribers.append(channel)
            self._publish_locked(channel)
        return channel

    def unsubscribe(self, channel: queue.Queue[dict[str, Any]]) -> None:
        with self.lock:
            if channel in self.subscribers:
                self.subscribers.remove(channel)

    def _snapshot_locked(self) -> dict[str, Any]:
        return {
            **self.state,
            "samples": list(self.state["samples"]),
            "events": [dict(event) for event in self.state["events"]],
            "errors": [dict(error) for error in self.state["errors"]],
        }

    def _publish_locked(self, only: queue.Queue[dict[str, Any]] | None = None) -> None:
        payload = self._snapshot_locked()
        channels = [only] if only is not None else list(self.subscribers)
        for channel in channels:
            try:
                channel.put_nowait(payload)
            except queue.Full:
                try:
                    channel.get_nowait()
                    channel.put_nowait(payload)
                except queue.Empty:
                    pass

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return self._snapshot_locked()

    def _set_error(self, message: str) -> None:
        with self.lock:
            self._append_error_locked(message)
            self.state["phase"] = "error"
            self._publish_locked()

    def _append_error_locked(self, message: str, line: str | None = None) -> None:
        error: dict[str, Any] = {"time": time.time(), "message": message}
        if line:
            error["line"] = line[-500:]
        errors: list[dict[str, Any]] = self.state["errors"]
        errors.append(error)
        del errors[:-30]
        self.state["last_error"] = message

    def _on_line(self, role: str, line: str) -> None:
        if not line:
            return
        with self.lock:
            self.state["last_line"] = line[-500:]
            self.state[f"{role}_connected"] = True
            if line.startswith("__stream_error__"):
                self.state[f"{role}_connected"] = False
                if not self._stopping:
                    self._append_error_locked(f"{role}: {line[16:].strip()}")
                if not self._stopping and self.state["phase"] in (
                    "starting", "arming", "armed", "running"
                ):
                    self.state["phase"] = "error"
                self._publish_locked()
                return
            if line == "__stream_closed__":
                self.state[f"{role}_connected"] = False
                if not self._stopping and self.state["phase"] in (
                    "starting", "arming", "armed", "running"
                ):
                    self._append_error_locked(f"{role} ADB 事件流已断开")
                    self.state["phase"] = "error"
                self._publish_locked()
                return
            if ERROR_LINE_RE.search(line):
                self._append_error_locked(f"{role} 设备日志报告 NFC 错误", line)
                self._publish_locked()
                return
            event = parse_recognized_event(line)
            if not event or role != "reader":
                return
            if self.state["phase"] not in ("running", "armed"):
                return
            local_id = str(event["local"])
            peer_id = str(event["peer"])
            if local_id != self.reader_id or peer_id != self.target_id:
                return
            latency_ms = int(event["latency_ms"])
            board_count = int(event["count"])
            received_at = float(event["received_at"])
            samples: list[int] = self.state["samples"]
            total_count = int(self.state["count"]) + 1
            samples.append(latency_ms)
            del samples[:-MAX_SAMPLE_HISTORY]
            self._sample_sum += latency_ms
            self._sample_min = (
                latency_ms
                if self._sample_min is None
                else min(self._sample_min, latency_ms)
            )
            self._sample_max = (
                latency_ms
                if self._sample_max is None
                else max(self._sample_max, latency_ms)
            )
            events: list[dict[str, Any]] = self.state["events"]
            events.append(
                {
                    "sequence": total_count,
                    "latency_ms": latency_ms,
                    "board_count": board_count,
                    "received_at": received_at,
                }
            )
            del events[:-100]
            self.state.update(
                count=total_count,
                latest_ms=latency_ms,
                min_ms=self._sample_min,
                max_ms=self._sample_max,
                avg_ms=round(self._sample_sum / total_count, 2),
                median_ms=median(samples),
                board_count=board_count,
                last_event_time=received_at,
                last_error=None,
            )
            self._publish_locked()

    def _find_pair(self) -> tuple[dict[str, Any], dict[str, Any]]:
        devices = discover_transports(self.adb)
        if len(devices) < 2:
            restart_adb_server(self.adb)
            devices = discover_transports(self.adb)
        identities = [(device, identify_transport(self.adb, device)) for device in devices]
        reader = next((device for device, identity in identities if identity == self.reader_id), None)
        target: dict[str, Any] | None = None
        actual_target_id: str | None = None
        if self.target_transport_arg is not None:
            pair = next(
                (
                    (device, identity)
                    for device, identity in identities
                    if int(device["transport_id"]) == self.target_transport_arg
                ),
                None,
            )
            if pair:
                target, actual_target_id = pair
                if self.requested_target_id and actual_target_id != self.requested_target_id:
                    raise RuntimeError(
                        "--target-transport 对应芯片 ID 与 --target-id 不一致"
                    )
        elif self.requested_target_id:
            pair = next(
                (
                    (device, identity)
                    for device, identity in identities
                    if identity == self.requested_target_id
                ),
                None,
            )
            if pair:
                target, actual_target_id = pair
        else:
            for preferred in (CURRENT_TARGET_ID, LEGACY_TARGET_ID):
                pair = next(
                    (
                        (device, identity)
                        for device, identity in identities
                        if identity == preferred
                    ),
                    None,
                )
                if pair:
                    target, actual_target_id = pair
                    break
        if actual_target_id not in (CURRENT_TARGET_ID, LEGACY_TARGET_ID):
            target = None
        if not reader or not target:
            found = ", ".join(identity or "unknown" for _, identity in identities)
            raise RuntimeError(
                f"未找到固定双板（Reader={self.reader_id[-4:]}, "
                "Target=4050d1bd/1d41cfbd）；"
                f"当前识别到：{found or '无设备'}"
            )
        self.target_id = str(actual_target_id)
        with self.lock:
            self.state["target_id"] = self.target_id
        return reader, target

    @staticmethod
    def _peer_is_enabled(status_line: str) -> bool:
        return re.search(r"\benabled=1\b", status_line) is not None

    def _quiesce_peer(self, stream: EventStream) -> None:
        """Stop an old worker only when setup status says one is running.

        The V2.1 timing firmware boots with peer discovery disabled. Calling
        ``peer stop`` on an already-disabled board needlessly reinitializes the
        PN532 and can turn a valid cold boot into an I2C error. This one status
        read is setup-only; the running measurement remains event-driven.
        """
        status_line: str | None = None
        try:
            status_line = stream.send_and_wait(
                "v2hw nfc peer status", "nfc peer:", 3.0
            )
        except RuntimeError:
            # Older firmware may not expose peer status. Preserve the legacy
            # cleanup path in that case.
            pass
        if status_line is not None and not self._peer_is_enabled(status_line):
            return
        stream.send_and_wait(
            "v2hw nfc peer stop", "nfc peer discovery stopped", 8.0
        )

    def start(self) -> dict[str, Any]:
        with self.lock:
            if self.state["phase"] in ("starting", "armed", "running"):
                return self._snapshot_locked()
            self.state.update(
                phase="starting",
                count=0,
                samples=[],
                latest_ms=None,
                min_ms=None,
                max_ms=None,
                avg_ms=None,
                median_ms=None,
                events=[],
                errors=[],
                board_count=0,
                last_error=None,
                last_event_time=None,
                reader_connected=False,
                target_connected=False,
                started_at=time.time(),
            )
            self._sample_sum = 0
            self._sample_min = None
            self._sample_max = None
            self._publish_locked()
        try:
            reader, target = self._find_pair()
            self.stop_streams()
            reader_stream = EventStream(self.adb, reader, "reader", self._on_line)
            target_stream = EventStream(self.adb, target, "target", self._on_line)
            # Register both streams before starting either one. If the second
            # shell fails to start, cleanup still sees the first shell.
            self.streams = {"reader": reader_stream, "target": target_stream}
            reader_stream.start()
            target_stream.start()
            with self.lock:
                self.state.update(
                    reader_transport=reader["transport_id"],
                    target_transport=target["transport_id"],
                    reader_connected=True,
                    target_connected=True,
                    phase="arming",
                )
                self._publish_locked()

            # Quiesce stale sessions, then arm Target before Reader. These
            # commands run only during setup; no status polling occurs during
            # measurement.
            self._quiesce_peer(reader_stream)
            self._quiesce_peer(target_stream)
            target_stream.send_and_wait(
                "v2hw nfc peer start", "nfc peer discovery started", 3.0
            )
            time.sleep(0.35)
            with self.lock:
                self.state["phase"] = "armed"
                self._publish_locked()
            reader_stream.send_and_wait(
                "v2hw nfc peer start", "nfc peer discovery started", 3.0
            )
            with self.lock:
                self.state["phase"] = "running"
                self._publish_locked()
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            self.stop_streams()
            self._set_error(str(error))
        return self.snapshot()

    def stop_streams(self) -> None:
        with self.lock:
            self._stopping = True
            streams = list(self.streams.values())
            self.streams = {}
        try:
            for stream in streams:
                stream.stop()
        finally:
            with self.lock:
                self._stopping = False
                self.state.update(reader_connected=False, target_connected=False)
                self._publish_locked()

    def stop(self) -> dict[str, Any]:
        self.stop_streams()
        with self.lock:
            self.state["phase"] = "stopped"
            self._publish_locked()
            return self._snapshot_locked()

    def reset(self) -> dict[str, Any]:
        self.stop_streams()
        with self.lock:
            self.state.update(
                phase="idle",
                count=0,
                samples=[],
                latest_ms=None,
                min_ms=None,
                max_ms=None,
                avg_ms=None,
                median_ms=None,
                events=[],
                errors=[],
                board_count=0,
                last_error=None,
                last_line=None,
                last_event_time=None,
                reader_connected=False,
                target_connected=False,
                started_at=None,
            )
            self._sample_sum = 0
            self._sample_min = None
            self._sample_max = None
            self._publish_locked()
            return self._snapshot_locked()


HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="data:,">
<title>NFC 双板识别测速</title>
<style>
:root{color-scheme:dark;font:16px system-ui,sans-serif;background:#111827;color:#f3f4f6}
body{max-width:900px;margin:0 auto;padding:28px 20px}h1{font-size:28px;margin:0 0 8px}
.sub{color:#9ca3af;margin-bottom:22px}.bar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:22px}
button{border:0;border-radius:6px;padding:10px 16px;font-size:15px;cursor:pointer;background:#2563eb;color:#fff}
button.secondary{background:#374151}button.danger{background:#b91c1c}button:disabled{opacity:.5;cursor:default}
.status{padding:10px 12px;border-left:4px solid #6b7280;background:#1f2937;margin-bottom:18px}
.status.running{border-color:#22c55e}.status.error{border-color:#ef4444}.status.arming{border-color:#f59e0b}
.hero{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:18px}
.metric{background:#1f2937;padding:16px;border-radius:6px}.metric strong{display:block;font-size:30px;color:#60a5fa;margin-top:4px}.metric small{color:#9ca3af}
.event{background:#1f2937;border-radius:6px;padding:14px;margin-top:14px}.event .ms{font-size:22px;color:#34d399}.muted{color:#9ca3af}.error{color:#fca5a5;white-space:pre-wrap}
code{color:#c4b5fd}@media(max-width:680px){.hero{grid-template-columns:repeat(2,1fr)}}
</style></head>
<body>
<h1>NFC 双板识别测速</h1>
<div class="sub">Reader <code id="reader"></code> · Target <code id="target"></code></div>
<div id="status" class="status">正在连接</div><div id="connection" class="sub"></div>
<div class="bar"><button id="start">开始测试</button><button id="stop" class="danger">停止</button><button id="reset" class="secondary">清零</button></div>
<section class="hero">
 <div class="metric"><small>识别次数</small><strong id="count">0</strong></div>
 <div class="metric"><small>最近一次（板端）</small><strong id="latest">--</strong><small>ms</small></div>
 <div class="metric"><small>最快</small><strong id="min">--</strong><small>ms</small></div>
 <div class="metric"><small>最慢</small><strong id="max">--</strong><small>ms</small></div>
 <div class="metric"><small>平均</small><strong id="avg">--</strong><small>ms</small></div>
 <div class="metric"><small>中位数</small><strong id="median">--</strong><small>ms</small></div>
</section>
<div class="sub" id="measurement-note"></div><div>最近事件：<span id="event-time">--</span></div>
<div id="error" class="error"></div><div id="events"></div>
<script>
const $=id=>document.getElementById(id);
function fmt(v){return v===null||v===undefined?'--':v}
function render(s){
 $('reader').textContent=s.reader_id.slice(-8);$('target').textContent=s.target_id.slice(-8);
 $('count').textContent=fmt(s.count);$('latest').textContent=fmt(s.latest_ms);
 $('min').textContent=fmt(s.min_ms);$('max').textContent=fmt(s.max_ms);
 $('avg').textContent=fmt(s.avg_ms);$('median').textContent=fmt(s.median_ms);
 const phase=s.phase||'idle';$('status').className='status '+phase;
 const labels={idle:'待开始',starting:'正在连接设备',arming:'正在启动 Target → Reader',running:'测试中：等待天线靠近',stopped:'已停止',error:'连接中断或测试失败'};
 $('status').textContent=labels[phase]||phase;
 $('connection').textContent='Reader '+(s.reader_connected?'已连接':'未连接')+' · Target '+(s.target_connected?'已连接':'未连接');
 $('measurement-note').textContent=s.measurement_note||'';
 $('event-time').textContent=s.last_event_time?new Date(s.last_event_time*1000).toLocaleTimeString():'--';
 $('error').textContent=(s.errors||[]).slice(-5).map(e=>new Date(e.time*1000).toLocaleTimeString()+' '+e.message).join('\n');
 const arr=s.events||[]; $('events').innerHTML=arr.slice().reverse().slice(0,30).map(e=>`<div class="event"><span class="ms">${e.latency_ms} ms</span><span class="muted"> 第 ${e.sequence} 次 · ${new Date(e.received_at*1000).toLocaleTimeString()} · 板端计时</span></div>`).join('');
 $('start').disabled=['starting','arming','running'].includes(phase);
}
async function action(path){await fetch(path,{method:'POST'});}
$('start').onclick=()=>action('/api/start');$('stop').onclick=()=>action('/api/stop');$('reset').onclick=()=>action('/api/reset');
const stream=new EventSource('/events');stream.onmessage=e=>render(JSON.parse(e.data));stream.onerror=()=>{$('status').textContent='上位机连接中断'};
fetch('/api/status').then(r=>r.json()).then(render);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    controller: TestController

    def _json(self, payload: dict[str, Any], status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/":
            body = HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/api/status":
            self._json(self.controller.snapshot())
            return
        if self.path == "/events":
            channel = self.controller.subscribe()
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                while True:
                    try:
                        payload = channel.get(timeout=20.0)
                        data = json.dumps(payload, ensure_ascii=False)
                        self.wfile.write(f"data: {data}\n\n".encode("utf-8"))
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                self.controller.unsubscribe(channel)
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/api/start":
            self._json(self.controller.start())
        elif self.path == "/api/stop":
            self._json(self.controller.stop())
        elif self.path == "/api/reset":
            self._json(self.controller.reset())
        else:
            self.send_error(404)

    def log_message(self, format: str, *args: Any) -> None:
        return


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="固定双板 NFC 识别测速上位机")
    parser.add_argument("--adb", default="adb", help="adb 可执行文件")
    parser.add_argument("--reader-id", default=READER_ID, choices=(READER_ID,))
    parser.add_argument(
        "--target-id",
        default=None,
        choices=(CURRENT_TARGET_ID, LEGACY_TARGET_ID),
        help="Target ID；默认优先当前 4050d1bd，找不到时兼容旧 1d41cfbd",
    )
    parser.add_argument(
        "--target-transport", type=int, default=None,
        help="显式指定 Target 的 adb transport_id",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--no-browser", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)

    controller = TestController(
        args.adb,
        args.reader_id,
        args.target_id,
        target_transport=args.target_transport,
    )
    Handler.controller = controller
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    url = f"http://{args.host}:{args.port}/"
    print(f"NFC 测速上位机：{url}", flush=True)
    print(
        f"Reader={args.reader_id.lower()} Target={args.target_id or 'auto(4050/1d41)'}；"
        "统计为 Reader 固件事件的板端 latency_ms",
        flush=True,
    )
    if not args.no_browser:
        try:
            webbrowser.open(url)
        except Exception:
            pass
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        controller.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
