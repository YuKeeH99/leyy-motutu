#!/usr/bin/env python3
"""ARCS-MINI V2.1 单页综合测试上位机。

一个页面同时覆盖三类功能：

* 主动控制：WS2812 开关/颜色、舵机通道与脉宽（默认锁定，需要先解锁）；
* 被动读数：IMU、触摸、NFC、声源定位（TDOA）；
* 云端语音对话：唤醒 / ASR / LLM / TTS 事件流，以及 ``interact start|stop``。

所有数据都来自板子自己的命令输出，主机不做任何二次推算。读命令走一条常驻
``adb shell``：每条回显都自带前缀（``TDOA_JSON``、``touch PA09:`` 等），所以
交错输出也能各自归位；同一条流还顺带拿到设备日志，用来还原云端对话状态。
控制命令走一次性 ``adb shell "<cmd>"``，避免污染常驻流。

PN532 在高频轮询下会出现 ``-3/-4``，所以 NFC 只做低频 info 轮询，读卡和互认
一律由页面上的按钮显式触发。
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


FAST_INTERVAL = 0.12
FAST_ROTATION = ("v2hw mpu", "tdoa status", "v2hw mpu", "v2hw touch all")
SLOW_INTERVAL = 1.5
NFC_INTERVAL = 10.0
STALE_AFTER = 3.0
MAX_HISTORY = 240
MAX_EVENTS = 160
COMMAND_TIMEOUT = 20.0

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

SERVO_COUNT = 4
SERVO_MIN_PULSE_US = 500
SERVO_CENTER_PULSE_US = 1500
SERVO_MAX_PULSE_US = 2500
SERVO_SWEEP_MIN_PERIOD_MS = 400
SERVO_SWEEP_MAX_PERIOD_MS = 8000
SERVO_SWEEP_DEF_PERIOD_MS = 1600
SERVO_PADS = ("PA04", "PA05", "PA06", "PA07")
TOUCH_PADS = ("PA09", "PA13")

ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
TDOA_JSON_RE = re.compile(r"TDOA_JSON\s*(?P<body>\{.*\})")
TOUCH_RE = re.compile(r"touch\s+PA(?P<pad>\d+):\s*status=(?P<status>-?\d+)\s*\((?P<text>[^)]*)\)\s*level=(?P<level>\d+)")
MPU_RE = re.compile(
    r"mpu6050\s+\S+:\s*status=(?P<status>-?\d+)\s*\((?P<text>[^)]*)\)"
    r"(?:\s*who=(?P<who>0x[0-9a-fA-F]+))?"
    r"(?:\s*accel=(?P<ax>-?\d+),(?P<ay>-?\d+),(?P<az>-?\d+))?"
    r"(?:\s*temp=(?P<temp>-?\d+))?"
    r"(?:\s*gyro=(?P<gx>-?\d+),(?P<gy>-?\d+),(?P<gz>-?\d+))?"
)
PN532_RE = re.compile(
    r"pn532\s+\S+:\s*status=(?P<status>-?\d+)\s*\((?P<text>[^)]*)\)"
    r"(?:\s*ic=(?P<ic>0x[0-9a-fA-F]+))?"
    r"(?:\s*fw=(?P<fw>[\d.]+))?"
    r"(?:\s*support=(?P<support>0x[0-9a-fA-F]+))?"
)
POWER_RE = re.compile(
    r"VBAT/servo\s+pin2=(?P<millivolt>-?\d+)mV\s+level=(?P<level>\S+)\s+battery=(?P<battery>\S+)"
)
BOARD_RE = re.compile(r"board=(?P<board>\S+)\s+actuators=(?P<actuators>\S+)\s+servos=(?P<servos>\S+)")
# ``v2hw status`` appends ``sweep=`` and ``v2hw servo sweep-status`` prints its
# own line, so either one can refresh the switch without a dedicated poll.
NETCFG_PORTAL_RE = re.compile(
    r"netcfg\s+portal=(?P<portal>\S+)\s+ssid=(?P<ssid>\S+)\s+ip=(?P<ip>\S+)\s+http=(?P<http>\d+)\s+dns=(?P<dns>\d+)"
)
NETCFG_WIFI_RE = re.compile(
    r"wifi\s+mode=(?P<mode>-?\d+)\s+available=(?P<available>\S+)\s+started=(?P<started>\S+)\s+connected=(?P<connected>\S+)\s+"
    r"saved_ap=(?P<saved_ap>\S+)\s+switching=(?P<switching>\S+)\s+bearer=(?P<bearer>-?\d+)\s+result=(?P<result>-?\d+)\s+ota=(?P<ota>-?\d+)"
)
SWEEP_FIELD_RE = re.compile(r"sweep=(?P<state>running|stopped)")
SWEEP_LINE_RE = re.compile(
    r"servo sweep:\s*(?P<state>running|stopped)(?:\s+channel=(?P<channel>\d+))?"
)
VOLUME_RE = re.compile(
    r"volume=(?P<volume>\d+)\s+min=(?P<min>\d+)\s+muted=(?P<muted>yes|no)"
)
UID_RE = re.compile(r"uid=(?P<uid>(?:[0-9a-fA-F]{2}\s*)+)")

GRAVITY_LSB = 16384.0

# ``v2_shell_cmd`` closes every dispatch with "ok" or "v2hw command failed: N".
COMMAND_END_RE = re.compile(r"^(ok|v2hw command failed: -?\d+)$")
COMMAND_SETTLE = 0.45

# ``v2_battery_status_name`` in service_v2_shell.c emits exactly these five.
BATTERY_LABELS = {
    "no-battery": "未接电池",
    "not-charging": "未充电",
    "charging": "充电中",
    "charged": "已充满",
    "unknown": "未知",
}

CLOUD_PATTERNS: tuple[tuple[str, str, str], ...] = (
    ("wakeup", r"voice_wakeup|wakeup word|wakeup_algo hit|keyword hit", "唤醒"),
    ("asr", r"voice_cloud_asr|asr_txt|stream-text", "语音识别"),
    ("llm", r"voice_cloud_nlp|llm_txt|chat_txt", "大模型"),
    ("tts", r"voice_cloud_tts_txt|tts_player|TTS", "语音合成"),
    ("session", r"lisa_ws_connect|lisa_ws_disconnect|interaction|int_mode", "会话"),
    ("network", r"voice_wifi_|reconnect start|sntp|got ip", "网络"),
    ("error", r"鉴权失败|auth fail|timeout|error|failed|-\d+ fail", "异常"),
)
CLOUD_COMPILED = tuple(
    (key, re.compile(pattern, re.IGNORECASE), label) for key, pattern, label in CLOUD_PATTERNS
)


def adb_environment() -> dict[str, str]:
    env = os.environ.copy()
    # The libusb backend collapses boards that share an ADB serial, so the
    # native Linux backend is the default because it keeps transport IDs
    # distinct. It does go blind after some re-enumerations though, and then
    # libusb is the only backend that sees the board at all -- so an
    # ADB_LIBUSB already set in the environment wins.
    env.setdefault("ADB_LIBUSB", "0")
    return env


def hidden_subprocess_kwargs() -> dict[str, Any]:
    """Keep packaged Windows ADB operations inside the GUI process."""
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NO_WINDOW}
    return {}


def clean_line(line: str) -> str:
    return ANSI_RE.sub("", line).replace("\r", "").strip()


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
        **hidden_subprocess_kwargs(),
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


def parse_tdoa(line: str) -> dict[str, Any] | None:
    match = TDOA_JSON_RE.search(line)
    if match is None:
        return None
    try:
        payload = json.loads(match.group("body"))
    except json.JSONDecodeError:
        return None
    if not isinstance(payload, dict) or "s" not in payload:
        return None
    return payload


def parse_touch(line: str) -> dict[str, Any] | None:
    match = TOUCH_RE.search(line)
    if match is None:
        return None
    level = int(match.group("level"))
    return {
        "pad": f"PA{match.group('pad')}",
        "status": int(match.group("status")),
        "status_text": match.group("text"),
        "level": level,
        "touched": level != 0,
    }


def parse_mpu(line: str) -> dict[str, Any] | None:
    match = MPU_RE.search(line)
    if match is None:
        return None
    record: dict[str, Any] = {
        "status": int(match.group("status")),
        "status_text": match.group("text"),
        "who": match.group("who"),
    }
    if match.group("ax") is not None:
        accel = (int(match.group("ax")), int(match.group("ay")), int(match.group("az")))
        record["accel"] = list(accel)
        magnitude = (accel[0] ** 2 + accel[1] ** 2 + accel[2] ** 2) ** 0.5
        record["accel_g"] = round(magnitude / GRAVITY_LSB, 3)
    if match.group("gx") is not None:
        record["gyro"] = [int(match.group("gx")), int(match.group("gy")), int(match.group("gz"))]
    if match.group("temp") is not None:
        record["temp_raw"] = int(match.group("temp"))
    return record


def parse_pn532(line: str) -> dict[str, Any] | None:
    match = PN532_RE.search(line)
    if match is None:
        return None
    return {
        "status": int(match.group("status")),
        "status_text": match.group("text"),
        "ic": match.group("ic"),
        "firmware": match.group("fw"),
        "support": match.group("support"),
    }


def parse_power(line: str) -> dict[str, Any] | None:
    match = POWER_RE.search(line)
    if match is None:
        return None
    millivolt = int(match.group("millivolt"))
    battery = match.group("battery")
    level = match.group("level")
    percent: int | None = None
    if level.endswith("%"):
        try:
            percent = int(level[:-1])
        except ValueError:
            percent = None
    return {
        "millivolt": millivolt,
        "volt": round(millivolt / 1000.0, 3),
        "level": level,
        "percent": percent,
        "battery": battery,
        "battery_label": BATTERY_LABELS.get(battery, battery),
        "charging": battery == "charging",
        "charge_done": battery == "charged",
        "present": battery not in {"no-battery", "n/a", "none", "unknown"},
    }


def parse_board(line: str) -> dict[str, Any] | None:
    match = BOARD_RE.search(line)
    if match is None:
        return None
    record = {
        "board": match.group("board"),
        "actuators": match.group("actuators"),
        "servos": match.group("servos"),
    }
    sweep = SWEEP_FIELD_RE.search(line)
    if sweep is not None:
        record["sweep"] = sweep.group("state")
    return record


def parse_netcfg_portal(line: str) -> dict[str, Any] | None:
    match = NETCFG_PORTAL_RE.search(line)
    if match is None:
        return None
    return {
        "portal_active": match.group("portal") == "active",
        "portal": match.group("portal"),
        "ssid": match.group("ssid"),
        "ip": match.group("ip"),
        "http": int(match.group("http")),
        "dns": int(match.group("dns")),
    }


def parse_netcfg_wifi(line: str) -> dict[str, Any] | None:
    match = NETCFG_WIFI_RE.search(line)
    if match is None:
        return None
    return {
        "wifi_mode": int(match.group("mode")),
        "wifi_available": match.group("available") == "yes",
        "wifi_started": match.group("started") == "yes",
        "wifi_connected": match.group("connected") == "yes",
        "saved_ap": match.group("saved_ap") == "yes",
        "switching": match.group("switching") == "yes",
        "bearer": int(match.group("bearer")),
        "connect_result": int(match.group("result")),
        "ota_state": int(match.group("ota")),
    }


def parse_sweep(line: str) -> dict[str, Any] | None:
    """Read the dedicated ``servo sweep:`` line, which also carries the channel."""
    match = SWEEP_LINE_RE.search(line)
    if match is None:
        return None
    channel = match.group("channel")
    return {
        "running": match.group("state") == "running",
        "channel": None if channel is None else int(channel),
    }


def parse_volume(line: str) -> dict[str, Any] | None:
    match = VOLUME_RE.search(line)
    if match is None:
        return None
    return {
        "volume": int(match.group("volume")),
        "min": int(match.group("min")),
        "muted": match.group("muted") == "yes",
    }


def parse_card_uid(line: str) -> str | None:
    match = UID_RE.search(line)
    if match is None:
        return None
    return " ".join(match.group("uid").split()).upper()


def classify_cloud_event(line: str) -> tuple[str, str] | None:
    """Map a device log line onto one cloud-dialogue stage, most specific first."""
    for key, pattern, label in CLOUD_COMPILED:
        if pattern.search(line):
            return key, label
    return None


def clamp_pulse(pulse_us: Any) -> int:
    try:
        value = int(pulse_us)
    except (TypeError, ValueError):
        return SERVO_CENTER_PULSE_US
    return max(SERVO_MIN_PULSE_US, min(SERVO_MAX_PULSE_US, value))


def clamp_channel(channel: Any) -> int:
    try:
        value = int(channel)
    except (TypeError, ValueError):
        return 0
    return max(0, min(SERVO_COUNT - 1, value))


def clamp_color(value: Any) -> int:
    try:
        number = int(value)
    except (TypeError, ValueError):
        return 0
    return max(0, min(255, number))
def clamp_int(value: Any, low: int, high: int, default: int) -> int:
    try:
        number = int(value)
    except (TypeError, ValueError):
        number = default
    return max(low, min(high, number))


def clamp_hold(value: Any) -> int:
    return clamp_int(value, 50, 5000, 500)


def clamp_duration(value: Any) -> int:
    return clamp_int(value, 500, 20000, 5000)


def clamp_timeout(value: Any) -> int:
    return clamp_int(value, 500, 10000, 3000)


def clamp_sweep_period(value: Any) -> int:
    return clamp_int(
        value,
        SERVO_SWEEP_MIN_PERIOD_MS,
        SERVO_SWEEP_MAX_PERIOD_MS,
        SERVO_SWEEP_DEF_PERIOD_MS,
    )


def clamp_volume(value: Any) -> int:
    return clamp_int(value, 0, 100, 50)


def build_command(action: str, params: dict[str, Any]) -> list[str] | None:
    """Turn a UI action into an argv list built only from validated values.

    Nothing from the request is ever interpolated as free text, so the page
    cannot smuggle extra shell words into the device command line.
    """
    if action == "rgb_off":
        return ["v2hw", "rgb", "off"]
    if action == "rgb_test":
        return ["v2hw", "rgb", "test"]
    if action == "rgb_set":
        red = clamp_color(params.get("red"))
        green = clamp_color(params.get("green"))
        blue = clamp_color(params.get("blue"))
        return ["v2hw", "rgb", "set", str(red), str(green), str(blue)]
    if action == "servo_all_off":
        return ["v2hw", "servo", "all-off"]
    if action == "servo_test":
        channel = clamp_channel(params.get("channel"))
        pulse = clamp_pulse(params.get("pulse_us"))
        hold = clamp_hold(params.get("hold_ms"))
        return ["v2hw", "servo", "test", str(channel), str(pulse), str(hold)]
    if action == "servo_sweep_start":
        channel = clamp_channel(params.get("channel"))
        period = clamp_sweep_period(params.get("period_ms"))
        return ["v2hw", "servo", "sweep", str(channel), str(period)]
    if action == "servo_sweep_stop":
        return ["v2hw", "servo", "sweep-stop"]
    if action == "servo_sweep_status":
        return ["v2hw", "servo", "sweep-status"]
    if action == "vol_get":
        return ["v2hw", "vol"]
    if action == "vol_set":
        return ["v2hw", "vol", "set", str(clamp_volume(params.get("volume")))]
    if action == "vol_mute":
        return ["v2hw", "vol", "mute"]
    if action == "vol_unmute":
        return ["v2hw", "vol", "unmute"]
    if action == "touch_watch":
        duration = clamp_duration(params.get("duration_ms"))
        return ["v2hw", "touch", "watch", "all", str(duration)]
    if action == "selftest":
        return ["v2hw", "selftest"]
    if action in {"netcfg_status", "netcfg_portal_start", "netcfg_portal_stop"}:
        return ["v2hw", "netcfg", action.rsplit("_", 1)[1]]
    if action == "status":
        return ["v2hw", "status"]
    if action == "power":
        return ["v2hw", "power"]
    if action == "nfc_info":
        return ["v2hw", "nfc", "info"]
    if action == "nfc_card_read":
        return ["v2hw", "nfc", "card-read", str(clamp_timeout(params.get("timeout_ms")))]
    if action in {"nfc_peer_start", "nfc_peer_stop", "nfc_peer_status"}:
        return ["v2hw", "nfc", "peer", action.rsplit("_", 1)[1]]
    if action == "interact_start":
        return ["interact", "start"]
    if action == "interact_stop":
        return ["interact", "stop"]
    if action == "tdoa_status":
        return ["tdoa", "status"]
    return None


#: ``servo_sweep_stop`` is deliberately absent: stopping a moving servo must
#: never be blocked by the arm switch, or re-locking the page would leave the
#: sweep running with no way to halt it.
ACTIONS_NEEDING_ARM = {"servo_test", "servo_sweep_start"}

#: ``interact`` is exported with ``SHELL_CMD_DISABLE_RETURN``, so it never
#: prints the ``ok`` terminator the other commands end with. Waiting for one
#: would always time out even though the session started fine, so these
#: actions collect a short window of log lines instead. Status commands also
#: omit the terminator and must return their expected structured status record.
ACTIONS_WITHOUT_ACK = {"interact_start", "interact_stop", "status", "tdoa_status"}
NO_ACK_COLLECT = 4.0


def command_budget(action: str, params: dict[str, Any]) -> float:
    """Commands that block the device shell need their own duration allowed."""
    if action in ACTIONS_WITHOUT_ACK:
        return NO_ACK_COLLECT
    extra = 0.0
    if action == "touch_watch":
        extra = clamp_duration(params.get("duration_ms")) / 1000.0
    elif action in {"nfc_scan", "nfc_card_read", "nfc_target"}:
        extra = clamp_timeout(params.get("timeout_ms")) / 1000.0
    elif action == "servo_test":
        extra = clamp_hold(params.get("hold_ms")) / 1000.0
    elif action == "rgb_test":
        extra = 4.0
    return COMMAND_TIMEOUT + extra


def is_echo(line: str, command: str) -> bool:
    """Drop the shell's own prompt-plus-echo of the command we just typed."""
    if line == command:
        return True
    return line.endswith(command) and "$" in line


