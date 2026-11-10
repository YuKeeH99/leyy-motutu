#!/usr/bin/env python3
"""Single-page dual-microphone direction dashboard.

The firmware exposes one machine-readable snapshot per ``tdoa status`` call as a
``TDOA_JSON`` line.  This tool keeps a persistent ADB shell open and issues that
command a few times per second, so the displayed direction comes from the
board-side localization result rather than any host-side processing.

Side values follow ``app_audio_localization_side_e``: 1 is MIC0 (physical left),
2 is CENTER, 3 is MIC1 (physical front).  A positive ``d`` means MIC0 received
the sound first.  ``v=0`` means no coherent speech, not an error.
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
from typing import Any, Sequence


POLL_INTERVAL = 0.35
STALE_AFTER = 2.5
MAX_HISTORY = 240

SIDE_UNKNOWN = 0
SIDE_MIC0 = 1
SIDE_CENTER = 2
SIDE_MIC1 = 3

SIDE_LABELS = {
    SIDE_UNKNOWN: "无声源",
    SIDE_MIC0: "左",
    SIDE_CENTER: "中",
    SIDE_MIC1: "前",
}

ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
JSON_RE = re.compile(r"TDOA_JSON\s*(?P<body>\{.*\})")


def adb_environment() -> dict[str, str]:
    env = os.environ.copy()
    # The libusb backend collapses boards that share an ADB serial. The native
    # Linux backend preserves distinct transport IDs.
    env["ADB_LIBUSB"] = "0"
    return env


def clean_line(line: str) -> str:
    return ANSI_RE.sub("", line).replace("\r", "").strip()


def parse_snapshot(line: str) -> dict[str, Any] | None:
    match = JSON_RE.search(clean_line(line))
    if match is None:
        return None
    try:
        payload = json.loads(match.group("body"))
    except json.JSONDecodeError:
        return None
    if not isinstance(payload, dict) or "s" not in payload:
        return None
    return payload


def discover_transports(adb: str) -> list[dict[str, Any]]:
    completed = subprocess.run(
        [adb, "devices", "-l"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=3.0,
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


def delay_microseconds(delay_milli: int, sample_rate: int) -> float:
    """Convert a milli-sample delay into microseconds."""
    if sample_rate <= 0:
        return 0.0
    return delay_milli / 1000.0 / sample_rate * 1_000_000.0


class DirectionMonitor:
    """Polls ``tdoa status`` over one long-lived ADB shell and fans out results."""

    def __init__(self, adb: str, transport_id: int | None, sample_rate: int) -> None:
        self.adb = adb
        self.transport_id = transport_id
        self.sample_rate = sample_rate
        self.process: subprocess.Popen[str] | None = None
        self.lock = threading.Lock()
        self.channels: list[queue.Queue[dict[str, Any]]] = []
        self.running = False
        self.threads: list[threading.Thread] = []
        self.last_sequence: int | None = None
        self.latest: dict[str, Any] | None = None
        self.last_valid: dict[str, Any] | None = None
        self.history: list[dict[str, Any]] = []
        self.error: str | None = None
        self.counts = {"left": 0, "center": 0, "front": 0}

    # -- subscription -----------------------------------------------------
    def subscribe(self) -> queue.Queue[dict[str, Any]]:
        channel: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=64)
        with self.lock:
            self.channels.append(channel)
            self._publish_locked(only=channel)
        return channel

    def unsubscribe(self, channel: queue.Queue[dict[str, Any]]) -> None:
        with self.lock:
            if channel in self.channels:
                self.channels.remove(channel)

    def _snapshot_locked(self) -> dict[str, Any]:
        now = time.monotonic()
        latest = self.latest
        stale = latest is None or (now - float(latest.get("host_time", 0.0))) > STALE_AFTER
        return {
            "transport_id": self.transport_id,
            "sample_rate": self.sample_rate,
            "running": self.running,
            "error": self.error,
            "stale": bool(stale),
            "latest": latest,
            "last_valid": self.last_valid,
            "counts": dict(self.counts),
            "history": list(self.history[-MAX_HISTORY:]),
        }

    def _publish_locked(self, only: queue.Queue[dict[str, Any]] | None = None) -> None:
        payload = self._snapshot_locked()
        targets = [only] if only is not None else list(self.channels)
        for channel in targets:
            try:
                channel.put_nowait(payload)
            except queue.Full:
                pass

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return self._snapshot_locked()

    # -- lifecycle --------------------------------------------------------
    def resolve_transport(self) -> None:
        if self.transport_id is not None:
            return
        devices = discover_transports(self.adb)
        if not devices:
            raise RuntimeError("没有在线的 ADB 设备，先确认板子已连上")
        if len(devices) > 1:
            listed = "、".join(f"t{item['transport_id']}({item['usb']})" for item in devices)
            raise RuntimeError(f"检测到多个设备：{listed}；用 --transport 明确指定一个")
        self.transport_id = int(devices[0]["transport_id"])

    def start(self) -> None:
        self.resolve_transport()
        self.process = subprocess.Popen(
            [self.adb, "-t", str(self.transport_id), "shell"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            bufsize=1,
            env=adb_environment(),
        )
        self.running = True
        for target in (self._read_loop, self._poll_loop):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self.threads.append(thread)

    def stop(self) -> None:
        self.running = False
        process, self.process = self.process, None
        if process is None:
            return
        try:
            if process.stdin and not process.stdin.closed:
                process.stdin.write("exit\n")
                process.stdin.flush()
        except (OSError, ValueError):
            pass
        try:
            process.wait(timeout=1.5)
        except subprocess.TimeoutExpired:
            process.kill()

    # -- worker threads ---------------------------------------------------
    def _poll_loop(self) -> None:
        while self.running:
            process = self.process
            if process is None or process.stdin is None or process.poll() is not None:
                self._set_error("ADB shell 已退出，重启工具或检查 USB 连接")
                return
            try:
                process.stdin.write("tdoa status\n")
                process.stdin.flush()
            except (OSError, ValueError):
                self._set_error("向 ADB shell 写入失败")
                return
            time.sleep(POLL_INTERVAL)

    def _read_loop(self) -> None:
        process = self.process
        if process is None or process.stdout is None:
            return
        try:
            for raw in process.stdout:
                if not self.running:
                    break
                payload = parse_snapshot(raw)
                if payload is not None:
                    self._on_snapshot(payload)
        except (OSError, ValueError):
            pass

    def _set_error(self, message: str) -> None:
        with self.lock:
            self.error = message
            self.running = False
            self._publish_locked()

    def _on_snapshot(self, payload: dict[str, Any]) -> None:
        sequence = payload.get("s")
        with self.lock:
            if sequence == self.last_sequence:
                return
            self.last_sequence = sequence
            record = self._decorate(payload)
            self.latest = record
            if record["valid"]:
                self.last_valid = record
                key = {SIDE_MIC0: "left", SIDE_CENTER: "center", SIDE_MIC1: "front"}.get(
                    int(record["side"])
                )
                if key is not None:
                    self.counts[key] += 1
                self.history.append(record)
                del self.history[:-MAX_HISTORY]
            self.error = None
            self._publish_locked()

    def _decorate(self, payload: dict[str, Any]) -> dict[str, Any]:
        side = int(payload.get("x", SIDE_UNKNOWN))
        delay_milli = int(payload.get("d", 0))
        return {
            "sequence": payload.get("s"),
            "valid": bool(payload.get("v")),
            "expired": bool(payload.get("e")),
            "side": side,
            "side_label": SIDE_LABELS.get(side, "未知"),
            "delay_milli": delay_milli,
            "delay_us": round(delay_microseconds(delay_milli, self.sample_rate), 1),
            "corr_milli": int(payload.get("c", 0)),
            "active_blocks": int(payload.get("a", 0)),
            "level_mic0": int(payload.get("m0", 0)),
            "level_mic1": int(payload.get("m1", 0)),
            "peak_mic0": int(payload.get("p0", 0)),
            "peak_mic1": int(payload.get("p1", 0)),
            "balance_milli": int(payload.get("b", 0)),
            "fused": bool(payload.get("f")),
            "host_time": time.monotonic(),
        }


HTML = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>双麦方向识别</title>
<style>
:root {
  color-scheme: dark;
  --bg: #0e1116;
  --panel: #171b22;
  --line: #262c36;
  --text: #e6edf3;
  --muted: #8b949e;
  --live: #3fb950;
  --idle: #30363d;
  --warn: #d29922;
}
* { box-sizing: border-box; }
body {
  margin: 0; padding: 24px;
  background: var(--bg); color: var(--text);
  font: 15px/1.5 system-ui, -apple-system, "Noto Sans CJK SC", sans-serif;
}
.wrap { max-width: 860px; margin: 0 auto; }
header { display: flex; align-items: baseline; gap: 12px; margin-bottom: 4px; }
h1 { font-size: 19px; font-weight: 600; margin: 0; }
.sub { color: var(--muted); font-size: 13px; }
.state { margin: 14px 0 18px; font-size: 13px; color: var(--muted); }
.state b { color: var(--text); font-weight: 600; }
.state.err { color: #f85149; }
.zones { display: grid; grid-template-columns: repeat(3, 1fr); gap: 12px; }
.zone {
  background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
  padding: 26px 12px; text-align: center;
  transition: background .12s, border-color .12s, transform .12s;
}
.zone .name { font-size: 34px; font-weight: 700; letter-spacing: 4px; }
.zone .hint { color: var(--muted); font-size: 12px; margin-top: 6px; }
.zone.on {
  background: #10331c; border-color: var(--live);
  transform: translateY(-2px);
}
.zone.on .name { color: var(--live); }
.zone.hold { border-color: #2b4a34; }
.grid {
  display: grid; grid-template-columns: repeat(auto-fit, minmax(132px, 1fr));
  gap: 10px; margin-top: 18px;
}
.cell {
  background: var(--panel); border: 1px solid var(--line);
  border-radius: 8px; padding: 10px 12px;
}
.cell .k { color: var(--muted); font-size: 12px; }
.cell .v { font-size: 18px; font-weight: 600; font-variant-numeric: tabular-nums; }
.cell .v small { color: var(--muted); font-size: 12px; font-weight: 400; }
.bars { margin-top: 18px; background: var(--panel);
  border: 1px solid var(--line); border-radius: 8px; padding: 14px; }
.bar { display: flex; align-items: center; gap: 10px; margin: 8px 0; }
.bar .lbl { width: 96px; color: var(--muted); font-size: 12px; }
.bar .track { flex: 1; height: 10px; background: var(--idle);
  border-radius: 5px; overflow: hidden; }
.bar .fill { height: 100%; background: var(--live); width: 0; transition: width .12s; }
.bar .num { width: 52px; text-align: right;
  font-variant-numeric: tabular-nums; font-size: 13px; }
.tally { margin-top: 16px; color: var(--muted); font-size: 13px; }
.tally span { color: var(--text); font-variant-numeric: tabular-nums; }
footer { margin-top: 20px; color: var(--muted); font-size: 12px; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>双麦方向识别</h1>
    <span class="sub" id="sub">连接中…</span>
  </header>
  <div class="state" id="state">等待固件上报</div>

  <div class="zones">
    <div class="zone" id="z1"><div class="name">左</div><div class="hint">MIC0 先到</div></div>
    <div class="zone" id="z2"><div class="name">中</div><div class="hint">两侧相当</div></div>
    <div class="zone" id="z3"><div class="name">前</div><div class="hint">MIC1 先到</div></div>
  </div>

  <div class="grid">
    <div class="cell"><div class="k">时延</div>
      <div class="v" id="delay">—</div></div>
    <div class="cell"><div class="k">相关性</div>
      <div class="v" id="corr">—</div></div>
    <div class="cell"><div class="k">有效块</div>
      <div class="v" id="active">—</div></div>
    <div class="cell"><div class="k">ILD 电平差</div>
      <div class="v" id="balance">—</div></div>
    <div class="cell"><div class="k">ILD 融合</div>
      <div class="v" id="fused">—</div></div>
    <div class="cell"><div class="k">快照序号</div>
      <div class="v" id="seq">—</div></div>
  </div>

  <div class="bars">
    <div class="bar"><div class="lbl">MIC0 均值</div>
      <div class="track"><div class="fill" id="b0"></div></div>
      <div class="num" id="n0">0</div></div>
    <div class="bar"><div class="lbl">MIC1 均值</div>
      <div class="track"><div class="fill" id="b1"></div></div>
      <div class="num" id="n1">0</div></div>
  </div>

  <div class="tally" id="tally">累计判定：左 <span>0</span> · 中 <span>0</span> · 前 <span>0</span></div>
  <footer>
    方向来自固件 <code>tdoa status</code> 的 TDOA_JSON 快照；时延为正表示 MIC0 先收到声音。
    相关性与有效块未达门限时固件不给方向，页面显示为静默。
  </footer>
</div>
<script>
const LIVE_MS = 1500;
const HOLD_MS = 4000;
const LEVEL_FULL = 600;
const el = (id) => document.getElementById(id);
const zones = { 1: el("z1"), 2: el("z2"), 3: el("z3") };
let lastValidAt = 0;
let lastValidSide = 0;

function paintZones(side, cls) {
  for (const key of [1, 2, 3]) {
    zones[key].className = "zone" + (Number(key) === Number(side) ? " " + cls : "");
  }
}

function fmtBalance(value) {
  const sign = value > 0 ? "+" : "";
  const who = value > 0 ? "MIC0 强" : value < 0 ? "MIC1 强" : "持平";
  return sign + value + " <small>" + who + "</small>";
}

function render(state) {
  const sub = el("sub");
  sub.textContent = state.transport_id === null
    ? "未绑定设备"
    : "transport " + state.transport_id + " · " + state.sample_rate + " Hz";

  const stateBox = el("state");
  const now = performance.now();
  const latest = state.latest;

  if (latest && latest.valid) {
    lastValidAt = now;
    lastValidSide = latest.side;
  }
  const since = now - lastValidAt;

  if (state.error) {
    stateBox.className = "state err";
    stateBox.textContent = state.error;
    paintZones(0, "on");
  } else if (lastValidAt && since < LIVE_MS) {
    stateBox.className = "state";
    stateBox.innerHTML = "识别到声源方向：<b>" + (SIDE_TEXT[lastValidSide] || "未知") + "</b>";
    paintZones(lastValidSide, "on");
  } else if (lastValidAt && since < HOLD_MS) {
    stateBox.className = "state";
    stateBox.innerHTML = "保持上一次判定：<b>" + (SIDE_TEXT[lastValidSide] || "未知") + "</b>";
    paintZones(lastValidSide, "hold");
  } else {
    stateBox.className = "state";
    stateBox.textContent = state.stale
      ? "没有收到新快照，检查 ADB 连接"
      : "静默中，说话即可出方向";
    paintZones(0, "on");
  }

  if (latest) {
    el("delay").innerHTML = latest.delay_milli + " <small>m-smp / "
      + latest.delay_us + " µs</small>";
    el("corr").innerHTML = (latest.corr_milli / 1000).toFixed(3);
    el("active").innerHTML = latest.active_blocks + " <small>/ 63</small>";
    el("balance").innerHTML = fmtBalance(latest.balance_milli);
    el("fused").textContent = latest.fused ? "已融合" : "未融合";
    el("seq").textContent = latest.sequence;
    el("n0").textContent = latest.level_mic0;
    el("n1").textContent = latest.level_mic1;
    el("b0").style.width =
      Math.min(100, (latest.level_mic0 / LEVEL_FULL) * 100) + "%";
    el("b1").style.width =
      Math.min(100, (latest.level_mic1 / LEVEL_FULL) * 100) + "%";
  }

  const c = state.counts || { left: 0, center: 0, front: 0 };
  el("tally").innerHTML = "累计判定：左 <span>" + c.left
    + "</span> · 中 <span>" + c.center + "</span> · 前 <span>" + c.front + "</span>";
}

const SIDE_TEXT = { 0: "无声源", 1: "左", 2: "中", 3: "前" };

const source = new EventSource("/events");
source.onmessage = (event) => {
  try {
    render(JSON.parse(event.data));
  } catch (err) {
    console.error(err);
  }
};
source.onerror = () => {
  el("state").className = "state err";
  el("state").textContent = "与上位机的连接中断，刷新页面重试";
};
setInterval(() => {
  fetch("/api/status").then((r) => r.json()).then(render).catch(() => {});
}, 1000);
</script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    monitor: DirectionMonitor

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
            self._json(self.monitor.snapshot())
            return
        if self.path == "/events":
            channel = self.monitor.subscribe()
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.end_headers()
                while True:
                    try:
                        payload = channel.get(timeout=15.0)
                        data = json.dumps(payload, ensure_ascii=False)
                        self.wfile.write(f"data: {data}\n\n".encode("utf-8"))
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                self.monitor.unsubscribe(channel)
            return
        self.send_error(404)

    def log_message(self, format: str, *args: Any) -> None:
        return


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="双麦方向识别上位机")
    parser.add_argument("--adb", default="adb", help="adb 可执行文件")
    parser.add_argument(
        "--transport", type=int, default=None,
        help="板子的 adb transport_id；只有一台在线时可省略",
    )
    parser.add_argument(
        "--sample-rate", type=int, default=16000,
        help="麦克风采样率，仅用于把时延换算成微秒",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--no-browser", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)

    monitor = DirectionMonitor(args.adb, args.transport, args.sample_rate)
    try:
        monitor.start()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"启动失败：{exc}", flush=True)
        return 1

    Handler.monitor = monitor
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    url = f"http://{args.host}:{args.port}/"
    print(f"双麦方向识别上位机：{url}", flush=True)
    print(
        f"transport={monitor.transport_id} 轮询间隔={POLL_INTERVAL}s；"
        "对着板子说话即可看到方向",
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
        monitor.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
