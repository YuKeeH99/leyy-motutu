#!/usr/bin/env python3
"""Minimal two-board PN532 target/reader test UI.

This deliberately uses the generic ``v2hw nfc target`` and ``scan`` commands
instead of the product-specific NFC peer service.  It therefore works with
new boards whose chip IDs are not present in the firmware peer allowlist.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import threading
import time
import webbrowser
from dataclasses import asdict, dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Sequence


DEFAULT_READER_ID = "6b3bc01b1642ccbd"
DEFAULT_TARGET_ID = "6b3bc01b1442ccbd"
TARGET_UID = "08123456"
TARGET_WINDOW_MS = 30000
TARGET_ARM_DELAY_S = 1.0
TARGET_REARM_DELAY_S = 0.25
SCAN_INTERVAL_S = 0.25
PEER_STOP_HOST_TIMEOUT_S = 10.0
ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
CHIP_ID_RE = re.compile(r"Chip ID:\s*([0-9a-fA-F]{16})")
SCAN_RE = re.compile(
    r"nfc target=(?P<target>\d+)\s+uid=(?P<uid>[0-9a-fA-F]+)\s+"
    r"sens_res=(?P<sens>0x[0-9a-fA-F]+)\s+"
    r"sel_res=(?P<sel>0x[0-9a-fA-F]+)"
)


def adb_environment() -> dict[str, str]:
    environment = os.environ.copy()
    # The two boards intentionally use the same USB serial number.  The
    # native backend preserves both transport IDs; libusb may collapse them.
    environment["ADB_LIBUSB"] = "0"
    return environment


def clean_output(output: str) -> str:
    return ANSI_RE.sub("", output).replace("\r", "")


@dataclass(frozen=True)
class Device:
    transport_id: int
    serial: str
    usb: str
    chip_id: str


def parse_adb_devices(output: str) -> list[dict[str, Any]]:
    devices: list[dict[str, Any]] = []
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
            {
                "transport_id": transport_id,
                "serial": fields[0],
                "usb": attributes.get("usb", "unknown"),
            }
        )
    return sorted(devices, key=lambda item: (item["usb"], item["transport_id"]))


def extract_chip_id(output: str) -> str | None:
    matches = CHIP_ID_RE.findall(clean_output(output))
    return matches[-1].lower() if matches else None


def parse_scan_result(output: str) -> dict[str, str] | None:
    match = SCAN_RE.search(clean_output(output))
    if not match:
        return None
    values = match.groupdict()
    return {
        "target": values["target"],
        "uid": values["uid"].upper(),
        "sens_res": values["sens"].lower(),
        "sel_res": values["sel"].lower(),
    }


def run_process(arguments: Sequence[str], timeout: float) -> str:
    completed = subprocess.run(
        list(arguments),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
        env=adb_environment(),
    )
    return completed.stdout


def discover_devices(adb: str) -> list[Device]:
    listed = parse_adb_devices(run_process([adb, "devices", "-l"], 2.0))
    devices: list[Device] = []
    # Keep discovery sequential.  The board shell accepts one connection at a
    # time and can drop packets when several one-shot shells race each other.
    for item in listed:
        output = run_process(
            [
                adb,
                "-t",
                str(item["transport_id"]),
                "shell",
                "device get_device_id",
            ],
            4.0,
        )
        chip_id = extract_chip_id(output)
        if chip_id:
            devices.append(
                Device(
                    transport_id=int(item["transport_id"]),
                    serial=str(item["serial"]),
                    usb=str(item["usb"]),
                    chip_id=chip_id,
                )
            )
    return devices


class PairController:
    def __init__(self, adb: str) -> None:
        self.adb = adb
        self.lock = threading.Lock()
        self.devices: list[Device] = []
        self.reader_id: str | None = None
        self.target_id: str | None = None
        self.running = False
        self.phase = "idle"
        self.message = "正在发现两块开发板"
        self.attempts = 0
        self.success_count = 0
        self.rounds = 0
        self.last_uid: str | None = None
        self.last_reader_output = ""
        self.last_target_output = ""
        self.started_at: float | None = None
        self.finished_at: float | None = None
        self.stop_requested = threading.Event()
        self.worker: threading.Thread | None = None

    def _device(self, chip_id: str | None) -> Device | None:
        return next((item for item in self.devices if item.chip_id == chip_id), None)

    def _set_default_roles(self) -> None:
        ids = {item.chip_id for item in self.devices}
        if DEFAULT_READER_ID in ids and DEFAULT_TARGET_ID in ids:
            self.reader_id = DEFAULT_READER_ID
            self.target_id = DEFAULT_TARGET_ID
        elif len(self.devices) >= 2:
            self.reader_id = self.devices[0].chip_id
            self.target_id = self.devices[1].chip_id
        else:
            self.reader_id = None
            self.target_id = None

    def refresh(self) -> dict[str, Any]:
        with self.lock:
            if self.running:
                return {"ok": False, "error": "测试运行中，不能重新枚举"}
        try:
            devices = discover_devices(self.adb)
        except (OSError, subprocess.SubprocessError) as error:
            with self.lock:
                self.phase = "error"
                self.message = f"ADB 枚举失败：{error}"
            return {"ok": False, "error": str(error)}
        with self.lock:
            previous = {self.reader_id, self.target_id}
            self.devices = devices
            live_ids = {item.chip_id for item in devices}
            if len(devices) != 2 or not previous.issubset(live_ids):
                self._set_default_roles()
            self.phase = "idle" if len(devices) == 2 else "error"
            self.message = (
                "两块板已就绪，点击开始后把天线贴近"
                if len(devices) == 2
                else f"需要恰好两块 ADB 在线，当前识别到 {len(devices)} 块"
            )
        return {"ok": len(devices) == 2, "state": self.snapshot()}

    def swap(self) -> dict[str, Any]:
        with self.lock:
            if self.running:
                return {"ok": False, "error": "测试运行中，不能互换角色"}
            self.reader_id, self.target_id = self.target_id, self.reader_id
            self.phase = "idle"
            self.message = "角色已互换"
        return {"ok": True, "state": self.snapshot()}

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            devices = [asdict(item) for item in self.devices]
            return {
                "devices": devices,
                "reader_id": self.reader_id,
                "target_id": self.target_id,
                "running": self.running,
                "phase": self.phase,
                "message": self.message,
                "attempts": self.attempts,
                "success_count": self.success_count,
                "rounds": self.rounds,
                "last_uid": self.last_uid,
                "expected_uid": TARGET_UID,
                "last_reader_output": self.last_reader_output[-1200:],
                "last_target_output": self.last_target_output[-1200:],
                "started_at": self.started_at,
                "finished_at": self.finished_at,
            }

    def start(self) -> dict[str, Any]:
        with self.lock:
            if self.running:
                return {"ok": False, "error": "已有测试正在运行"}
            reader = self._device(self.reader_id)
            target = self._device(self.target_id)
            if reader is None or target is None or reader == target:
                return {"ok": False, "error": "读卡器/卡模拟端没有正确绑定"}
            self.running = True
            self.phase = "preparing"
            self.message = "正在停止旧 NFC 会话并检查 PN532"
            self.attempts = 0
            self.success_count = 0
            self.rounds = 0
            self.last_uid = None
            self.last_reader_output = ""
            self.last_target_output = ""
            self.started_at = time.time()
            self.finished_at = None
            self.stop_requested.clear()
        worker = threading.Thread(
            target=self._run_test,
            args=(reader, target),
            name="nfc-board-pair-test",
            daemon=True,
        )
        self.worker = worker
        worker.start()
        return {"ok": True, "state": self.snapshot()}

    def stop(self) -> dict[str, Any]:
        with self.lock:
            if self.running:
                self.stop_requested.set()
                self.phase = "stopping"
                self.message = (
                    "正在安全停止：请暂时保持两块天线贴近，等待当前 Target 自然退出"
                )
        return {"ok": True, "state": self.snapshot()}

    def shutdown(self) -> None:
        self.stop_requested.set()
        worker = self.worker
        if worker is not None and worker.is_alive():
            worker.join()

    def _shell(self, device: Device, command: str, timeout: float) -> str:
        return run_process(
            [self.adb, "-t", str(device.transport_id), "shell", command],
            timeout,
        )

    def _finish(self, phase: str, message: str) -> None:
        with self.lock:
            self.running = False
            self.phase = phase
            self.message = message
            self.finished_at = time.time()

    def _spawn_target(self, target: Device) -> subprocess.Popen[str]:
        return subprocess.Popen(
            [
                self.adb,
                "-t",
                str(target.transport_id),
                "shell",
                f"v2hw nfc target {TARGET_WINDOW_MS}",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            env=adb_environment(),
        )

    def _collect_target_safely(
        self,
        target_process: subprocess.Popen[str],
        timeout: float,
    ) -> str:
        """Wait for Target without killing an ADB shell holding the PN532 lock."""

        try:
            output, _ = target_process.communicate(timeout=max(timeout, 0.1))
            return clean_output(output)
        except subprocess.TimeoutExpired:
            with self.lock:
                self.phase = "stopping"
                self.message = (
                    "Target 尚未退出，继续安全等待；不要重开同板 NFC 命令或拔线"
                )
            while target_process.poll() is None:
                try:
                    output, _ = target_process.communicate(timeout=1.0)
                    return clean_output(output)
                except subprocess.TimeoutExpired:
                    continue
            output, _ = target_process.communicate()
            return clean_output(output)

    def _shell_safely(self, device: Device, command: str, timeout: float) -> str:
        """Run a bounded firmware command without killing it at soft timeout."""

        process = subprocess.Popen(
            [self.adb, "-t", str(device.transport_id), "shell", command],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            env=adb_environment(),
        )
        try:
            output, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            with self.lock:
                self.phase = "stopping"
                self.message = f"正在等待命令安全结束：{command}"
            while process.poll() is None:
                try:
                    output, _ = process.communicate(timeout=1.0)
                    break
                except subprocess.TimeoutExpired:
                    continue
            else:
                output, _ = process.communicate()
        return clean_output(output)

    def _peer_enabled(self, device: Device) -> bool:
        output = clean_output(
            self._shell(device, "v2hw nfc peer status", 6.0)
        ).lower()
        if "enabled=1" in output:
            return True
        if "enabled=0" in output or "status=-2" in output or "not ready" in output:
            return False
        raise RuntimeError(f"无法读取 NFC peer 状态：{output[-300:]}")

    def _stop_peer_if_needed(self, device: Device, enabled: bool) -> None:
        if not enabled:
            return
        output = self._shell_safely(
            device,
            "v2hw nfc peer stop",
            PEER_STOP_HOST_TIMEOUT_S,
        ).lower()
        if "nfc peer discovery stopped" not in output:
            raise RuntimeError(f"停止 NFC peer 失败：{output[-300:]}")

    def _restore_peer_if_needed(self, device: Device, enabled: bool) -> None:
        if not enabled:
            return
        output = self._shell_safely(device, "v2hw nfc peer start", 8.0).lower()
        if "nfc peer discovery started" not in output:
            raise RuntimeError(f"恢复 NFC peer 失败：{output[-300:]}")

    def _run_test(self, reader: Device, target: Device) -> None:
        target_process: subprocess.Popen[str] | None = None
        reader_peer_enabled = False
        target_peer_enabled = False
        final_phase = "stopped"
        final_message = "连续识别已停止"
        try:
            # -2/not-ready is expected on boards outside the product peer
            # allowlist.  The generic target/scan path remains available.
            reader_peer_enabled = self._peer_enabled(reader)
            target_peer_enabled = self._peer_enabled(target)
            self._stop_peer_if_needed(reader, reader_peer_enabled)
            self._stop_peer_if_needed(target, target_peer_enabled)
            for name, device in (("读卡器", reader), ("卡模拟端", target)):
                info = self._shell(device, "v2hw nfc info", 6.0)
                if "ic=0x32" not in clean_output(info):
                    raise RuntimeError(f"{name} PN532 info 未通过：{clean_output(info)[-300:]}")

            while not self.stop_requested.is_set():
                with self.lock:
                    self.rounds += 1
                    current_round = self.rounds
                    self.phase = "arming"
                    self.message = f"第 {current_round} 轮：正在重新进入 Target 模式"

                target_process = self._spawn_target(target)
                round_started = time.monotonic()
                # Device-side shell output is buffered until target returns;
                # use a fixed settle delay instead of waiting for "waiting".
                self.stop_requested.wait(TARGET_ARM_DELAY_S)
                deadline = round_started + TARGET_WINDOW_MS / 1000.0 + 3.0
                detected = False

                while target_process.poll() is None and time.monotonic() < deadline:
                    with self.lock:
                        if self.stop_requested.is_set():
                            self.phase = "stopping"
                            self.message = (
                                "正在安全停止：保持天线贴近可让当前 Target 立即退出"
                            )
                        else:
                            self.phase = "scanning"
                            self.message = (
                                f"第 {current_round} 轮扫描中；成功后会自动重新布卡"
                            )

                    output = self._shell(reader, "v2hw nfc scan 5000", 8.0)
                    result = parse_scan_result(output)
                    with self.lock:
                        self.attempts += 1
                        self.last_reader_output = clean_output(output)
                        if result:
                            self.last_uid = result["uid"]
                    if result and result["uid"] == TARGET_UID:
                        with self.lock:
                            self.success_count += 1
                            self.phase = "success"
                            self.message = (
                                f"第 {self.success_count} 次识别成功；正在自动重新布卡"
                            )
                        detected = True
                        break

                    # Stop is cooperative.  Keep scanning this last window so
                    # a nearby Reader releases the one-shot Target promptly.
                    if self.stop_requested.is_set():
                        time.sleep(SCAN_INTERVAL_S)
                    else:
                        self.stop_requested.wait(SCAN_INTERVAL_S)

                target_output = self._collect_target_safely(
                    target_process,
                    deadline - time.monotonic() + 3.0,
                )
                target_process = None
                with self.lock:
                    self.last_target_output = target_output

                if self.stop_requested.is_set():
                    final_message = (
                        f"连续识别已停止：共成功 {self.success_count} 次，"
                        f"扫描 {self.attempts} 次"
                    )
                    break

                if detected:
                    self.stop_requested.wait(TARGET_REARM_DELAY_S)
                    continue

                elapsed = time.monotonic() - round_started
                normalized_target = target_output.lower()
                if (
                    "timeout" in normalized_target
                    or "activated" in normalized_target
                    or elapsed >= TARGET_WINDOW_MS / 1000.0 - 1.0
                ):
                    with self.lock:
                        self.phase = "arming"
                        self.message = "本轮未命中，正在自动重新布卡"
                    self.stop_requested.wait(TARGET_REARM_DELAY_S)
                    continue

                raise RuntimeError(
                    "卡模拟端提前异常退出：" + target_output[-300:]
                )
        except (OSError, subprocess.SubprocessError, RuntimeError) as error:
            final_phase = "error"
            final_message = str(error)
        finally:
            # Never terminate an active target shell early: the device-side
            # ADB backend may force-delete a task holding the PN532 mutex.
            # The firmware command has its own bounded Target timeout.
            if target_process is not None and target_process.poll() is None:
                try:
                    output = self._collect_target_safely(
                        target_process,
                        TARGET_WINDOW_MS / 1000.0 + 5.0,
                    )
                    with self.lock:
                        self.last_target_output = output
                except (OSError, subprocess.SubprocessError) as error:
                    final_phase = "error"
                    final_message = f"Target 安全收尾失败：{error}"

            # Restore only peer workers that were running before this test.
            # Reverse order avoids reactivating the Reader while Target is
            # still being restored.
            for device, enabled in (
                (target, target_peer_enabled),
                (reader, reader_peer_enabled),
            ):
                try:
                    self._restore_peer_if_needed(device, enabled)
                except (OSError, subprocess.SubprocessError, RuntimeError) as error:
                    final_phase = "error"
                    final_message = str(error)
            self._finish(final_phase, final_message)


HTML = r"""<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ARCS 双板 NFC 测试</title>
<style>
body{margin:0;background:#07111f;color:#eaf3ff;font:16px system-ui,sans-serif}main{max-width:920px;margin:32px auto;padding:0 18px}h1{font-size:34px;margin:0 0 8px}.sub{color:#8fa6c2}.grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin:24px 0}.card,.panel{background:#0e1b2d;border:1px solid #263b57;border-radius:18px;padding:20px}.role{color:#77c4ff;font-weight:800}.id{font:18px ui-monospace,monospace;margin:10px 0}.usb{color:#879bb5}button{border:1px solid #3b587c;background:#172a43;color:#eaf3ff;border-radius:12px;padding:12px 18px;font:inherit;cursor:pointer;margin:0 8px 8px 0}button.primary{background:#14764a;border-color:#32ce82;font-weight:800}button.stop{background:#71303a;border-color:#e26773}button:disabled{opacity:.4;cursor:not-allowed}.hero{text-align:center;padding:32px 20px}.phase{font-size:30px;font-weight:900}.msg{color:#9cb2cd;margin:10px}.uid{font:26px ui-monospace,monospace;color:#62dfff}.success{border-color:#3fe58b;box-shadow:0 0 45px #28d67b35}.error,.timeout{border-color:#ef6d78}.stats{display:flex;justify-content:center;gap:34px;margin-top:18px}.stats b{display:block;font-size:30px}.stats span{color:#8196b0}pre{white-space:pre-wrap;max-height:170px;overflow:auto;background:#07101c;padding:12px;border-radius:10px;color:#93a8c3;font-size:12px}@media(max-width:700px){.grid{grid-template-columns:1fr}}
</style><main><h1>双板 NFC 卡 / 读卡测试</h1><div class="sub">使用原始 PN532 Target + Scan，不依赖固定 peer ID</div>
<div class="grid"><div class="card"><div class="role">读卡器 Reader</div><div id="reader"></div></div><div class="card"><div class="role">卡模拟端 Target</div><div id="target"></div></div></div>
<div><button onclick="post('/api/refresh')">重新识别两块板</button><button onclick="post('/api/swap')">互换角色</button><button id="start" class="primary" onclick="post('/api/start')">开始连续识别</button><button id="stop" class="stop" onclick="post('/api/stop')">停止</button></div>
<section id="hero" class="panel hero"><div id="phase" class="phase">准备中</div><div id="msg" class="msg"></div><div id="uid" class="uid"></div><div class="stats"><div><b id="attempts">0</b><span>扫描次数</span></div><div><b id="successes">0</b><span>成功次数</span></div></div></section>
<details class="panel"><summary>诊断输出</summary><h4>Reader</h4><pre id="readerOut"></pre><h4>Target</h4><pre id="targetOut"></pre></details>
<script>
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const labels={idle:'准备就绪',preparing:'检查 PN532',arming:'重新布卡',scanning:'连续扫描中',success:'识别成功',stopping:'安全停止中',stopped:'已停止',timeout:'识别超时',error:'测试异常'};
function board(s,id){const d=(s.devices||[]).find(x=>x.chip_id===id);return d?`<div class="id">${esc(d.chip_id)}</div><div class="usb">USB ${esc(d.usb)} · transport ${esc(d.transport_id)}</div>`:'<div class="msg">未绑定</div>'}
async function post(path){const r=await fetch(path,{method:'POST'});const x=await r.json();if(!x.ok&&x.error)alert(x.error);await refresh()}
async function refresh(){const s=await (await fetch('/api/state')).json();document.getElementById('reader').innerHTML=board(s,s.reader_id);document.getElementById('target').innerHTML=board(s,s.target_id);document.getElementById('phase').textContent=labels[s.phase]||s.phase;document.getElementById('msg').textContent=s.message||'';document.getElementById('uid').textContent=s.last_uid?`UID ${s.last_uid}`:`目标 UID ${s.expected_uid}`;document.getElementById('attempts').textContent=s.attempts||0;document.getElementById('successes').textContent=s.success_count||0;document.getElementById('readerOut').textContent=s.last_reader_output||'';document.getElementById('targetOut').textContent=s.last_target_output||'';document.getElementById('start').disabled=s.running||(s.devices||[]).length!==2;document.getElementById('stop').disabled=!s.running;document.getElementById('hero').className=`panel hero ${s.phase}`}
setInterval(refresh,500);refresh();
</script></main></html>"""


def make_handler(controller: PairController) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        def _json(self, value: Any, status: int = 200) -> None:
            payload = json.dumps(value, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self) -> None:  # noqa: N802
            if self.path == "/":
                payload = HTML.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            elif self.path == "/api/state":
                self._json(controller.snapshot())
            else:
                self.send_error(404)

        def do_POST(self) -> None:  # noqa: N802
            if self.path == "/api/refresh":
                result = controller.refresh()
            elif self.path == "/api/swap":
                result = controller.swap()
            elif self.path == "/api/start":
                result = controller.start()
            elif self.path == "/api/stop":
                result = controller.stop()
            else:
                self.send_error(404)
                return
            self._json(result, 200 if result.get("ok") else 409)

        def log_message(self, format: str, *args: Any) -> None:
            return

    return Handler


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="双板 PN532 卡/读卡上位机")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8770)
    parser.add_argument("--no-browser", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    controller = PairController(args.adb)
    controller.refresh()
    server = ThreadingHTTPServer(
        (args.host, args.port), make_handler(controller)
    )
    server.daemon_threads = True
    url = f"http://{args.host}:{args.port}/"
    print(f"双板 NFC 测试上位机：{url}", flush=True)
    if not args.no_browser:
        try:
            webbrowser.open(url)
        except OSError:
            pass
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        controller.shutdown()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