class TestBench:
    """Drives one board: polls read commands, tails logs, runs control commands."""

    def __init__(self, adb: str, transport_id: int | None, sample_rate: int) -> None:
        self.adb = adb
        self.transport_id = transport_id
        self.sample_rate = sample_rate
        self.process: subprocess.Popen[str] | None = None
        self.lock = threading.Lock()
        self.channels: list[queue.Queue[dict[str, Any]]] = []
        self.running = False
        self.threads: list[threading.Thread] = []
        self.error: str | None = None
        self.servo_armed = False

        # The device shell service serves one session at a time: opening a
        # second ``adb shell`` silently starves the persistent one. Control
        # commands therefore share this shell, and the polling loops hold off
        # while one runs so the reply can be attributed to it.
        self.command_lock = threading.Lock()
        self.poll_gate = threading.Event()
        self.poll_gate.set()
        self.capture_lock = threading.Lock()
        self.capture: list[str] | None = None
        self.capture_last = 0.0
        self.capture_done = False

        self.last_sequence: int | None = None
        self.tdoa: dict[str, Any] | None = None
        self.tdoa_last_valid: dict[str, Any] | None = None
        self.history: list[dict[str, Any]] = []
        self.counts = {"left": 0, "center": 0, "front": 0}
        # UI labels are physical MIC1/MIC2; firmware TDOA fields are m0/m1.
        self.mic_peaks = {"mic1": 0, "mic2": 0}
        self.mic_levels = {"mic1": 0, "mic2": 0}

        self.touches: dict[str, dict[str, Any]] = {}
        self.touch_transition_counts = {pad: 0 for pad in TOUCH_PADS}
        self.touch_last_valid_levels: dict[str, int] = {}
        self.mpu: dict[str, Any] | None = None
        self.nfc: dict[str, Any] | None = None
        self.power: dict[str, Any] | None = None
        self.board: dict[str, Any] | None = None
        self.netcfg: dict[str, Any] | None = None
        self.card_uids: list[str] = []
        self.rgb: dict[str, Any] = {"state": "unknown", "red": 0, "green": 0, "blue": 0}
        self.servo: dict[str, Any] = {"last_channel": None, "last_pulse_us": None}
        self.sweep: dict[str, Any] = {"running": False, "channel": None}
        self.volume: dict[str, Any] | None = None
        self.cloud: dict[str, Any] = {"stage": None, "stage_label": "空闲", "updated": None}
        self.events: list[dict[str, Any]] = []
        self.command_log: list[dict[str, Any]] = []

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
        tdoa = self.tdoa
        stale = tdoa is None or (now - float(tdoa.get("host_time", 0.0))) > STALE_AFTER
        return {
            "transport_id": self.transport_id,
            "sample_rate": self.sample_rate,
            "running": self.running,
            "error": self.error,
            "stale": bool(stale),
            "servo_armed": self.servo_armed,
            "board": self.board,
            "netcfg": self.netcfg,
            "tdoa": tdoa,
            "tdoa_last_valid": self.tdoa_last_valid,
            "counts": dict(self.counts),
            "mic_peaks": dict(self.mic_peaks),
            "mic_levels": dict(self.mic_levels),
            "history": list(self.history[-MAX_HISTORY:]),
            # Keep the original PA09 fields for clients made for the first
            # single-channel dashboard, and publish the real dual-channel
            # state alongside them.
            "touch": self.touches.get("PA09"),
            "touch_transitions": self.touch_transition_counts["PA09"],
            "touches": {pad: dict(record) for pad, record in self.touches.items()},
            "touch_transition_counts": dict(self.touch_transition_counts),
            "mpu": self.mpu,
            "nfc": self.nfc,
            "power": self.power,
            "power_age": (
                None if self.power is None
                else round(now - float(self.power.get("host_time", now)), 1)
            ),
            "card_uids": list(self.card_uids),
            "rgb": dict(self.rgb),
            "servo": dict(self.servo),
            "sweep": dict(self.sweep),
            "volume": self.volume,
            "cloud": dict(self.cloud),
            "events": list(self.events[-MAX_EVENTS:]),
            "command_log": list(self.command_log[-30:]),
            "servo_limits": {
                "count": SERVO_COUNT,
                "min_pulse_us": SERVO_MIN_PULSE_US,
                "center_pulse_us": SERVO_CENTER_PULSE_US,
                "max_pulse_us": SERVO_MAX_PULSE_US,
                "pads": list(SERVO_PADS),
                "sweep_min_period_ms": SERVO_SWEEP_MIN_PERIOD_MS,
                "sweep_max_period_ms": SERVO_SWEEP_MAX_PERIOD_MS,
                "sweep_def_period_ms": SERVO_SWEEP_DEF_PERIOD_MS,
            },
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

    def publish(self) -> None:
        with self.lock:
            self._publish_locked()
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
            **hidden_subprocess_kwargs(),
        )
        self.running = True
        for target in (self._read_loop, self._fast_loop, self._slow_loop):
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

    def _write(self, command: str) -> bool:
        process = self.process
        if process is None or process.stdin is None or process.poll() is not None:
            self._set_error("ADB shell 已退出，重启工具或检查 USB 连接")
            return False
        try:
            process.stdin.write(f"{command}\n")
            process.stdin.flush()
        except (OSError, ValueError):
            self._set_error("向 ADB shell 写入失败")
            return False
        return True

    def _set_error(self, message: str) -> None:
        with self.lock:
            self.error = message
            self.running = False
            self._publish_locked()

    # -- worker threads ---------------------------------------------------
    def _fast_loop(self) -> None:
        """Weight the IMU highest so the orientation cube tracks real motion.

        The single shell session serialises every read, so a slot given to one
        sensor is a slot taken from another. ``v2hw mpu`` appears twice per
        cycle because the cube needs the fastest refresh; touch and TDOA each
        stay well under a second.
        """
        index = 0
        while self.running:
            self.poll_gate.wait()
            if not self.running:
                return
            if not self._write(FAST_ROTATION[index % len(FAST_ROTATION)]):
                return
            index += 1
            time.sleep(FAST_INTERVAL)

    def _slow_loop(self) -> None:
        """Rotate the cheap read commands; NFC info stays deliberately rare."""
        rotation = ("v2hw power", "v2hw status", "v2hw netcfg status")
        index = 0
        last_nfc = 0.0
        if self.running:
            self._write("v2hw nfc info")
            last_nfc = time.monotonic()
            # Volume only changes when the page changes it, so one read at
            # startup is enough to seed the slider instead of a poll slot.
            self._write("v2hw vol")
        while self.running:
            self.poll_gate.wait()
            if not self.running:
                return
            if not self._write(rotation[index % len(rotation)]):
                return
            index += 1
            now = time.monotonic()
            if now - last_nfc >= NFC_INTERVAL:
                if not self._write("v2hw nfc info"):
                    return
                last_nfc = now
            time.sleep(SLOW_INTERVAL)

    def _read_loop(self) -> None:
        process = self.process
        if process is None or process.stdout is None:
            return
        try:
            for raw in process.stdout:
                if not self.running:
                    break
                self.ingest(raw)
        except (OSError, ValueError):
            pass

    # -- ingestion --------------------------------------------------------
    def ingest(self, raw: str) -> None:
        line = clean_line(raw)
        if not line:
            return

        with self.capture_lock:
            if self.capture is not None:
                self.capture.append(line)
                self.capture_last = time.monotonic()
                if COMMAND_END_RE.search(line):
                    self.capture_done = True

        payload = parse_tdoa(line)
        if payload is not None:
            self._on_tdoa(payload)
            return

        for parser, handler in (
            (parse_touch, self._on_touch),
            (parse_mpu, self._on_mpu),
            (parse_pn532, self._on_nfc),
            (parse_power, self._on_power),
            (parse_board, self._on_board),
            (parse_netcfg_portal, self._on_netcfg_portal),
            (parse_netcfg_wifi, self._on_netcfg_wifi),
            (parse_sweep, self._on_sweep),
            (parse_volume, self._on_volume),
        ):
            record = parser(line)
            if record is not None:
                handler(record)
                return

        uid = parse_card_uid(line)
        if uid is not None:
            self._on_card_uid(uid)
            return

        self._on_log(line)

    def _on_tdoa(self, payload: dict[str, Any]) -> None:
        sequence = payload.get("s")
        with self.lock:
            if sequence == self.last_sequence:
                return
            self.last_sequence = sequence
            record = self._decorate_tdoa(payload)
            self.tdoa = record
            self.mic_peaks["mic1"] = max(self.mic_peaks["mic1"], record["peak_mic0"])
            self.mic_peaks["mic2"] = max(self.mic_peaks["mic2"], record["peak_mic1"])
            self.mic_levels["mic1"] = max(self.mic_levels["mic1"], record["level_mic0"])
            self.mic_levels["mic2"] = max(self.mic_levels["mic2"], record["level_mic1"])
            if record["valid"]:
                self.tdoa_last_valid = record
                key = {SIDE_MIC0: "left", SIDE_CENTER: "center", SIDE_MIC1: "front"}.get(
                    int(record["side"])
                )
                if key is not None:
                    self.counts[key] += 1
                self.history.append(record)
                del self.history[:-MAX_HISTORY]
            self.error = None
            self._publish_locked()

    def _decorate_tdoa(self, payload: dict[str, Any]) -> dict[str, Any]:
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
    def _on_touch(self, record: dict[str, Any]) -> None:
        with self.lock:
            pad = str(record["pad"])
            # A failed read may still carry a placeholder level. Never let it
            # create a false transition or make a channel pass the test.
            if record["status"] == 0:
                previous_level = self.touch_last_valid_levels.get(pad)
                if previous_level is not None and previous_level != record["level"]:
                    self.touch_transition_counts[pad] = self.touch_transition_counts.get(pad, 0) + 1
                    self._append_event_locked(
                        "touch", f"触摸 {pad}", f"level {previous_level} -> {record['level']}"
                    )
                self.touch_last_valid_levels[pad] = int(record["level"])
            record["host_time"] = time.monotonic()
            self.touches[pad] = record
            self._publish_locked()

    def _on_mpu(self, record: dict[str, Any]) -> None:
        with self.lock:
            record["host_time"] = time.monotonic()
            self.mpu = record
            self._publish_locked()

    def _on_nfc(self, record: dict[str, Any]) -> None:
        with self.lock:
            record["host_time"] = time.monotonic()
            self.nfc = record
            self._publish_locked()

    def _on_power(self, record: dict[str, Any]) -> None:
        with self.lock:
            previous = self.power
            if previous is not None:
                if previous.get("battery") != record["battery"]:
                    self._append_event_locked(
                        "power",
                        "电池",
                        f"{previous.get('battery_label')} -> {record['battery_label']}",
                    )
                elif previous.get("percent") != record["percent"]:
                    self._append_event_locked(
                        "power",
                        "电池",
                        f"电量 {previous.get('percent')}% -> {record['percent']}%"
                        f"（{record['volt']:.3f} V）",
                    )
            record["host_time"] = time.monotonic()
            self.power = record
            self._publish_locked()

    def _on_board(self, record: dict[str, Any]) -> None:
        with self.lock:
            self.board = record
            state = record.get("sweep")
            if state is not None:
                running = state == "running"
                # ``status`` carries no channel, so keep the known one while
                # running and forget it once the sweep has stopped.
                self.sweep = {
                    "running": running,
                    "channel": self.sweep.get("channel") if running else None,
                }
            self._publish_locked()

    def _on_sweep(self, record: dict[str, Any]) -> None:
        with self.lock:
            channel = record.get("channel")
            if channel is None and record["running"]:
                channel = self.sweep.get("channel")
            self.sweep = {"running": record["running"], "channel": channel}
            self._publish_locked()

    def _on_volume(self, record: dict[str, Any]) -> None:
        with self.lock:
            self.volume = record
            self._publish_locked()

    def _on_netcfg_portal(self, record: dict[str, Any]) -> None:
        with self.lock:
            current = dict(self.netcfg or {})
            current.update(record)
            current["host_time"] = time.monotonic()
            self.netcfg = current
            self._publish_locked()

    def _on_netcfg_wifi(self, record: dict[str, Any]) -> None:
        with self.lock:
            current = dict(self.netcfg or {})
            current.update(record)
            current["host_time"] = time.monotonic()
            self.netcfg = current
            self._publish_locked()

    def _on_card_uid(self, uid: str) -> None:
        with self.lock:
            if uid not in self.card_uids:
                self.card_uids.append(uid)
                del self.card_uids[:-8]
            self._append_event_locked("nfc", "NFC", f"UID {uid}")
            self._publish_locked()

    def _on_log(self, line: str) -> None:
        classified = classify_cloud_event(line)
        if classified is None:
            return
        key, label = classified
        with self.lock:
            self._append_event_locked(key, label, line[:200])
            if key not in {"error", "network"}:
                self.cloud = {"stage": key, "stage_label": label, "updated": time.monotonic()}
            self._publish_locked()

    def _append_event_locked(self, key: str, label: str, text: str) -> None:
        self.events.append(
            {
                "kind": key,
                "label": label,
                "text": text,
                "host_time": time.monotonic(),
                "clock": time.strftime("%H:%M:%S"),
            }
        )
        del self.events[:-MAX_EVENTS]

    # -- control ----------------------------------------------------------
    def set_servo_armed(self, armed: bool) -> None:
        with self.lock:
            self.servo_armed = bool(armed)
            self._append_event_locked(
                "servo", "舵机", "已解锁，可以转动" if armed else "已锁定，禁止转动"
            )
            self._publish_locked()

    def run_command(self, action: str, params: dict[str, Any]) -> dict[str, Any]:
        argv = build_command(action, params)
        if argv is None:
            return {"ok": False, "error": f"未知动作：{action}"}
        if action in ACTIONS_NEEDING_ARM and not self.servo_armed:
            return {"ok": False, "error": "舵机处于锁定状态，先在页面上解锁再转动"}
        if self.transport_id is None:
            return {"ok": False, "error": "还没有解析到 transport_id"}

        command = " ".join(argv)
        budget = command_budget(action, params)
        lines, timed_out = self._exchange(command, budget)
        if lines is None:
            return {"ok": False, "command": command, "error": "ADB shell 不可用"}

        failed = any(item.startswith("v2hw command failed") for item in lines)
        no_ack = action in ACTIONS_WITHOUT_ACK
        result: dict[str, Any] = {
            "ok": not failed and (no_ack or not timed_out),
            "command": command,
            "output": lines[-40:],
        }
        if timed_out and not no_ack:
            result["error"] = "命令超时，板子没有回 ok"
        if action in {"status", "tdoa_status"}:
            parser = parse_board if action == "status" else parse_tdoa
            if not any(parser(line) is not None for line in lines):
                result["ok"] = False
                result["error"] = "未收到有效状态，不以空日志判定成功"
        self._note_command(action, params, result)
        return result

    def _exchange(self, command: str, budget: float) -> tuple[list[str] | None, bool]:
        """Send one command down the shared shell and collect its reply.

        The polling loops are paused for the duration so the captured lines
        belong to this command rather than to a concurrent ``tdoa status``.
        """
        with self.command_lock:
            self.poll_gate.clear()
            try:
                time.sleep(COMMAND_SETTLE)
                with self.capture_lock:
                    self.capture = []
                    self.capture_done = False
                    self.capture_last = time.monotonic()
                if not self._write(command):
                    with self.capture_lock:
                        self.capture = None
                    return None, False

                deadline = time.monotonic() + budget
                while time.monotonic() < deadline:
                    with self.capture_lock:
                        done = self.capture_done
                        idle = time.monotonic() - self.capture_last
                        seen = len(self.capture or [])
                    if done:
                        break
                    # Long NFC Target / servo-test operations log before they
                    # finish. Only commands without an ACK may end on silence.
                    if seen and idle >= COMMAND_SETTLE and command in {
                        "interact start", "interact stop", "v2hw status", "tdoa status"
                    }:
                        break
                    time.sleep(0.05)

                with self.capture_lock:
                    captured = list(self.capture or [])
                    done = self.capture_done
                    self.capture = None
                    self.capture_done = False
            finally:
                self.poll_gate.set()

        lines = [item for item in captured if not is_echo(item, command)]
        return lines, not done

    def _note_command(self, action: str, params: dict[str, Any], result: dict[str, Any]) -> None:
        with self.lock:
            if action == "rgb_off":
                self.rgb = {"state": "off", "red": 0, "green": 0, "blue": 0}
            elif action == "rgb_set":
                self.rgb = {
                    "state": "on",
                    "red": clamp_color(params.get("red")),
                    "green": clamp_color(params.get("green")),
                    "blue": clamp_color(params.get("blue")),
                }
            elif action == "rgb_test":
                self.rgb = {"state": "self-test", "red": 0, "green": 0, "blue": 0}
            elif action == "servo_test":
                self.servo = {
                    "last_channel": clamp_channel(params.get("channel")),
                    "last_pulse_us": clamp_pulse(params.get("pulse_us")),
                }
            elif action == "servo_all_off":
                self.servo = {"last_channel": None, "last_pulse_us": None}
                self.sweep = {"running": False, "channel": None}
            elif action == "servo_sweep_start" and result.get("ok"):
                # ``sweep`` acknowledges with its own pulse line rather than the
                # ``servo sweep:`` form the parser reads, so record it here.
                self.sweep = {
                    "running": True,
                    "channel": clamp_channel(params.get("channel")),
                }
            elif action == "servo_sweep_stop" and result.get("ok"):
                self.sweep = {"running": False, "channel": None}
            self._append_event_locked(
                "command",
                "命令",
                f"{result.get('command', action)} -> {'ok' if result.get('ok') else '失败'}",
            )
            self.command_log.append(
                {
                    "action": action,
                    "command": result.get("command", action),
                    "ok": bool(result.get("ok")),
                    "clock": time.strftime("%H:%M:%S"),
                }
            )
            del self.command_log[:-30]
            self._publish_locked()
HTML_HEAD = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>ARCS-MINI V2.1 综合测试台</title>
<style>
:root { color-scheme: dark; }
* { box-sizing: border-box; }
body { margin: 0; padding: 18px; background: #0f1115; color: #e6e8ee;
  font: 14px/1.55 -apple-system, "Noto Sans CJK SC", "Microsoft YaHei", sans-serif; }
h1 { font-size: 18px; margin: 0 0 4px; }
h2 { font-size: 14px; margin: 0 0 10px; color: #9aa4b8; font-weight: 600; }
.top { display: flex; flex-wrap: wrap; gap: 12px; align-items: center;
  margin-bottom: 14px; }
.state { padding: 4px 10px; border-radius: 999px; font-size: 12px;
  background: #1d3a24; color: #7fe0a0; }
.state.err { background: #3a1d1d; color: #ff9d9d; }
.state.warn { background: #3a331d; color: #ffd98a; }
.state.on { background: #1d3a24; color: #7fe0a0; }
.mono { font-family: ui-monospace, Menlo, Consolas, monospace;
  min-width: 3ch; text-align: right; }
input[type=range] { flex: 1 1 140px; min-width: 120px; accent-color: #4f8cff; }
.row.sweep, .row.vol { padding: 8px; border-radius: 8px;
  background: #10131a; border: 1px solid #242938; }
.grid { display: grid; gap: 12px;
  grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); }
.card { background: #171a21; border: 1px solid #242938; border-radius: 10px;
  padding: 14px; }
.card.wide { grid-column: 1 / -1; }
.row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center;
  margin-bottom: 8px; }
.kv { display: grid; grid-template-columns: auto 1fr; gap: 4px 12px;
  font-variant-numeric: tabular-nums; }
.kv dt { color: #8d97ab; }
.kv dd { margin: 0; }
button { background: #232838; color: #e6e8ee; border: 1px solid #313850;
  border-radius: 7px; padding: 7px 12px; cursor: pointer; font-size: 13px; }
button:hover { background: #2c3346; }
button:disabled { opacity: .45; cursor: not-allowed; }
button.primary { background: #1f4d31; border-color: #2c6b44; }
button.danger { background: #4d1f1f; border-color: #6b2c2c; }
input[type=number] { width: 92px; }
input, select { background: #10131a; color: #e6e8ee; border: 1px solid #313850;
  border-radius: 6px; padding: 6px 8px; font-size: 13px; }
label { color: #8d97ab; font-size: 12px; }
.swatch { width: 26px; height: 26px; border-radius: 6px; border: 1px solid #313850;
  display: inline-block; vertical-align: middle; }
.dir { display: flex; gap: 8px; margin: 10px 0; }
.dir div { flex: 1; text-align: center; padding: 14px 0; border-radius: 8px;
  background: #10131a; border: 1px solid #242938; font-size: 15px; }
.dir div.on { background: #1f4d31; border-color: #2c6b44; color: #b6f5cd; }
.bar { height: 8px; background: #10131a; border-radius: 999px; overflow: hidden;
  margin: 3px 0 8px; }
.bar span { display: block; height: 100%; background: #4f8cff; width: 0; }
.dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%;
  background: #444b60; margin-right: 6px; vertical-align: middle; }
.dot.on { background: #7fe0a0; }
.dot.bad { background: #ff7a7a; }
.log { max-height: 240px; overflow-y: auto; font-family: ui-monospace, monospace;
  font-size: 12px; }
.log div { padding: 2px 0; border-bottom: 1px solid #1d2130; white-space: pre-wrap;
  word-break: break-all; }
.log .t { color: #6f7a90; margin-right: 8px; }
.tag { display: inline-block; min-width: 62px; color: #9ecbff; }
.tag.error { color: #ff9d9d; }
.hint { color: #6f7a90; font-size: 12px; margin-top: 8px; }
.stage { height: 190px; margin: 10px 0 4px; display: flex;
         align-items: center; justify-content: center;
         perspective: 620px; background: #10131a; border-radius: 10px; }
.cube { position: relative; width: 96px; height: 96px;
        transform-style: preserve-3d;
        transition: transform 120ms linear; }
.cube .face { position: absolute; width: 96px; height: 96px;
        display: flex; align-items: center; justify-content: center;
        font-size: 13px; font-weight: 600; color: #cdd6e4;
        border: 1px solid #4f8cff; background: rgba(79,140,255,.14); }
.cube .fz { background: rgba(79,140,255,.34); }
.cube .bz { background: rgba(20,24,32,.72); }
.cube .fx { transform: rotateY(90deg)  translateZ(48px); }
.cube .bx { transform: rotateY(-90deg) translateZ(48px); }
.cube .fy { transform: rotateX(-90deg) translateZ(48px); }
.cube .by { transform: rotateX(90deg)  translateZ(48px); }
.cube .fz { transform: translateZ(48px); }
.cube .bz { transform: rotateY(180deg) translateZ(48px); }
.chk { display: inline-flex; align-items: center; gap: 6px; font-size: 13px; }
.touchpad { margin: 10px 0 4px; height: 96px; border-radius: 12px;
  display: flex; align-items: center; justify-content: center;
  font-size: 22px; font-weight: 700; letter-spacing: 2px;
  color: #6f7a90; background: #10131a; border: 2px solid #262c38;
  transition: background 90ms linear, color 90ms linear, border-color 90ms linear; }
.touchpad.on { color: #06210f; background: #7fe0a0; border-color: #7fe0a0; }
.touchpad.flash { animation: touchflash 420ms ease-out; }
.touch-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }
.touch-channel { padding: 10px; border-radius: 9px; background: #10131a;
  border: 1px solid #242938; }
.touch-channel h3 { margin: 0 0 6px; font-size: 13px; color: #cdd6e4; }
.touch-channel .touchpad { height: 72px; }
@media (max-width: 520px) { .touch-grid { grid-template-columns: 1fr; } }
@keyframes touchflash {
  0%   { box-shadow: 0 0 0 0 rgba(127,224,160,.85); }
  100% { box-shadow: 0 0 0 22px rgba(127,224,160,0); }
}
.out { font-family: ui-monospace, monospace; font-size: 12px; color: #9aa4b8;
  white-space: pre-wrap; margin-top: 8px; max-height: 120px; overflow-y: auto; }
</style>
</head>
<body>
"""
HTML_BODY = """<div class="top">
  <h1>ARCS-MINI V2.1 综合测试台</h1>
  <span id="state" class="state">连接中…</span>
  <span id="meta" class="hint"></span>
</div>

<div class="grid">

  <div class="card">
    <h2>WS2812 灯（PA08）</h2>
    <div class="row">
      <button class="primary" onclick="send('rgb_set', colorParams())">开灯</button>
      <button onclick="send('rgb_off')">关灯</button>
      <button onclick="send('rgb_test')">自检序列</button>
    </div>
    <div class="row">
      <label>颜色</label>
      <input type="color" id="color" value="#003c00" oninput="syncColor()" />
      <span class="swatch" id="swatch"></span>
      <label>R</label><input type="number" id="r" min="0" max="255" value="0" />
      <label>G</label><input type="number" id="g" min="0" max="255" value="60" />
      <label>B</label><input type="number" id="b" min="0" max="255" value="0" />
    </div>
    <div class="row">
      <button onclick="preset(60,0,0)">红</button>
      <button onclick="preset(0,60,0)">绿</button>
      <button onclick="preset(0,0,60)">蓝</button>
      <button onclick="preset(60,60,60)">白</button>
    </div>
    <dl class="kv"><dt>当前状态</dt><dd id="rgbState">未知</dd></dl>
    <p class="hint">亮度建议先用 60 以内，5 V 供电下满亮很刺眼。</p>
  </div>

  <div class="card">
    <h2>舵机 0–3（PA04–PA07）</h2>
    <div class="row">
      <label><input type="checkbox" id="arm" onchange="arm(this.checked)" /> 解锁转动</label>
      <span id="armState" class="state warn">已锁定</span>
    </div>
    <div class="row">
      <label>通道</label>
      <select id="ch">
        <option value="0">0 / PA04</option><option value="1">1 / PA05</option>
        <option value="2">2 / PA06</option><option value="3">3 / PA07</option>
      </select>
      <label>脉宽 µs</label>
      <input type="number" id="pulse" min="500" max="2500" step="10" value="1500" />
      <label>保持 ms</label>
      <input type="number" id="hold" min="50" max="5000" step="50" value="500" />
    </div>
    <div class="row">
      <button class="primary" id="servoGo" onclick="servoTest()">转动</button>
      <button onclick="setPulse(500)">最小 500</button>
      <button onclick="setPulse(1500)">中位 1500</button>
      <button onclick="setPulse(2500)">最大 2500</button>
      <button class="danger" onclick="send('servo_all_off')">全部断开</button>
    </div>
    <div class="row sweep">
      <label><input type="checkbox" id="sweepOn" onchange="sweep(this.checked)" /> 持续转动</label>
      <span id="sweepState" class="state">已停止</span>
      <label>周期 ms</label>
      <input type="number" id="sweepPeriod" min="400" max="8000" step="100" value="1600" />
    </div>
    <dl class="kv"><dt>最近动作</dt><dd id="servoState">未动过</dd></dl>
    <p class="hint">
      单次「转动」保持到时会自动断开输出。「持续转动」让上面选中的通道在两端之间来回走，
      直到你把开关关掉；周期越小转得越快。持续转动时单次转动会被固件拒绝。
    </p>
  </div>

  <div class="card">
    <h2>声源定位（双麦 TDOA）</h2>
    <div class="dir">
      <div id="dLeft">左<br /><small>MIC0</small></div>
      <div id="dCenter">中</div>
      <div id="dFront">前<br /><small>MIC1</small></div>
    </div>
    <dl class="kv">
      <dt>判定</dt><dd id="tSide">—</dd>
      <dt>时延</dt><dd id="tDelay">—</dd>
      <dt>相关度</dt><dd id="tCorr">—</dd>
      <dt>有效块</dt><dd id="tActive">—</dd>
      <dt>ILD 融合</dt><dd id="tFused">—</dd>
      <dt>累计</dt><dd id="tTally">—</dd>
    </dl>
    <label>MIC0 电平 <span id="m0">0</span></label>
    <div class="bar"><span id="bar0"></span></div>
    <label>MIC1 电平 <span id="m1">0</span></label>
    <div class="bar"><span id="bar1"></span></div>
  </div>

  <div class="card">
    <h2>IMU（MPU6050 / I2C0 0x68）</h2>
    <dl class="kv">
      <dt>状态</dt><dd id="mStatus">—</dd>
      <dt>WHO_AM_I</dt><dd id="mWho">—</dd>
      <dt>加速度</dt><dd id="mAccel">—</dd>
      <dt>合成模量</dt><dd id="mG">—</dd>
      <dt>角速度</dt><dd id="mGyro">—</dd>
    </dl>
    <div class="stage">
      <div class="cube" id="cube">
        <span class="face fx">+X</span>
        <span class="face bx">-X</span>
        <span class="face fy">+Y</span>
        <span class="face by">-Y</span>
        <span class="face fz">上</span>
        <span class="face bz">下</span>
      </div>
    </div>
    <dl class="kv">
      <dt>横滚 / 俯仰</dt><dd id="mRP">—</dd>
      <dt>偏航（陀螺积分）</dt><dd id="mYaw">—</dd>
    </dl>
    <div class="row">
      <button class="ghost" onclick="resetYaw()">偏航归零</button>
      <label class="chk"><input type="checkbox" id="yawOn" checked> 启用偏航</label>
    </div>
    <p class="hint">
      立方体的横滚/俯仰由重力方向直接算出，板子静止时也准。偏航只能靠陀螺积分，
      会随时间漂移，转完一圈回来不一定对齐，需要时点「偏航归零」。
      静止时合成模量应接近 1.000 g。
    </p>
  </div>

  <div class="card">
    <h2>双路触摸（TTP223 锁存模式）</h2>
    <div class="touch-grid">
      <div class="touch-channel">
        <h3>触摸 1 · PA09（U10/U12）</h3>
        <div class="touchpad" id="k09Big">等待数据</div>
        <dl class="kv">
          <dt>GPIO</dt><dd id="k09Status">—</dd>
          <dt>当前电平</dt><dd id="k09Level">—</dd>
          <dt>本轮翻转</dt><dd id="k09Flips">0</dd>
          <dt>结果</dt><dd><span id="k09Result" class="state warn">请点触</span></dd>
        </dl>
      </div>
      <div class="touch-channel">
        <h3>触摸 2 · PA13（U7/U9）</h3>
        <div class="touchpad" id="k13Big">等待数据</div>
        <dl class="kv">
          <dt>GPIO</dt><dd id="k13Status">—</dd>
          <dt>当前电平</dt><dd id="k13Level">—</dd>
          <dt>本轮翻转</dt><dd id="k13Flips">0</dd>
          <dt>结果</dt><dd><span id="k13Result" class="state warn">请点触</span></dd>
        </dl>
      </div>
    </div>
    <div class="row">
      <button class="primary" onclick="resetTouchTest()">清零重新测</button>
    </div>
    <p class="hint">每路慢速点触一次即应从 0↔1 翻转，检测到任意一次翻转后会粘性显示“通过”。
      这是锁存输出，松手不会自动回到 0。</p>
  </div>

  <div class="card">
    <h2>NFC（PN532 / I2C0 0x24）</h2>
    <dl class="kv">
      <dt>状态</dt><dd id="nStatus">—</dd>
      <dt>IC / 固件</dt><dd id="nIc">—</dd>
      <dt>读到的 UID</dt><dd id="nUid">—</dd>
    </dl>
    <div class="row">
      <button onclick="send('nfc_card_read', {timeout_ms: 3000})">读外部卡 3 s</button>
      <button onclick="send('nfc_info')">重新读 info</button>
    </div>
    <div class="row">
      <button onclick="send('nfc_peer_start')">互认 start</button>
      <button onclick="send('nfc_peer_status')">互认 status</button>
      <button class="danger" onclick="send('nfc_peer_stop')">互认 stop</button>
    </div>
    <p class="hint">互认要两块板都 start；高频轮询会诱发 PN532 -3/-4，这里只每 10 s 读一次 info。</p>
  </div>

  <div class="card">
    <h2>配网</h2>
    <dl class="kv">
      <dt>门户</dt><dd id="nPortal">—</dd>
      <dt>热点 / IP</dt><dd id="nSsid">—</dd>
      <dt>Wi‑Fi</dt><dd id="nWifi">—</dd>
      <dt>已存凭据</dt><dd id="nSavedAp">—</dd>
      <dt>连接结果</dt><dd id="nConnectRes">—</dd>
    </dl>
    <div class="row">
      <button onclick="send('netcfg_portal_start')">打开热点配网页</button>
      <button class="danger" onclick="send('netcfg_portal_stop')">关闭热点配网页</button>
      <button onclick="send('netcfg_status')">刷新状态</button>
    </div>
    <p class="hint">V2.1 无凭据或保存 AP 连接失败时会开启热点。连接设备热点后，访问上方 IP 配网；本机工作台不采集 Wi-Fi 密码。</p>
  </div>

  <div class="card">
    <h2>电池 / 充电</h2>
    <div class="row">
      <span id="pBig" style="font-size: 26px; font-variant-numeric: tabular-nums;">—</span>
      <span id="pBattery" class="state warn">—</span>
    </div>
    <div class="bar"><span id="pBar" style="background: #7fe0a0"></span></div>
    <dl class="kv">
      <dt>VBAT pin2</dt><dd id="pVolt">—</dd>
      <dt>电量档</dt><dd id="pLevel">—</dd>
      <dt>刷新</dt><dd id="pAge">—</dd>
    </dl>
    <div class="row"><button onclick="send('power')">立即测一次</button></div>
    <p class="hint" id="pHint"></p>
  </div>

  <div class="card">
    <h2>云端语音对话</h2>
    <dl class="kv">
      <dt>当前阶段</dt><dd id="cStage">空闲</dd>
    </dl>
    <div class="row">
      <button class="primary" onclick="send('interact_start')">开始云端对话（跳过唤醒词）</button>
      <button class="danger" onclick="send('interact_stop')">结束会话</button>
      <button onclick="send('selftest')">硬件自检</button>
    </div>
    <div class="row vol">
      <label><input type="checkbox" id="muteOn" onchange="mute(this.checked)" /> 静音</label>
      <span id="volState" class="state">—</span>
    </div>
    <div class="row">
      <label>音量</label>
      <input type="range" id="volSlider" min="0" max="100" step="1" value="60"
             oninput="volPreview(this.value)" onchange="volCommit(this.value)" />
      <span id="volValue" class="mono">60</span>
      <button onclick="setVol(30)">30</button>
      <button onclick="setVol(60)">60</button>
      <button onclick="setVol(100)">100</button>
    </div>
    <p class="hint" id="volHint">
      滑块松手才下发 <code>v2hw vol set</code>。静音开关会真的把输出压到 0，
      解除后回到静音前的音量；静音不写 KV，重启后自动恢复。
    </p>
    <p class="hint">
      这就是真正的云端对话，不是模拟。按钮只替掉「小灵小灵」这一步：它发的
      <code>VOICE_MSG_WAKEUP_KEYWORD</code> 和声学唤醒引擎发的是同一条消息，
      之后的收音、ASR、大模型、TTS 全部走真实云端。想连唤醒词一起测就直接对着板子说
      「小灵小灵」，不用点这个按钮。
    </p>
    <p class="hint">打断测试：TTS 正在播时直接说唤醒词，下面事件流会出现新的唤醒和会话行。</p>
  </div>

  <div class="card wide">
    <h2>事件流</h2>
    <div class="log" id="events"></div>
    <div class="out" id="out"></div>
  </div>

</div>
"""
HTML_SCRIPT = """<script>
const el = (id) => document.getElementById(id);
const num = (id) => parseInt(el(id).value, 10) || 0;
const LEVEL_FULL = 900;

function colorParams() {
  return { red: num("r"), green: num("g"), blue: num("b") };
}
function syncSwatch() {
  el("swatch").style.background =
    "rgb(" + num("r") + "," + num("g") + "," + num("b") + ")";
}
function syncColor() {
  const hex = el("color").value;
  el("r").value = parseInt(hex.slice(1, 3), 16);
  el("g").value = parseInt(hex.slice(3, 5), 16);
  el("b").value = parseInt(hex.slice(5, 7), 16);
  syncSwatch();
}
function preset(r, g, b) {
  el("r").value = r; el("g").value = g; el("b").value = b;
  syncSwatch();
  send("rgb_set", colorParams());
}
function setPulse(v) { el("pulse").value = v; }
function servoTest() {
  send("servo_test", {
    channel: parseInt(el("ch").value, 10),
    pulse_us: num("pulse"),
    hold_ms: num("hold"),
  });
}
function sweep(on) {
  if (on) {
    send("servo_sweep_start", {
      channel: parseInt(el("ch").value, 10),
      period_ms: num("sweepPeriod"),
    });
  } else {
    send("servo_sweep_stop");
  }
}
function setVol(v) {
  el("volSlider").value = v;
  el("volValue").textContent = v;
  send("vol_set", { volume: v });
}
function volPreview(v) { el("volValue").textContent = v; }
function volCommit(v) { send("vol_set", { volume: parseInt(v, 10) }); }
function mute(on) { send(on ? "vol_mute" : "vol_unmute"); }
function arm(on) {
  fetch("/api/arm", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ armed: !!on }),
  }).then((r) => r.json()).then(render).catch(() => {});
}
function send(action, params) {
  el("out").textContent = "执行 " + action + " …";
  fetch("/api/command", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action: action, params: params || {} }),
  })
    .then((r) => r.json())
    .then((res) => {
      const head = (res.command || action) + (res.ok ? "  ok" : "  失败");
      const tail = (res.output || []).join("\\n") || res.error || "";
      el("out").textContent = head + "\\n" + tail;
    })
    .catch((err) => { el("out").textContent = String(err); });
}

function fmtStatus(node, record) {
  if (!record) { el(node).textContent = "—"; return false; }
  const ok = record.status === 0;
  el(node).innerHTML =
    '<span class="dot ' + (ok ? "on" : "bad") + '"></span>' +
    record.status + " (" + (record.status_text || "") + ")";
  return ok;
}

const GYRO_LSB = 131.0;
const SMOOTH = 0.35;
let cubeRoll = 0, cubePitch = 0, cubeYaw = 0;
let cubeHave = false, cubeStamp = null;

function resetYaw() { cubeYaw = 0; }

function shortestStep(from, to) {
  let d = to - from;
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

const TOUCH_PADS = ["PA09", "PA13"];
const touchWas = { PA09: null, PA13: null };
const touchBaseline = { PA09: 0, PA13: 0 };
const touchCounts = { PA09: 0, PA13: 0 };

function touchPrefix(pad) { return pad === "PA13" ? "k13" : "k09"; }

function paintTouch(pad, record, transitions) {
  const prefix = touchPrefix(pad);
  const panel = el(prefix + "Big");
  const level = record ? record.level : null;
  panel.textContent = level === null ? "等待数据" : "电平 " + level;
  panel.classList.toggle("on", level === 1);
  if (touchWas[pad] !== null && touchWas[pad] !== level) {
    panel.classList.remove("flash");
    void panel.offsetWidth;
    panel.classList.add("flash");
  }
  touchWas[pad] = level;

  const result = el(prefix + "Result");
  const readError = record && record.status !== 0;
  result.className = readError ? "state err" : (transitions > 0 ? "state on" : "state warn");
  result.textContent = readError ? "读取失败" : (transitions > 0 ? "通过" : "请点触");
}

function resetTouchTest() {
  TOUCH_PADS.forEach((pad) => {
    touchBaseline[pad] = touchCounts[pad];
    const prefix = touchPrefix(pad);
    el(prefix + "Flips").textContent = "0";
    el(prefix + "Result").className = "state warn";
    el(prefix + "Result").textContent = "请点触";
  });
}

function updateCube(m) {
  if (!m || !m.accel || m.status !== 0) return;
  const ax = m.accel[0], ay = m.accel[1], az = m.accel[2];
  const roll = Math.atan2(ay, az) * 180 / Math.PI;
  const pitch = Math.atan2(-ax, Math.hypot(ay, az)) * 180 / Math.PI;

  if (!cubeHave) {
    cubeRoll = roll; cubePitch = pitch; cubeHave = true;
  } else {
    cubeRoll += shortestStep(cubeRoll, roll) * SMOOTH;
    cubePitch += shortestStep(cubePitch, pitch) * SMOOTH;
  }

  const now = m.host_time;
  if (el("yawOn").checked && m.gyro && cubeStamp !== null && now > cubeStamp) {
    const dt = Math.min(2.0, now - cubeStamp);
    const rate = m.gyro[2] / GYRO_LSB;
    if (Math.abs(rate) > 1.5) cubeYaw += rate * dt;
  }
  cubeStamp = now;

  el("cube").style.transform =
    "rotateY(" + cubeYaw.toFixed(1) + "deg) " +
    "rotateX(" + cubePitch.toFixed(1) + "deg) " +
    "rotateZ(" + cubeRoll.toFixed(1) + "deg)";
  el("mRP").textContent = roll.toFixed(1) + "° / " + pitch.toFixed(1) + "°";
  el("mYaw").textContent = cubeYaw.toFixed(1) + "°";
}

function render(state) {
  if (!state) return;
  const badge = el("state");
  if (state.error) {
    badge.className = "state err";
    badge.textContent = state.error;
  } else if (state.stale) {
    badge.className = "state warn";
    badge.textContent = "板子在线，暂时没有新的 TDOA 结果";
  } else {
    badge.className = "state";
    badge.textContent = "运行中";
  }
  const board = state.board || {};
  el("meta").textContent =
    "transport=" + (state.transport_id === null ? "?" : state.transport_id) +
    (board.board ? " · " + board.board : "") +
    (board.actuators ? " · actuators=" + board.actuators : "");

  const rgb = state.rgb || {};
  el("rgbState").innerHTML = rgb.state === "on"
    ? '<span class="swatch" style="background: rgb(' + rgb.red + "," + rgb.green +
      "," + rgb.blue + ')"></span> ' + rgb.red + "," + rgb.green + "," + rgb.blue
    : (rgb.state === "off" ? "已关闭" : (rgb.state || "未知"));

  el("arm").checked = !!state.servo_armed;
  el("servoGo").disabled = !state.servo_armed;
  el("armState").className = state.servo_armed ? "state" : "state warn";
  el("armState").textContent = state.servo_armed ? "已解锁" : "已锁定";
  const servo = state.servo || {};
  const pads = (state.servo_limits || {}).pads || [];
  el("servoState").textContent = servo.last_channel === null || servo.last_channel === undefined
    ? "未动过"
    : "通道 " + servo.last_channel + " / " + (pads[servo.last_channel] || "") +
      " @ " + servo.last_pulse_us + " µs";

  const sw = state.sweep || {};
  el("sweepOn").checked = !!sw.running;
  el("sweepOn").disabled = !state.servo_armed;
  el("sweepState").className = sw.running ? "state on" : "state";
  el("sweepState").textContent = sw.running
    ? "持续转动中 · 通道 " + (sw.channel === null || sw.channel === undefined ? "?" : sw.channel)
    : "已停止";
  // A one-shot hold fights the sweep task over the same shadow register, so
  // the firmware refuses it -- grey the button out instead of letting it fail.
  el("servoGo").disabled = !state.servo_armed || !!sw.running;

  const vol = state.volume;
  if (vol) {
    // While muted the device reports volume=0, which is not the level it will
    // come back to -- so leave the slider showing the last real level.
    if (!vol.muted && document.activeElement !== el("volSlider")) {
      el("volSlider").value = vol.volume;
      el("volValue").textContent = vol.volume;
    }
    el("muteOn").checked = !!vol.muted;
    el("volState").className = vol.muted ? "state warn" : "state on";
    el("volState").textContent = vol.muted
      ? "已静音"
      : "音量 " + vol.volume + "（下限 " + vol.min + "）";
  }

  const t = state.tdoa;
  const shown = (t && t.valid) ? t : state.tdoa_last_valid;
  el("dLeft").className = shown && shown.side === 1 ? "on" : "";
  el("dCenter").className = shown && shown.side === 2 ? "on" : "";
  el("dFront").className = shown && shown.side === 3 ? "on" : "";
  if (t) {
    el("tSide").textContent = t.valid
      ? t.side_label
      : (state.tdoa_last_valid ? "无声源（上次：" + state.tdoa_last_valid.side_label + "）" : "无声源");
    el("tDelay").textContent = (t.delay_milli / 1000).toFixed(3) + " 采样 / " +
      t.delay_us.toFixed(1) + " µs";
    el("tCorr").textContent = (t.corr_milli / 1000).toFixed(3);
    el("tActive").textContent = t.active_blocks + " / 63";
    el("tFused").textContent = t.fused ? "是" : "否";
    el("m0").textContent = t.level_mic0;
    el("m1").textContent = t.level_mic1;
    el("bar0").style.width = Math.min(100, (t.level_mic0 / LEVEL_FULL) * 100) + "%";
    el("bar1").style.width = Math.min(100, (t.level_mic1 / LEVEL_FULL) * 100) + "%";
  }
  const c = state.counts || { left: 0, center: 0, front: 0 };
  el("tTally").textContent = "左 " + c.left + " · 中 " + c.center + " · 前 " + c.front;

  const m = state.mpu;
  fmtStatus("mStatus", m);
  if (m) {
    el("mWho").textContent = m.who || "—";
    el("mAccel").textContent = m.accel ? m.accel.join(", ") : "—";
    el("mG").textContent = m.accel_g === undefined ? "—" : m.accel_g.toFixed(3) + " g";
    el("mGyro").textContent = m.gyro ? m.gyro.join(", ") : "—";
    updateCube(m);
  }

  const touches = state.touches || {};
  const transitionCounts = state.touch_transition_counts || {};
  TOUCH_PADS.forEach((pad) => {
    // ``state.touch`` keeps compatibility with dashboards made before PA13
    // support. It is only a fallback for PA09.
    const k = touches[pad] || (pad === "PA09" ? state.touch : null);
    const prefix = touchPrefix(pad);
    fmtStatus(prefix + "Status", k);
    if (k) {
      el(prefix + "Level").innerHTML =
        '<span class="dot ' + (k.level === 1 ? "on" : "") + '"></span>' +
        k.level + "（锁存）";
    }
    const rawCount = Number(transitionCounts[pad] ||
      (pad === "PA09" ? state.touch_transitions : 0) || 0);
    touchCounts[pad] = rawCount;
    const transitions = Math.max(0, rawCount - touchBaseline[pad]);
    el(prefix + "Flips").textContent = transitions;
    paintTouch(pad, k, transitions);
  });

  const n = state.nfc;
  fmtStatus("nStatus", n);
  if (n) {
    el("nIc").textContent = (n.ic || "—") + " / fw " + (n.firmware || "—");
  }
  const uids = state.card_uids || [];
  el("nUid").textContent = uids.length ? uids.join("　") : "还没读到卡";

  const nc = state.netcfg;
  if (nc) {
    const portalActive = !!nc.portal_active;
    el("nPortal").innerHTML =
      '<span class="dot ' + (portalActive ? "on" : "") + '"></span>' +
      (portalActive ? "热点配网页已开启" : "热点配网页未开启");
    el("nSsid").textContent = nc.ssid
      ? (nc.ssid + (nc.ip ? " / " + nc.ip : ""))
      : "—";
    const wifiBits = [];
    wifiBits.push(nc.wifi_connected ? "已连上" : "未连上");
    if (nc.wifi_started !== undefined) {
      wifiBits.push(nc.wifi_started ? "STA已启动" : "STA未启动");
    }
    if (nc.wifi_available !== undefined && !nc.wifi_available) {
      wifiBits.push("Wi‑Fi 不可用");
    }
    if (nc.switching) {
      wifiBits.push("切换中");
    }
    el("nWifi").textContent = wifiBits.join(" / ");
    el("nSavedAp").textContent = nc.saved_ap ? "有" : "无";
    el("nConnectRes").textContent = nc.connect_result === undefined ? "—" : String(nc.connect_result);
  }

  const p = state.power;
  if (p) {
    el("pBig").textContent = p.percent === null || p.percent === undefined
      ? "—" : p.percent + "%";
    el("pBar").style.width =
      (p.percent === null || p.percent === undefined ? 0 : p.percent) + "%";
    el("pBar").style.background = p.charging ? "#4f8cff"
      : (p.percent !== null && p.percent < 20 ? "#ff7a7a" : "#7fe0a0");
    el("pBattery").className = p.present ? "state" : "state warn";
    el("pBattery").textContent = (p.charging ? "⚡ " : "") + p.battery_label +
      "（" + p.battery + "）";
    el("pVolt").textContent = p.volt.toFixed(3) + " V (" + p.millivolt + " mV)";
    el("pLevel").textContent = p.level;
    el("pAge").textContent = state.power_age === null || state.power_age === undefined
      ? "—" : state.power_age.toFixed(1) + " s 前";
    el("pHint").textContent = p.present
      ? (p.charging ? "正在充电，百分比会随时间上升。"
        : (p.charge_done ? "已充满。" : "电池在放电，百分比应缓慢下降。"))
      : "固件报 no-battery：VBAT 分压上没有电压，接上电池后这里才会有数。";
  }

  const cloud = state.cloud || {};
  el("cStage").textContent = cloud.stage_label || "空闲";

  const events = state.events || [];
  el("events").innerHTML = events.slice(-80).reverse().map((item) =>
    '<div><span class="t">' + item.clock + '</span><span class="tag ' + item.kind +
    '">' + item.label + "</span>" + escapeHtml(item.text) + "</div>"
  ).join("");
}

function escapeHtml(text) {
  return String(text).replace(/[&<>]/g, (ch) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[ch]);
}

syncSwatch();
const source = new EventSource("/events");
source.onmessage = (event) => {
  try { render(JSON.parse(event.data)); } catch (err) { console.error(err); }
};
source.onerror = () => {
  el("state").className = "state err";
  el("state").textContent = "与上位机的连接中断，刷新页面重试";
};
setInterval(() => {
  fetch("/api/status").then((r) => r.json()).then(render).catch(() => {});
}, 1500);
</script>
</body>
</html>
"""

HTML = HTML_HEAD + HTML_BODY + HTML_SCRIPT
class Handler(BaseHTTPRequestHandler):
    bench: TestBench

    def _json(self, payload: dict[str, Any], status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return {}
        if length <= 0 or length > 64 * 1024:
            return {}
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return {}
        return payload if isinstance(payload, dict) else {}

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
            self._json(self.bench.snapshot())
            return
        if self.path == "/events":
            channel = self.bench.subscribe()
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
                self.bench.unsubscribe(channel)
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/api/command":
            payload = self._read_json()
            action = str(payload.get("action", ""))
            params = payload.get("params")
            result = self.bench.run_command(
                action, params if isinstance(params, dict) else {}
            )
            self._json(result, 200 if result.get("ok") else 400)
            return
        if self.path == "/api/arm":
            payload = self._read_json()
            self.bench.set_servo_armed(bool(payload.get("armed")))
            self._json(self.bench.snapshot())
            return
        self.send_error(404)

    def log_message(self, format: str, *args: Any) -> None:
        return


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ARCS-MINI V2.1 综合测试上位机")
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
    parser.add_argument("--port", type=int, default=8770)
    parser.add_argument("--no-browser", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)

    bench = TestBench(args.adb, args.transport, args.sample_rate)
    try:
        bench.start()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"启动失败：{exc}", flush=True)
        return 1

    Handler.bench = bench
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    url = f"http://{args.host}:{args.port}/"
    print(f"综合测试上位机：{url}", flush=True)
    print(
        f"transport={bench.transport_id}；页面上的控制会直接下发到板子，"
        "舵机默认锁定，要转动先在页面上勾选解锁",
        flush=True,
    )
    if args.host not in {"127.0.0.1", "localhost", "::1"}:
        print(
            f"注意：绑定在 {args.host} 上没有任何鉴权，同网段的人都能控制这块板子；"
            "只在可信网络里这么用",
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
        bench.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
