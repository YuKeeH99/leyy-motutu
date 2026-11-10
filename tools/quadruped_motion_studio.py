#!/usr/bin/env python3
"""ARCS-MINI V2.1 four-servo motion authoring studio.

The studio deliberately keeps the browser, project model and ADB transport in
one dependency-free program.  Device commands share one persistent ``adb
shell``; slider events are coalesced before they reach that shell, so dragging
cannot build an ever-growing queue of stale positions.

Safety rules are part of the implementation rather than UI decoration:

* every process starts locked and with an emergency-stop state;
* arming performs a fresh ``v2hw power`` check;
* losing ADB or the last browser control lease locks the studio immediately;
* disarm, emergency stop, shutdown and the first command after reconnect all
  execute ``v2hw servo all-off``;
* a failed command never updates the confirmed pose.

The current firmware protocol is line-oriented::

    v2hw motion frame <duration_ms> <easing> <us0> <us1> <us2> <us3> \
        <count> <r> <g> <b> <brightness_percent>
    v2hw servo frame <duration_ms> <easing> <us0> <us1> <us2> <us3>
    v2hw servo state
    v2hw servo stop
    v2hw servo all-off
    v2hw servo watchdog <0|200..5000>
    v2hw rgb release
    v2hw rgb calibration status|reset
    v2hw rgb calibration set <order> <r_gain> <g_gain> <b_gain> <gamma_x100>
    v2hw mpu

``servo frame`` is non-blocking.  Firmware performs the 20 ms interpolation;
the host only chooses the target, transition duration and easing.  Hobby PWM
servos have no position feedback, so the UI consistently calls values
"commanded" or "confirmed command", never an actual measured angle.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
import queue
import re
import secrets
import subprocess
import threading
import time
import uuid
import webbrowser
from dataclasses import dataclass
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Sequence
from urllib.parse import unquote, urlparse


APP_NAME = "ARCS Motion Studio"
PROJECT_FORMAT = "arcs-motion-project"
PROJECT_VERSION = 1
CALIBRATION_FORMAT = "arcs-motion-device-calibrations"
CALIBRATION_VERSION = 1
CALIBRATION_MAX_RECORDS = 64
BOARD_NAME = "arcs_mini_v2_1"

SERVO_COUNT = 4
SERVO_PADS = ("PA04", "PA05", "PA06", "PA07")
SERVO_MIN_US = 500
SERVO_CENTER_US = 1500
SERVO_MAX_US = 2500
SERVO_SAFE_MIN_US = 1000
SERVO_SAFE_MAX_US = 2000
SERVO_FRAME_MIN_MS = 0
SERVO_FRAME_MAX_MS = 60_000
SERVO_LIVE_INTERVAL_S = 0.040
SERVO_LEASE_MS = 1_000
# The first fixture pass used 1300..1700 us.  The operator then confirmed all
# four correctly directed sliders reached that boundary before the mechanism
# could stand.  Passes at 1250..1750 us and 1200..1800 us moved the feet in the
# verified high/low/high/low directions but still left the chassis prone.  The
# real standing pose is 2000/1000/2000/1000.  A lift-and-return gait at those
# endpoints only marched in place, so allow one deliberately small 100 us
# propulsive stroke beyond Home; the firmware absolute guard remains
# 500..2500 us.
# normalize_servo_test_request() still intersects these values with the
# identity-bound per-device soft limits; this is not a firmware hard limit.
SERVO_TEST_MIN_US = 900
SERVO_TEST_MAX_US = 2100
SERVO_TEST_DEFAULT_HOLD_MS = 300
SERVO_TEST_MIN_HOLD_MS = 50
SERVO_TEST_MAX_HOLD_MS = 1_000
POSTURE_TEST_DEFAULT_DURATION_MS = 800
POSTURE_TEST_MIN_DURATION_MS = 50
POSTURE_TEST_MAX_DURATION_MS = 4_000
POSTURE_TEST_WATCHDOG_MARGIN_MS = 500
POSTURE_TEST_COMPLETION_GRACE_MS = 300
POSTURE_TEST_STATE_POLL_MS = 20

# The firmware samples the MPU6050 at 200 Hz. A 10 Hz host preview is responsive
# enough for the operator while leaving ample room on the single ADB shell;
# _imu_loop skips rather than queues whenever motion or safety owns transport.
IMU_POLL_INTERVAL_S = 0.1
IMU_MIN_POLL_INTERVAL_S = 0.05
IMU_COMMAND_TIMEOUT_S = 1.0
IMU_FRESH_AFTER_S = 2.0
IMU_ACCEL_LSB_PER_G = 16_384.0
IMU_GYRO_LSB_PER_DPS = 131.0
IMU_FILTER_TIME_CONSTANT_S = 0.5
IMU_FILTER_RESET_GAP_S = 2.0

LED_MIN_COUNT = 1
LED_MAX_COUNT = 64
LED_DEFAULT_COUNT = 8
LED_MIN_BRIGHTNESS = 0
LED_MAX_BRIGHTNESS = 100
LED_CALIBRATION_VERSION = 1
LED_COLOR_ORDERS = ("RGB", "RBG", "GRB", "GBR", "BRG", "BGR")
LED_GAIN_MIN_PERMILLE = 0
LED_GAIN_MAX_PERMILLE = 1000
LED_GAMMA_MIN_X100 = 100
LED_GAMMA_MAX_X100 = 300

PLAYBACK_MIN_RATE = 0.25
PLAYBACK_MAX_RATE = 4.0
PROJECT_MAX_ACTIONS = 256
ACTION_MAX_FRAMES = 10_000
HTTP_MAX_BODY = 4 * 1024 * 1024
ASSET_DIR = Path(__file__).resolve().with_name("quadruped_studio_assets")
ASSET_TYPES = {
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
    ".md": "text/markdown; charset=utf-8",
    ".glb": "model/gltf-binary",
    ".gltf": "model/gltf+json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".webp": "image/webp",
    ".ico": "image/x-icon",
}

EASINGS = ("linear", "smooth", "ease-in", "ease-out")
END_BEHAVIORS = ("hold", "off")

ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
POWER_RE = re.compile(
    r"VBAT/servo\s+pin2=(?P<millivolt>-?\d+)mV\s+"
    r"level=(?P<level>\S+)\s+battery=(?P<battery>\S+)"
)
MPU_RE = re.compile(
    r"^mpu6050\s+i2c(?P<bus>\d+)/0x(?P<address>[0-9a-fA-F]{2}):\s*"
    r"status=(?P<status>[+-]?\d+)\s+\((?P<status_text>[^)\x00]{1,80})\)\s+"
    r"who=0x(?P<who>[0-9a-fA-F]{2})\s+"
    r"accel=(?P<ax>[+-]?\d+)\s*,\s*(?P<ay>[+-]?\d+)\s*,\s*(?P<az>[+-]?\d+)\s+"
    r"temp=(?P<temp>[+-]?\d+)\s+"
    r"gyro=(?P<gx>[+-]?\d+)\s*,\s*(?P<gy>[+-]?\d+)\s*,\s*(?P<gz>[+-]?\d+)$"
)
CHIP_ID_PATTERNS = (
    re.compile(r"Chip ID:\s*(?P<id>[0-9a-fA-F]{16})"),
    re.compile(r"device(?:_| )id[=: ]+(?P<id>[0-9a-fA-F]{16})", re.IGNORECASE),
)
FIRMWARE_RE = re.compile(r"Firmware version:\s*(?P<version>\S+)")
CHIP_ID_TOKEN_RE = re.compile(r"(?<![0-9a-fA-F])(?P<id>[0-9a-fA-F]{16})(?![0-9a-fA-F])")
RGB_CAL_RE = re.compile(
    r"^RGB_CAL version=(?P<version>\d+) "
    r"order=(?P<order>RGB|RBG|GRB|GBR|BRG|BGR) "
    r"r_gain=(?P<r_gain>\d+) g_gain=(?P<g_gain>\d+) b_gain=(?P<b_gain>\d+) "
    r"gamma=(?P<gamma>\d+) persistent=(?P<persistent>true|false|0|1)$",
    re.IGNORECASE,
)


class StudioError(RuntimeError):
    """Expected error that can be returned to the browser without a traceback."""


class ValidationError(StudioError):
    """Project or request validation failed."""


class DeviceOfflineError(StudioError):
    """The persistent ADB shell is unavailable."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def clean_line(line: str) -> str:
    return ANSI_RE.sub("", line).replace("\r", "").strip()


def protocol_fields(line: str, prefix: str) -> dict[str, str] | None:
    """Parse one short firmware status line without accepting truncated data."""
    marker = prefix + " "
    if not line.startswith(marker) or "\x00" in line:
        return None
    fields = dict(re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s\x00]+)", line[len(marker) :]))
    return fields or None


def protocol_int(fields: dict[str, str], key: str, base: int = 10) -> int:
    try:
        return int(fields[key], base)
    except (KeyError, ValueError) as exc:
        raise ValidationError(f"设备状态字段 {key} 无效") from exc


def protocol_pose(fields: dict[str, str], key: str) -> list[int]:
    try:
        values = [int(item) for item in fields[key].split(",")]
    except (KeyError, ValueError) as exc:
        raise ValidationError(f"设备状态字段 {key} 无效") from exc
    if len(values) != SERVO_COUNT:
        raise ValidationError(f"设备状态字段 {key} 必须包含 4 路")
    return values


def require_int(value: Any, name: str, low: int, high: int) -> int:
    if isinstance(value, bool):
        raise ValidationError(f"{name} 必须是整数")
    try:
        number = int(value)
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"{name} 必须是整数") from exc
    if number < low or number > high:
        raise ValidationError(f"{name} 必须在 {low}..{high} 之间")
    return number


def require_float(value: Any, name: str, low: float, high: float) -> float:
    if isinstance(value, bool):
        raise ValidationError(f"{name} 必须是数字")
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"{name} 必须是数字") from exc
    if not (low <= number <= high):
        raise ValidationError(f"{name} 必须在 {low}..{high} 之间")
    return number


def require_name(value: Any, name: str = "name", maximum: int = 80) -> str:
    if not isinstance(value, str):
        raise ValidationError(f"{name} 必须是文本")
    result = value.strip()
    if not result or len(result) > maximum or any(ord(ch) < 32 for ch in result):
        raise ValidationError(f"{name} 长度必须在 1..{maximum} 之间")
    return result


def require_identifier(value: Any, name: str = "id") -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value):
        raise ValidationError(f"{name} 格式无效")
    return value


def require_exact_keys(value: dict[str, Any], allowed: set[str], required: set[str], name: str) -> None:
    unknown = set(value) - allowed
    missing = required - set(value)
    if unknown:
        raise ValidationError(f"{name} 包含未支持字段: {', '.join(sorted(unknown))}")
    if missing:
        raise ValidationError(f"{name} 缺少字段: {', '.join(sorted(missing))}")


def require_json_int(value: Any, name: str, low: int, high: int) -> int:
    """Require an actual JSON integer; calibration must never round floats."""
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValidationError(f"{name} 必须是整数")
    if value < low or value > high:
        raise ValidationError(f"{name} 必须在 {low}..{high} 之间")
    return value


def require_json_number(value: Any, name: str, low: float, high: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValidationError(f"{name} 必须是数字")
    number = float(value)
    if not math.isfinite(number) or number < low or number > high:
        raise ValidationError(f"{name} 必须在 {low}..{high} 之间")
    return number


def require_chip_id(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{16}", value):
        raise ValidationError("chip_id 必须是 16 位十六进制字符")
    return value.lower()


def require_board(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value):
        raise ValidationError("board 格式无效")
    return value


def require_timestamp(value: Any, name: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value):
        raise ValidationError(f"{name} 必须是 UTC ISO-8601 时间")
    return value


def parse_chip_id_output(value: Any) -> str:
    """Parse one unambiguous 64-bit chip ID from a one-shot command."""
    if not isinstance(value, str) or "\x00" in value:
        raise ValidationError("设备 chip_id 输出无效")
    identities = {match.group("id").lower() for match in CHIP_ID_TOKEN_RE.finditer(value)}
    if len(identities) != 1:
        raise ValidationError("设备未返回唯一可解析的 16 位 chip_id")
    return identities.pop()


def parse_led_calibration_line(value: Any) -> dict[str, Any]:
    """Parse the firmware's complete, single-line persistent RGB calibration."""
    if not isinstance(value, str) or "\x00" in value:
        raise ValidationError("设备灯板校色输出无效")
    match = RGB_CAL_RE.fullmatch(clean_line(value))
    if match is None:
        raise ValidationError("设备未返回完整可解析的 RGB_CAL 状态")
    version = require_json_int(int(match.group("version")), "RGB_CAL.version", 1, LED_CALIBRATION_VERSION)
    return {
        "version": version,
        "color_order": match.group("order").upper(),
        "r_gain_permille": require_json_int(
            int(match.group("r_gain")),
            "RGB_CAL.r_gain",
            LED_GAIN_MIN_PERMILLE,
            LED_GAIN_MAX_PERMILLE,
        ),
        "g_gain_permille": require_json_int(
            int(match.group("g_gain")),
            "RGB_CAL.g_gain",
            LED_GAIN_MIN_PERMILLE,
            LED_GAIN_MAX_PERMILLE,
        ),
        "b_gain_permille": require_json_int(
            int(match.group("b_gain")),
            "RGB_CAL.b_gain",
            LED_GAIN_MIN_PERMILLE,
            LED_GAIN_MAX_PERMILLE,
        ),
        "gamma_x100": require_json_int(
            int(match.group("gamma")),
            "RGB_CAL.gamma",
            LED_GAMMA_MIN_X100,
            LED_GAMMA_MAX_X100,
        ),
        "persistent": match.group("persistent").lower() in {"true", "1"},
    }


def parse_led_calibration_reply(reply: CommandReply) -> dict[str, Any]:
    """Require one unambiguous RGB_CAL record before the command terminator."""
    candidates = [line for line in reply.lines if clean_line(line).startswith("RGB_CAL")]
    if len(candidates) != 1:
        raise ValidationError("设备未返回唯一 RGB_CAL 状态")
    return parse_led_calibration_line(candidates[0])


def normalize_led_calibration_request(value: Any) -> dict[str, Any]:
    """Validate an identity-bound firmware calibration mutation request."""
    if not isinstance(value, dict):
        raise ValidationError("灯板校色请求必须是对象")
    identity_keys = {"connection_epoch", "chip_id", "board"}
    if "reset" in value:
        allowed = identity_keys | {"reset"}
        require_exact_keys(value, allowed, allowed, "灯板校色重置请求")
        if value["reset"] is not True:
            raise ValidationError("灯板校色 reset 必须为 true")
        result: dict[str, Any] = {"reset": True}
    else:
        settings = {
            "color_order",
            "r_gain_permille",
            "g_gain_permille",
            "b_gain_permille",
            "gamma_x100",
        }
        allowed = identity_keys | settings
        require_exact_keys(value, allowed, allowed, "灯板校色请求")
        if not isinstance(value["color_order"], str):
            raise ValidationError("color_order 必须是 RGB 通道顺序")
        color_order = value["color_order"].upper()
        if color_order not in LED_COLOR_ORDERS:
            raise ValidationError(f"color_order 必须是 {', '.join(LED_COLOR_ORDERS)} 之一")
        result = {
            "reset": False,
            "color_order": color_order,
            "r_gain_permille": require_json_int(
                value["r_gain_permille"],
                "r_gain_permille",
                LED_GAIN_MIN_PERMILLE,
                LED_GAIN_MAX_PERMILLE,
            ),
            "g_gain_permille": require_json_int(
                value["g_gain_permille"],
                "g_gain_permille",
                LED_GAIN_MIN_PERMILLE,
                LED_GAIN_MAX_PERMILLE,
            ),
            "b_gain_permille": require_json_int(
                value["b_gain_permille"],
                "b_gain_permille",
                LED_GAIN_MIN_PERMILLE,
                LED_GAIN_MAX_PERMILLE,
            ),
            "gamma_x100": require_json_int(
                value["gamma_x100"],
                "gamma_x100",
                LED_GAMMA_MIN_X100,
                LED_GAMMA_MAX_X100,
            ),
        }
    result.update(
        {
            "connection_epoch": require_json_int(
                value["connection_epoch"], "connection_epoch", 1, 2_147_483_647
            ),
            "chip_id": require_chip_id(value["chip_id"]),
            "board": require_board(value["board"]),
        }
    )
    return result


def normalize_servo_test_request(value: Any, profile: dict[str, Any]) -> dict[str, Any]:
    """Validate one identity-bound, deliberately narrow calibration jog."""
    if not isinstance(value, dict):
        raise ValidationError("单舵机点动请求必须是对象")
    required = {"channel", "connection_epoch", "chip_id", "board", "safety_confirmed"}
    allowed = required | {"pulse_us", "hold_ms"}
    require_exact_keys(value, allowed, required, "单舵机点动请求")
    if value["safety_confirmed"] is not True:
        raise ValidationError("safety_confirmed 必须明确为 true")

    channel = require_json_int(value["channel"], "channel", 0, SERVO_COUNT - 1)
    servos = profile.get("servos") if isinstance(profile, dict) else None
    channel_profile = servos[channel] if isinstance(servos, list) and channel < len(servos) else {}
    if not isinstance(channel_profile, dict):
        channel_profile = {}
    profile_min = require_int(
        channel_profile.get("min_us", SERVO_SAFE_MIN_US),
        f"servos[{channel}].min_us",
        SERVO_MIN_US,
        SERVO_MAX_US,
    )
    profile_max = require_int(
        channel_profile.get("max_us", SERVO_SAFE_MAX_US),
        f"servos[{channel}].max_us",
        SERVO_MIN_US,
        SERVO_MAX_US,
    )
    low = max(SERVO_TEST_MIN_US, profile_min)
    high = min(SERVO_TEST_MAX_US, profile_max)
    if low > high:
        raise ValidationError(f"舵机通道 {channel} 的校准软限位没有可用的安全点动范围")
    default_pulse = require_int(
        channel_profile.get("center_us", SERVO_CENTER_US),
        f"servos[{channel}].center_us",
        SERVO_MIN_US,
        SERVO_MAX_US,
    )
    default_pulse = max(low, min(high, default_pulse))
    pulse_us = require_json_int(value.get("pulse_us", default_pulse), "pulse_us", low, high)
    hold_ms = require_json_int(
        value.get("hold_ms", SERVO_TEST_DEFAULT_HOLD_MS),
        "hold_ms",
        SERVO_TEST_MIN_HOLD_MS,
        SERVO_TEST_MAX_HOLD_MS,
    )
    return {
        "channel": channel,
        "pulse_us": pulse_us,
        "hold_ms": hold_ms,
        "connection_epoch": require_json_int(
            value["connection_epoch"], "connection_epoch", 1, 2_147_483_647
        ),
        "chip_id": require_chip_id(value["chip_id"]),
        "board": require_board(value["board"]),
        "safety_confirmed": True,
        "safe_min_us": low,
        "safe_max_us": high,
    }


def normalize_posture_test_request(value: Any, profile: dict[str, Any]) -> dict[str, Any]:
    """Validate one identity-bound, locked-only four-channel posture test."""
    if not isinstance(value, dict):
        raise ValidationError("四腿同步姿态请求必须是对象")
    required = {
        "servos_us",
        "duration_ms",
        "connection_epoch",
        "chip_id",
        "board",
        "safety_confirmed",
    }
    require_exact_keys(value, required, required, "四腿同步姿态请求")
    if value["safety_confirmed"] is not True:
        raise ValidationError("safety_confirmed 必须明确为 true")
    raw_servos = value["servos_us"]
    if not isinstance(raw_servos, list) or len(raw_servos) != SERVO_COUNT:
        raise ValidationError("servos_us 必须恰好包含 4 个整数")

    profile_servos = profile.get("servos") if isinstance(profile, dict) else None
    pulses: list[int] = []
    safe_ranges: list[dict[str, int]] = []
    for channel, raw_pulse in enumerate(raw_servos):
        channel_profile = (
            profile_servos[channel]
            if isinstance(profile_servos, list)
            and channel < len(profile_servos)
            and isinstance(profile_servos[channel], dict)
            else {}
        )
        profile_min = require_int(
            channel_profile.get("min_us", SERVO_SAFE_MIN_US),
            f"servos[{channel}].min_us",
            SERVO_MIN_US,
            SERVO_MAX_US,
        )
        profile_max = require_int(
            channel_profile.get("max_us", SERVO_SAFE_MAX_US),
            f"servos[{channel}].max_us",
            SERVO_MIN_US,
            SERVO_MAX_US,
        )
        low = max(SERVO_TEST_MIN_US, profile_min)
        high = min(SERVO_TEST_MAX_US, profile_max)
        if low > high:
            raise ValidationError(f"舵机通道 {channel} 没有可用的同步测试范围")
        pulses.append(
            require_json_int(raw_pulse, f"servos_us[{channel}]", low, high)
        )
        safe_ranges.append({"min_us": low, "max_us": high})

    return {
        "servos_us": pulses,
        "duration_ms": require_json_int(
            value["duration_ms"],
            "duration_ms",
            POSTURE_TEST_MIN_DURATION_MS,
            POSTURE_TEST_MAX_DURATION_MS,
        ),
        "connection_epoch": require_json_int(
            value["connection_epoch"], "connection_epoch", 1, 2_147_483_647
        ),
        "chip_id": require_chip_id(value["chip_id"]),
        "board": require_board(value["board"]),
        "safety_confirmed": True,
        "safe_ranges": safe_ranges,
    }


def normalize_color(value: Any) -> str:
    if not isinstance(value, str):
        raise ValidationError("led.color 必须是 #RRGGBB")
    value = value.strip().upper()
    if not re.fullmatch(r"#[0-9A-F]{6}", value):
        raise ValidationError("led.color 必须是 #RRGGBB")
    return value


def normalize_led(value: Any, default_count: int = LED_DEFAULT_COUNT) -> dict[str, Any]:
    if value is None:
        value = {}
    if not isinstance(value, dict):
        raise ValidationError("led 必须是对象")
    return {
        "enabled": bool(value.get("enabled", False)),
        "color": normalize_color(value.get("color", "#20D6C7")),
        "brightness": require_int(
            value.get("brightness", 35), "led.brightness", LED_MIN_BRIGHTNESS, LED_MAX_BRIGHTNESS
        ),
        "pixel_count": require_int(
            value.get("pixel_count", default_count), "led.pixel_count", LED_MIN_COUNT, LED_MAX_COUNT
        ),
    }


def normalize_easing(value: Any) -> str:
    easing = str(value or "smooth")
    easing = {"ease-in-out": "smooth", "step": "linear"}.get(easing, easing)
    if easing not in EASINGS:
        raise ValidationError(f"easing 必须是 {', '.join(EASINGS)} 之一")
    return easing


def normalize_servos(value: Any, profile: dict[str, Any] | None = None) -> list[int]:
    if not isinstance(value, (list, tuple)) or len(value) != SERVO_COUNT:
        raise ValidationError("servos_us 必须恰好包含 4 个整数")
    limits = (profile or {}).get("servos") if isinstance(profile, dict) else None
    result: list[int] = []
    for index, item in enumerate(value):
        low = SERVO_SAFE_MIN_US
        high = SERVO_SAFE_MAX_US
        if isinstance(limits, list) and index < len(limits) and isinstance(limits[index], dict):
            low = max(SERVO_MIN_US, int(limits[index].get("min_us", low)))
            high = min(SERVO_MAX_US, int(limits[index].get("max_us", high)))
        if isinstance(item, bool):
            raise ValidationError(f"servos_us[{index}] 必须是整数")
        try:
            number = int(item)
        except (TypeError, ValueError) as exc:
            raise ValidationError(f"servos_us[{index}] 必须是整数") from exc
        # A slider race or an imported project must never escape the calibrated
        # soft range, and that soft range itself is already intersected with
        # the firmware's absolute 500..2500 us range.
        result.append(max(low, min(high, number)))
    return result


def default_device_profile() -> dict[str, Any]:
    return {
        "id": "arcs-mini-v2-1-default",
        "revision": 1,
        "board": BOARD_NAME,
        "servo_frequency_hz": 50,
        "servos": [
            {
                "channel": index,
                "pin": SERVO_PADS[index],
                "name": f"通道 {index}",
                "min_us": SERVO_SAFE_MIN_US,
                "center_us": SERVO_CENTER_US,
                "max_us": SERVO_SAFE_MAX_US,
                "invert": False,
                "trim_us": 0,
            }
            for index in range(SERVO_COUNT)
        ],
        "led": {
            "pin": "PA08",
            "protocol": "ws2812-grb",
            "pixel_count": LED_DEFAULT_COUNT,
            "max_pixel_count": LED_MAX_COUNT,
        },
    }


def default_led() -> dict[str, Any]:
    return {"enabled": False, "color": "#20D6C7", "brightness": 35, "pixel_count": LED_DEFAULT_COUNT}


def default_pose() -> dict[str, Any]:
    return {
        "servos_us": [SERVO_CENTER_US] * SERVO_COUNT,
        "duration_ms": 320,
        "easing": "smooth",
        "led": default_led(),
        "source": "system",
    }


def default_project() -> dict[str, Any]:
    stamp = utc_now()
    return {
        "format": PROJECT_FORMAT,
        "version": PROJECT_VERSION,
        "id": uuid.uuid4().hex,
        "name": "ARCS-MINI 动作项目",
        "created_at": stamp,
        "updated_at": stamp,
        "device_profile": default_device_profile(),
        "device_calibrations": default_calibration_collection(),
        "actions": [],
    }


def default_calibration_collection() -> dict[str, Any]:
    return {"format": CALIBRATION_FORMAT, "version": CALIBRATION_VERSION, "records": []}


def normalize_servo_calibration(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list) or len(value) != SERVO_COUNT:
        raise ValidationError("servo_calibration 必须恰好包含 4 路")
    result: list[dict[str, Any]] = []
    allowed = {"channel", "min_us", "center_us", "max_us", "reverse", "trim_deg"}
    for index, raw in enumerate(value):
        if not isinstance(raw, dict):
            raise ValidationError(f"servo_calibration[{index}] 必须是对象")
        require_exact_keys(raw, allowed, allowed, f"servo_calibration[{index}]")
        channel = require_json_int(raw["channel"], f"servo_calibration[{index}].channel", index, index)
        low = require_json_int(raw["min_us"], f"servo_calibration[{index}].min_us", SERVO_MIN_US, SERVO_MAX_US)
        high = require_json_int(raw["max_us"], f"servo_calibration[{index}].max_us", SERVO_MIN_US, SERVO_MAX_US)
        if high <= low:
            raise ValidationError(f"servo_calibration[{index}].max_us 必须大于 min_us")
        center = require_json_int(raw["center_us"], f"servo_calibration[{index}].center_us", low, high)
        if center in {low, high}:
            raise ValidationError(f"servo_calibration[{index}] 必须满足 min_us < center_us < max_us")
        if not isinstance(raw["reverse"], bool):
            raise ValidationError(f"servo_calibration[{index}].reverse 必须是布尔值")
        result.append(
            {
                "channel": channel,
                "min_us": low,
                "center_us": center,
                "max_us": high,
                "reverse": raw["reverse"],
                "trim_deg": require_json_int(
                    raw["trim_deg"], f"servo_calibration[{index}].trim_deg", -30, 30
                ),
            }
        )
    return result


def normalize_calibration_record(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValidationError("校准记录必须是对象")
    core = {
        "chip_id",
        "board",
        "mounting_revision",
        "servo_calibration",
        "home_pose_us",
        "imu_mounting_baseline_deg",
    }
    allowed = core | {"created_at", "updated_at"}
    require_exact_keys(payload, allowed, core, "calibration record")
    servos = normalize_servo_calibration(payload["servo_calibration"])
    home_raw = payload["home_pose_us"]
    if home_raw is None:
        home = None
    else:
        if not isinstance(home_raw, list) or len(home_raw) != SERVO_COUNT:
            raise ValidationError("home_pose_us 必须为 null 或恰好包含 4 个整数")
        home = [
            require_json_int(item, f"home_pose_us[{index}]", servos[index]["min_us"], servos[index]["max_us"])
            for index, item in enumerate(home_raw)
        ]
    imu = payload["imu_mounting_baseline_deg"]
    if imu is None:
        normalized_imu = None
    else:
        if not isinstance(imu, dict):
            raise ValidationError("imu_mounting_baseline_deg 必须为 null 或对象")
        # Deliberately excludes yaw: an MPU6050 has no absolute yaw reference.
        require_exact_keys(imu, {"roll", "pitch"}, {"roll", "pitch"}, "imu_mounting_baseline_deg")
        normalized_imu = {
            "roll": require_json_number(imu["roll"], "imu_mounting_baseline_deg.roll", -180.0, 180.0),
            "pitch": require_json_number(imu["pitch"], "imu_mounting_baseline_deg.pitch", -90.0, 90.0),
        }
    stamp = utc_now()
    return {
        "chip_id": require_chip_id(payload["chip_id"]),
        "board": require_board(payload["board"]),
        "mounting_revision": require_json_int(
            payload["mounting_revision"], "mounting_revision", 1, 1_000_000
        ),
        "servo_calibration": servos,
        "home_pose_us": home,
        "imu_mounting_baseline_deg": normalized_imu,
        "created_at": require_timestamp(payload.get("created_at", stamp), "created_at"),
        "updated_at": require_timestamp(payload.get("updated_at", stamp), "updated_at"),
    }


def normalize_calibration_collection(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError("device_calibrations 必须是对象")
    require_exact_keys(value, {"format", "version", "records"}, {"format", "version", "records"}, "device_calibrations")
    if value["format"] != CALIBRATION_FORMAT:
        raise ValidationError("device_calibrations.format 无效")
    version = require_json_int(value["version"], "device_calibrations.version", CALIBRATION_VERSION, CALIBRATION_VERSION)
    raw_records = value["records"]
    if not isinstance(raw_records, list) or len(raw_records) > CALIBRATION_MAX_RECORDS:
        raise ValidationError(f"device_calibrations.records 最多 {CALIBRATION_MAX_RECORDS} 条")
    records = [normalize_calibration_record(item) for item in raw_records]
    chip_ids = [item["chip_id"] for item in records]
    if len(chip_ids) != len(set(chip_ids)):
        raise ValidationError("device_calibrations.records 的 chip_id 不能重复")
    return {"format": CALIBRATION_FORMAT, "version": version, "records": records}


def normalize_pose(payload: Any, profile: dict[str, Any] | None = None) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValidationError("姿态必须是 JSON 对象")
    led_default_count = LED_DEFAULT_COUNT
    if isinstance(profile, dict):
        led_profile = profile.get("led")
        if isinstance(led_profile, dict):
            led_default_count = int(led_profile.get("pixel_count", LED_DEFAULT_COUNT))
    return {
        "servos_us": normalize_servos(payload.get("servos_us"), profile),
        "duration_ms": require_int(
            payload.get("duration_ms", 320), "duration_ms", SERVO_FRAME_MIN_MS, SERVO_FRAME_MAX_MS
        ),
        "easing": normalize_easing(payload.get("easing", "smooth")),
        "led": normalize_led(payload.get("led"), led_default_count),
        "source": str(payload.get("source", "manual"))[:32],
    }


def normalize_frame(payload: Any, profile: dict[str, Any], fallback_id: str | None = None) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValidationError("关键帧必须是对象")
    pose = normalize_pose(payload, profile)
    return {
        "id": require_identifier(payload.get("id", fallback_id or uuid.uuid4().hex)),
        "time_ms": require_int(payload.get("time_ms", 0), "time_ms", 0, 86_400_000),
        "duration_ms": pose["duration_ms"],
        "easing": pose["easing"],
        "servos_us": pose["servos_us"],
        "led": pose["led"],
    }


def normalize_loop(value: Any, duration_ms: int) -> dict[str, Any]:
    if isinstance(value, bool):
        return {
            "enabled": value,
            "start_ms": 0,
            "end_ms": duration_ms,
            "count": 0 if value else 1,
        }
    if value is None:
        value = {}
    if not isinstance(value, dict):
        raise ValidationError("loop 必须是对象")
    enabled = bool(value.get("enabled", False))
    start_ms = require_int(value.get("start_ms", 0), "loop.start_ms", 0, max(duration_ms, 0))
    end_ms = require_int(value.get("end_ms", duration_ms), "loop.end_ms", 0, max(duration_ms, 0))
    if enabled and end_ms <= start_ms:
        raise ValidationError("loop.end_ms 必须大于 loop.start_ms")
    return {
        "enabled": enabled,
        "start_ms": start_ms,
        "end_ms": end_ms,
        "count": require_int(value.get("count", 1), "loop.count", 0, 10_000),
    }


def normalize_action(payload: Any, profile: dict[str, Any], existing_id: str | None = None) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValidationError("动作必须是对象")
    raw_frames = payload.get("keyframes", [])
    if not isinstance(raw_frames, list):
        raise ValidationError("keyframes 必须是数组")
    if len(raw_frames) > ACTION_MAX_FRAMES:
        raise ValidationError(f"单个动作最多 {ACTION_MAX_FRAMES} 帧")
    frames = [normalize_frame(item, profile) for item in raw_frames]
    frames.sort(key=lambda item: item["time_ms"])
    previous = -1
    seen_ids: set[str] = set()
    for frame in frames:
        if frame["time_ms"] <= previous:
            raise ValidationError("关键帧 time_ms 必须严格递增")
        if frame["id"] in seen_ids:
            raise ValidationError("关键帧 id 不能重复")
        previous = frame["time_ms"]
        seen_ids.add(frame["id"])
    inferred_duration = max((item["time_ms"] + item["duration_ms"] for item in frames), default=0)
    requested_duration = require_int(payload.get("duration_ms", inferred_duration), "action.duration_ms", 0, 86_400_000)
    duration_ms = max(inferred_duration, requested_duration)
    end_behavior = str(payload.get("end_behavior", "hold"))
    if end_behavior not in END_BEHAVIORS:
        raise ValidationError("end_behavior 必须是 hold 或 off")
    return {
        "id": require_identifier(existing_id or payload.get("id", uuid.uuid4().hex)),
        "name": require_name(payload.get("name", "未命名动作"), "action.name"),
        "description": str(payload.get("description", payload.get("note", "")))[:500],
        "duration_ms": duration_ms,
        "default_rate": require_float(payload.get("default_rate", 1.0), "default_rate", PLAYBACK_MIN_RATE, PLAYBACK_MAX_RATE),
        "end_behavior": end_behavior,
        "loop": normalize_loop(payload.get("loop"), duration_ms),
        "keyframes": frames,
    }


def normalize_device_profile(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValidationError("device_profile 必须是对象")
    profile = default_device_profile()
    profile["id"] = require_identifier(value.get("id", profile["id"]), "device_profile.id")
    profile["revision"] = require_int(value.get("revision", 1), "device_profile.revision", 1, 1_000_000)
    profile["board"] = str(value.get("board", BOARD_NAME))
    raw_servos = value.get("servos", profile["servos"])
    if not isinstance(raw_servos, list) or len(raw_servos) != SERVO_COUNT:
        raise ValidationError("device_profile.servos 必须包含 4 路")
    servos: list[dict[str, Any]] = []
    for index, raw in enumerate(raw_servos):
        if not isinstance(raw, dict):
            raise ValidationError(f"device_profile.servos[{index}] 无效")
        low = require_int(
            raw.get("min_us", SERVO_SAFE_MIN_US), f"servo[{index}].min_us", SERVO_MIN_US, SERVO_MAX_US
        )
        high = require_int(
            raw.get("max_us", SERVO_SAFE_MAX_US), f"servo[{index}].max_us", SERVO_MIN_US, SERVO_MAX_US
        )
        center = require_int(raw.get("center_us", SERVO_CENTER_US), f"servo[{index}].center_us", low, high)
        if high <= low:
            raise ValidationError(f"servo[{index}] max_us 必须大于 min_us")
        servos.append(
            {
                "channel": index,
                "pin": SERVO_PADS[index],
                "name": require_name(raw.get("name", f"通道 {index}"), f"servo[{index}].name", 30),
                "min_us": low,
                "center_us": center,
                "max_us": high,
                "invert": bool(raw.get("invert", False)),
                "trim_us": require_int(raw.get("trim_us", 0), f"servo[{index}].trim_us", -500, 500),
            }
        )
    profile["servos"] = servos
    led_raw = value.get("led", {})
    if not isinstance(led_raw, dict):
        raise ValidationError("device_profile.led 必须是对象")
    profile["led"] = {
        "pin": "PA08",
        "protocol": "ws2812-grb",
        "pixel_count": require_int(
            led_raw.get("pixel_count", LED_DEFAULT_COUNT), "device_profile.led.pixel_count", LED_MIN_COUNT, LED_MAX_COUNT
        ),
        "max_pixel_count": LED_MAX_COUNT,
    }
    return profile


def normalize_project(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValidationError("项目必须是 JSON 对象")
    if payload.get("format", PROJECT_FORMAT) != PROJECT_FORMAT:
        raise ValidationError("不是 ARCS Motion Studio 项目")
    version = require_int(payload.get("version", PROJECT_VERSION), "version", PROJECT_VERSION, PROJECT_VERSION)
    raw_profile = payload.get("device_profile")
    if raw_profile is None and isinstance(payload.get("calibration"), list):
        raw_profile = default_device_profile()
        for index, calibration in enumerate(payload["calibration"][:SERVO_COUNT]):
            if not isinstance(calibration, dict):
                continue
            servo = raw_profile["servos"][index]
            servo["min_us"] = calibration.get("min_us", servo["min_us"])
            servo["center_us"] = calibration.get("center_us", servo["center_us"])
            servo["max_us"] = calibration.get("max_us", servo["max_us"])
            servo["invert"] = bool(calibration.get("reverse", False))
            # UI trim_deg is already baked into its frame pulses. Keeping the
            # firmware-facing trim at zero prevents applying the trim twice.
            servo["trim_us"] = 0
    profile = normalize_device_profile(raw_profile or default_device_profile())
    raw_actions = payload.get("actions", [])
    if not isinstance(raw_actions, list) or len(raw_actions) > PROJECT_MAX_ACTIONS:
        raise ValidationError(f"actions 必须是数组，且最多 {PROJECT_MAX_ACTIONS} 个")
    actions = [normalize_action(item, profile) for item in raw_actions]
    ids = [item["id"] for item in actions]
    if len(ids) != len(set(ids)):
        raise ValidationError("动作 id 不能重复")
    return {
        "format": PROJECT_FORMAT,
        "version": version,
        "id": require_identifier(payload.get("id", uuid.uuid4().hex), "project.id"),
        "name": require_name(payload.get("name", "ARCS-MINI 动作项目"), "project.name"),
        "created_at": str(payload.get("created_at", utc_now()))[:40],
        "updated_at": utc_now(),
        "device_profile": profile,
        "device_calibrations": normalize_calibration_collection(payload["device_calibrations"])
        if "device_calibrations" in payload
        else default_calibration_collection(),
        "actions": actions,
    }


class ProjectStore:
    """Validated project model with optional atomic JSON persistence."""

    def __init__(self, path: Path | None = None) -> None:
        self.path = path
        self.project = default_project()
        if path is not None and path.is_file():
            self.project = normalize_project(json.loads(path.read_text(encoding="utf-8")))

    def snapshot(self) -> dict[str, Any]:
        return copy.deepcopy(self.project)

    def replace(self, payload: Any) -> dict[str, Any]:
        if isinstance(payload, dict) and "device_calibrations" not in payload:
            payload = copy.deepcopy(payload)
            payload["device_calibrations"] = copy.deepcopy(self.project["device_calibrations"])
        self.project = normalize_project(payload)
        self.save()
        return self.snapshot()

    def save(self) -> None:
        self._write_project(self.project)

    def _write_project(self, project: dict[str, Any]) -> None:
        if self.path is None:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp = self.path.with_name(f".{self.path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
        try:
            with temp.open("x", encoding="utf-8") as stream:
                json.dump(project, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temp, self.path)
            # Persist the directory entry as well. Some filesystems/platforms do
            # not support opening directories, so durability enhancement is
            # best-effort while the atomic replace above remains mandatory.
            try:
                directory_fd = os.open(self.path.parent, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            except OSError:
                pass
        finally:
            try:
                temp.unlink()
            except FileNotFoundError:
                pass

    def get_calibration(self, chip_id: str) -> dict[str, Any] | None:
        identity = require_chip_id(chip_id)
        for record in self.project["device_calibrations"]["records"]:
            if record["chip_id"] == identity:
                return copy.deepcopy(record)
        return None

    def put_calibration(self, payload: Any) -> dict[str, Any]:
        candidate_record = normalize_calibration_record(payload)
        current = self.get_calibration(candidate_record["chip_id"])
        stamp = utc_now()
        candidate_record["created_at"] = current["created_at"] if current is not None else stamp
        candidate_record["updated_at"] = stamp
        candidate_project = copy.deepcopy(self.project)
        records = candidate_project["device_calibrations"]["records"]
        for index, record in enumerate(records):
            if record["chip_id"] == candidate_record["chip_id"]:
                records[index] = candidate_record
                break
        else:
            if len(records) >= CALIBRATION_MAX_RECORDS:
                raise ValidationError("设备校准记录数量已达上限")
            records.append(candidate_record)
        candidate_project["updated_at"] = stamp
        # Save the complete candidate before exposing it in memory. A failed
        # fsync/replace therefore leaves both the old file and model intact.
        self._write_project(candidate_project)
        self.project = candidate_project
        return copy.deepcopy(candidate_record)

    def get_action(self, action_id: str) -> dict[str, Any]:
        for action in self.project["actions"]:
            if action["id"] == action_id:
                return copy.deepcopy(action)
        raise ValidationError("动作不存在")

    def create_action(self, payload: Any, profile: dict[str, Any] | None = None) -> dict[str, Any]:
        if len(self.project["actions"]) >= PROJECT_MAX_ACTIONS:
            raise ValidationError("动作数量已达上限")
        action = normalize_action(payload, profile or self.project["device_profile"])
        self.project["actions"].append(action)
        self._touch()
        return copy.deepcopy(action)

    def update_action(
        self,
        action_id: str,
        payload: Any,
        profile: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        require_identifier(action_id, "action_id")
        for index, current in enumerate(self.project["actions"]):
            if current["id"] == action_id:
                action = normalize_action(
                    payload,
                    profile or self.project["device_profile"],
                    existing_id=action_id,
                )
                self.project["actions"][index] = action
                self._touch()
                return copy.deepcopy(action)
        raise ValidationError("动作不存在")

    def delete_action(self, action_id: str) -> None:
        require_identifier(action_id, "action_id")
        before = len(self.project["actions"])
        self.project["actions"] = [item for item in self.project["actions"] if item["id"] != action_id]
        if len(self.project["actions"]) == before:
            raise ValidationError("动作不存在")
        self._touch()

    def _touch(self) -> None:
        self.project["updated_at"] = utc_now()
        self.save()


@dataclass(frozen=True)
class CommandReply:
    ok: bool
    lines: tuple[str, ...]
    elapsed_ms: float
    error_code: int | None = None


def parse_mpu_reply(reply: CommandReply) -> dict[str, Any]:
    """Parse the complete, single-line ``v2hw mpu`` payload.

    The shell may contain unrelated asynchronous log lines, so only a full
    MPU6050 line is considered.  Values are checked against the firmware's
    int16 raw-sample type before they enter the attitude filter.
    """
    matches = [match for line in reply.lines if (match := MPU_RE.fullmatch(clean_line(line)))]
    if not matches:
        raise StudioError("设备未返回完整可解析的 MPU6050 数据")
    match = matches[-1]
    try:
        bus = int(match.group("bus"))
        address = int(match.group("address"), 16)
        status = int(match.group("status"))
        who_am_i = int(match.group("who"), 16)
        values = {
            key: int(match.group(key))
            for key in ("ax", "ay", "az", "temp", "gx", "gy", "gz")
        }
    except ValueError as exc:
        raise StudioError("MPU6050 数据字段无效") from exc
    if bus != 0 or address != 0x68:
        raise StudioError(f"MPU6050 总线或地址异常: i2c{bus}/0x{address:02x}")
    if status != 0:
        raise StudioError(f"MPU6050 返回错误 status={status} ({match.group('status_text')})")
    if who_am_i != 0x68:
        raise StudioError(f"MPU6050 WHO_AM_I 异常: 0x{who_am_i:02x}")
    if any(value < -32_768 or value > 32_767 for value in values.values()):
        raise StudioError("MPU6050 原始数据超出 int16 范围")
    return {
        "bus": bus,
        "address": address,
        "who_am_i": who_am_i,
        "device_status": match.group("status_text"),
        "accel": {"x": values["ax"], "y": values["ay"], "z": values["az"]},
        "gyro": {"x": values["gx"], "y": values["gy"], "z": values["gz"]},
        "temp": values["temp"],
    }


def _wrap_degrees(value: float) -> float:
    return (value + 180.0) % 360.0 - 180.0


def _fuse_wrapped_degrees(predicted: float, measured: float, alpha: float) -> float:
    """Blend two angles along the shortest arc while keeping a canonical state."""
    correction = (1.0 - alpha) * _wrap_degrees(measured - predicted)
    return _wrap_degrees(predicted + correction)


class ImuComplementaryFilter:
    """Stateful roll/pitch fusion with explicitly relative, drifting yaw."""

    def __init__(self, time_constant_s: float = IMU_FILTER_TIME_CONSTANT_S) -> None:
        self.time_constant_s = max(0.01, float(time_constant_s))
        self._lock = threading.Lock()
        self.reset()

    def reset(self) -> None:
        with self._lock:
            self._initialized = False
            self._last_monotonic: float | None = None
            self._roll_deg = 0.0
            self._pitch_deg = 0.0
            self._yaw_deg = 0.0

    def update(self, raw: dict[str, Any], *, monotonic_s: float | None = None) -> dict[str, Any]:
        now = time.monotonic() if monotonic_s is None else float(monotonic_s)
        accel = raw["accel"]
        gyro = raw["gyro"]
        ax = float(accel["x"]) / IMU_ACCEL_LSB_PER_G
        ay = float(accel["y"]) / IMU_ACCEL_LSB_PER_G
        az = float(accel["z"]) / IMU_ACCEL_LSB_PER_G
        gx = float(gyro["x"]) / IMU_GYRO_LSB_PER_DPS
        gy = float(gyro["y"]) / IMU_GYRO_LSB_PER_DPS
        gz = float(gyro["z"]) / IMU_GYRO_LSB_PER_DPS
        accel_norm = math.sqrt(ax * ax + ay * ay + az * az)
        accel_usable = accel_norm >= 0.05
        accel_trusted = 0.5 <= accel_norm <= 1.5
        accel_roll = math.degrees(math.atan2(ay, az)) if accel_usable else 0.0
        accel_pitch = (
            math.degrees(math.atan2(-ax, math.sqrt(ay * ay + az * az))) if accel_usable else 0.0
        )

        with self._lock:
            gap_reset = False
            if not self._initialized:
                self._roll_deg = accel_roll
                self._pitch_deg = accel_pitch
                self._yaw_deg = 0.0
                self._initialized = True
                dt = 0.0
            else:
                dt = max(0.0, now - (self._last_monotonic if self._last_monotonic is not None else now))
                if dt > IMU_FILTER_RESET_GAP_S:
                    # Do not integrate a long ADB/disconnect gap. Roll/pitch can
                    # safely re-anchor to gravity; yaw has no absolute reference.
                    gap_reset = True
                    dt = 0.0
                    if accel_usable:
                        self._roll_deg = accel_roll
                        self._pitch_deg = accel_pitch
                    self._yaw_deg = 0.0
                else:
                    roll_from_gyro = _wrap_degrees(self._roll_deg + gx * dt)
                    pitch_from_gyro = _wrap_degrees(self._pitch_deg + gy * dt)
                    if accel_trusted:
                        alpha = self.time_constant_s / (self.time_constant_s + dt) if dt > 0.0 else 1.0
                        self._roll_deg = _fuse_wrapped_degrees(roll_from_gyro, accel_roll, alpha)
                        self._pitch_deg = _fuse_wrapped_degrees(pitch_from_gyro, accel_pitch, alpha)
                    else:
                        self._roll_deg = roll_from_gyro
                        self._pitch_deg = pitch_from_gyro
                    self._yaw_deg = _wrap_degrees(self._yaw_deg + gz * dt)
            self._roll_deg = _wrap_degrees(self._roll_deg)
            self._pitch_deg = _wrap_degrees(self._pitch_deg)
            self._yaw_deg = _wrap_degrees(self._yaw_deg)
            self._last_monotonic = now
            return {
                "roll_deg": round(self._roll_deg, 3),
                "pitch_deg": round(self._pitch_deg, 3),
                "yaw_deg": round(self._yaw_deg, 3),
                "frame": "mpu6050_sensor_axes",
                "mounting_calibrated": False,
                "yaw_reference": "relative_since_filter_reset",
                "yaw_absolute": False,
                "yaw_drift_expected": True,
                "accel_trusted": accel_trusted,
                "filter_gap_reset": gap_reset,
            }


def adb_environment() -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("ADB_LIBUSB", "0")
    return env


def discover_transports(adb: str = "adb") -> list[dict[str, Any]]:
    completed = subprocess.run(
        [adb, "devices", "-l"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=4.0,
        check=False,
        env=adb_environment(),
    )
    transports: list[dict[str, Any]] = []
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
        transports.append(
            {
                "transport_id": transport_id,
                "serial": fields[0],
                "usb": attrs.get("usb", "unknown"),
                "model": attrs.get("model", "unknown"),
            }
        )
    return sorted(transports, key=lambda item: (str(item["usb"]), item["transport_id"]))


class AdbPersistentShell:
    """Reconnectable ADB transport with exactly one long-lived device shell."""

    def __init__(self, adb: str = "adb", transport_id: int | None = None) -> None:
        self.adb = adb
        self.requested_transport_id = transport_id
        self.transport_id: int | None = None
        self.serial: str | None = None
        self.usb: str | None = None
        self._cached_chip_id: str | None = None
        self._identity_transport_id: int | None = None
        self._process: subprocess.Popen[str] | None = None
        self._process_lock = threading.Lock()
        self._command_lock = threading.Lock()
        self._responses: queue.Queue[str | None] = queue.Queue()
        self._running = False
        self._connected = threading.Event()
        self._manager: threading.Thread | None = None
        self._reader: threading.Thread | None = None
        self._state_callback: Callable[[bool, str | None], None] | None = None
        self._line_callback: Callable[[str], None] | None = None
        self.last_error: str | None = None

    @property
    def online(self) -> bool:
        with self._process_lock:
            process = self._process
            return bool(self._connected.is_set() and process is not None and process.poll() is None)

    @property
    def command_busy(self) -> bool:
        """Allow best-effort telemetry to yield to motion/safety commands."""
        return self._command_lock.locked()

    def start(
        self,
        state_callback: Callable[[bool, str | None], None] | None = None,
        line_callback: Callable[[str], None] | None = None,
    ) -> None:
        if self._running:
            return
        self._state_callback = state_callback
        self._line_callback = line_callback
        self._running = True
        self._manager = threading.Thread(target=self._connection_loop, name="adb-motion-manager", daemon=True)
        self._manager.start()

    def stop(self) -> None:
        self._running = False
        self._connected.clear()
        with self._process_lock:
            process = self._process
            self._process = None
            self._cached_chip_id = None
            self._identity_transport_id = None
        if process is not None and process.poll() is None:
            try:
                process.terminate()
                process.wait(timeout=1.0)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    process.kill()
                except OSError:
                    pass
        self._responses.put(None)
        if self._manager is not None and self._manager is not threading.current_thread():
            self._manager.join(timeout=2.0)

    def wait_online(self, timeout: float = 0.0) -> bool:
        return self._connected.wait(timeout)

    def identity(self) -> str:
        """Return identity probed before opening this persistent shell."""
        with self._process_lock:
            transport_id = self.transport_id
            identity_transport_id = self._identity_transport_id
            chip_id = self._cached_chip_id
        if transport_id is None or not self._connected.is_set():
            raise DeviceOfflineError("ADB shell 未连接，无法识别 chip_id")
        if chip_id is None or identity_transport_id != transport_id:
            raise DeviceOfflineError("当前长驻 shell 没有与 transport 匹配的 chip_id")
        return require_chip_id(chip_id)

    def _probe_identity(self, selected: dict[str, Any], timeout: float = 3.0) -> str:
        """Probe identity before `_open`; a second shell would evict curr_service."""
        transport_id = int(selected["transport_id"])
        try:
            completed = subprocess.run(
                [self.adb, "-t", str(transport_id), "shell", "device", "get_device_id"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                timeout=timeout,
                check=False,
                env=adb_environment(),
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise StudioError(f"主动识别 chip_id 失败: {exc}") from exc
        if completed.returncode != 0:
            detail = clean_line(completed.stdout)[:160] or f"exit={completed.returncode}"
            raise StudioError(f"主动识别 chip_id 失败: {detail}")
        return parse_chip_id_output(completed.stdout)

    def exchange(self, command: str, timeout: float = 3.0) -> CommandReply:
        if "\n" in command or "\r" in command or not command.strip():
            raise ValidationError("设备命令格式无效")
        # Most v2hw commands end with the shell's generic ``ok`` line.  RGB
        # calibration is deliberately self-framing instead: a successful
        # status/set/reset emits exactly one complete ``RGB_CAL ...`` record.
        # Treat that record as the terminator so the persistent shell remains
        # compatible with the device protocol without opening a second ADB
        # shell.  ServoProtocol still strictly validates the full record.
        rgb_calibration_command = command.startswith("v2hw rgb calibration ")
        with self._command_lock:
            with self._process_lock:
                process = self._process
            if process is None or process.poll() is not None or process.stdin is None:
                raise DeviceOfflineError("ADB shell 未连接")

            while True:
                try:
                    self._responses.get_nowait()
                except queue.Empty:
                    break

            started = time.monotonic()
            try:
                process.stdin.write(command + "\n")
                process.stdin.flush()
            except (BrokenPipeError, OSError) as exc:
                self._mark_disconnected(str(exc))
                raise DeviceOfflineError("ADB shell 写入失败") from exc

            lines: list[str] = []
            deadline = started + timeout
            failure_code: int | None = None
            while time.monotonic() < deadline:
                try:
                    line = self._responses.get(timeout=min(0.10, max(0.001, deadline - time.monotonic())))
                except queue.Empty:
                    if process.poll() is not None:
                        self._mark_disconnected("ADB shell 已退出")
                        raise DeviceOfflineError("ADB shell 已断开")
                    continue
                if line is None:
                    raise DeviceOfflineError("ADB shell 已断开")
                if not line:
                    continue
                lines.append(line)
                if line == "ok" or (rgb_calibration_command and line.startswith("RGB_CAL ")):
                    return CommandReply(True, tuple(lines), (time.monotonic() - started) * 1000.0)
                match = re.match(r"v2hw command failed:\s*(-?\d+)", line)
                if match:
                    failure_code = int(match.group(1))
                    return CommandReply(False, tuple(lines), (time.monotonic() - started) * 1000.0, failure_code)

            raise StudioError(f"设备命令超时: {command}")

    def _connection_loop(self) -> None:
        while self._running:
            if self.online:
                time.sleep(0.20)
                continue
            try:
                transports = discover_transports(self.adb)
                selected: dict[str, Any] | None = None
                if self.requested_transport_id is not None:
                    selected = next(
                        (item for item in transports if item["transport_id"] == self.requested_transport_id), None
                    )
                    if selected is None:
                        raise DeviceOfflineError(f"transport_id={self.requested_transport_id} 不在线")
                elif len(transports) == 1:
                    selected = transports[0]
                elif not transports:
                    raise DeviceOfflineError("没有在线 ADB 设备")
                else:
                    raise DeviceOfflineError("检测到多块板，请用 --transport 指定")
                chip_id = self._probe_identity(selected)
                self._open(selected, chip_id)
            except (OSError, subprocess.SubprocessError, StudioError) as exc:
                self.last_error = str(exc)
                self._notify_state(False, self.last_error)
                time.sleep(1.0)

    def _open(self, selected: dict[str, Any], chip_id: str) -> None:
        process = subprocess.Popen(
            [self.adb, "-t", str(selected["transport_id"]), "shell"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            bufsize=1,
            env=adb_environment(),
        )
        with self._process_lock:
            self._process = process
            self.transport_id = int(selected["transport_id"])
            self.serial = str(selected["serial"])
            self.usb = str(selected["usb"])
            self._cached_chip_id = require_chip_id(chip_id)
            self._identity_transport_id = self.transport_id
        self.last_error = None
        self._connected.set()
        self._reader = threading.Thread(target=self._reader_loop, args=(process,), name="adb-motion-reader", daemon=True)
        self._reader.start()
        self._notify_state(True, None)

    def _reader_loop(self, process: subprocess.Popen[str]) -> None:
        assert process.stdout is not None
        try:
            for raw in process.stdout:
                line = clean_line(raw)
                if line:
                    if self._line_callback is not None:
                        self._line_callback(line)
                    self._responses.put(line)
        finally:
            with self._process_lock:
                if self._process is process:
                    self._process = None
            self._mark_disconnected("ADB shell 输出已结束")
            self._responses.put(None)

    def _mark_disconnected(self, reason: str) -> None:
        was_connected = self._connected.is_set()
        self._connected.clear()
        with self._process_lock:
            self._cached_chip_id = None
            self._identity_transport_id = None
        self.last_error = reason
        if was_connected:
            self._notify_state(False, reason)

    def _notify_state(self, online: bool, reason: str | None) -> None:
        callback = self._state_callback
        if callback is not None:
            try:
                callback(online, reason)
            except Exception:
                pass


class ServoProtocol:
    """Small typed facade over the board's line protocol."""

    def __init__(self, shell: Any) -> None:
        self.shell = shell

    @property
    def online(self) -> bool:
        return bool(self.shell.online)

    @property
    def transport_id(self) -> int | None:
        return self.shell.transport_id

    @property
    def command_busy(self) -> bool:
        return bool(getattr(self.shell, "command_busy", False))

    def identity(self) -> str:
        identity_reader = getattr(self.shell, "identity", None)
        if not callable(identity_reader):
            raise StudioError("当前 ADB 传输不支持主动识别 chip_id")
        return require_chip_id(identity_reader())

    def power(self) -> tuple[dict[str, Any], CommandReply]:
        reply = self._must("v2hw power", timeout=3.0)
        for line in reply.lines:
            match = POWER_RE.search(line)
            if match:
                millivolt = int(match.group("millivolt"))
                battery = match.group("battery")
                present = millivolt >= 3000 and battery not in {"no-battery", "unknown"}
                return {
                    "millivolt": millivolt,
                    "servo_millivolt": millivolt,
                    "vbat_mv": millivolt,
                    "volt": round(millivolt / 1000.0, 3),
                    "level": match.group("level"),
                    "battery": battery,
                    "battery_label": battery,
                    "status": battery,
                    "present": present,
                    "servo_power_present": present,
                    "ready": present,
                    "checked_at": time.time(),
                }, reply
        raise StudioError("设备未返回可解析的舵机电源状态")

    def imu(self) -> tuple[dict[str, Any], CommandReply]:
        reply = self._must("v2hw mpu", timeout=IMU_COMMAND_TIMEOUT_S)
        return parse_mpu_reply(reply), reply

    def frame(self, pose: dict[str, Any]) -> CommandReply:
        servos = pose["servos_us"]
        led = pose["led"]
        color = led["color"]
        red = int(color[1:3], 16) if led["enabled"] else 0
        green = int(color[3:5], 16) if led["enabled"] else 0
        blue = int(color[5:7], 16) if led["enabled"] else 0
        brightness = led["brightness"] if led["enabled"] else 0
        return self._must(
            f"v2hw motion frame {pose['duration_ms']} {pose['easing']} "
            f"{servos[0]} {servos[1]} {servos[2]} {servos[3]} "
            f"{led['pixel_count']} {red} {green} {blue} {brightness}",
            timeout=3.0,
        )

    def servo_frame(
        self,
        servos_us: Sequence[int],
        duration_ms: int,
        easing: str = "smooth",
    ) -> CommandReply:
        """Send one four-channel frame without taking ownership of the LED."""
        if not isinstance(servos_us, (list, tuple)) or len(servos_us) != SERVO_COUNT:
            raise ValidationError("servos_us 必须恰好包含 4 个整数")
        servos = [
            require_int(
                pulse,
                f"servos_us[{channel}]",
                SERVO_TEST_MIN_US,
                SERVO_TEST_MAX_US,
            )
            for channel, pulse in enumerate(servos_us)
        ]
        duration_ms = require_int(
            duration_ms,
            "duration_ms",
            POSTURE_TEST_MIN_DURATION_MS,
            POSTURE_TEST_MAX_DURATION_MS,
        )
        easing = normalize_easing(easing)
        return self._must(
            f"v2hw servo frame {duration_ms} {easing} "
            f"{servos[0]} {servos[1]} {servos[2]} {servos[3]}",
            timeout=3.0,
        )

    def servo_test(self, channel: int, pulse_us: int, hold_ms: int) -> CommandReply:
        """Jog exactly one channel; firmware must disable it before replying."""
        channel = require_int(channel, "channel", 0, SERVO_COUNT - 1)
        pulse_us = require_int(pulse_us, "pulse_us", SERVO_TEST_MIN_US, SERVO_TEST_MAX_US)
        hold_ms = require_int(
            hold_ms,
            "hold_ms",
            SERVO_TEST_MIN_HOLD_MS,
            SERVO_TEST_MAX_HOLD_MS,
        )
        return self._must(
            f"v2hw servo test {channel} {pulse_us} {hold_ms}",
            timeout=max(2.0, hold_ms / 1000.0 + 1.0),
        )

    def light(self, led: dict[str, Any]) -> CommandReply:
        color = led["color"]
        red = int(color[1:3], 16) if led["enabled"] else 0
        green = int(color[3:5], 16) if led["enabled"] else 0
        blue = int(color[5:7], 16) if led["enabled"] else 0
        brightness = led["brightness"] if led["enabled"] else 0
        return self._must(
            f"v2hw rgb strip {led['pixel_count']} {red} {green} {blue} {brightness}",
            timeout=3.0,
        )

    def led_calibration_status(self) -> tuple[dict[str, Any], CommandReply]:
        reply = self._must("v2hw rgb calibration status", timeout=2.0)
        return parse_led_calibration_reply(reply), reply

    def led_calibration_set(self, calibration: dict[str, Any]) -> tuple[dict[str, Any], CommandReply]:
        order = calibration["color_order"]
        if order not in LED_COLOR_ORDERS:
            raise ValidationError(f"color_order 必须是 {', '.join(LED_COLOR_ORDERS)} 之一")
        red = require_json_int(
            calibration["r_gain_permille"],
            "r_gain_permille",
            LED_GAIN_MIN_PERMILLE,
            LED_GAIN_MAX_PERMILLE,
        )
        green = require_json_int(
            calibration["g_gain_permille"],
            "g_gain_permille",
            LED_GAIN_MIN_PERMILLE,
            LED_GAIN_MAX_PERMILLE,
        )
        blue = require_json_int(
            calibration["b_gain_permille"],
            "b_gain_permille",
            LED_GAIN_MIN_PERMILLE,
            LED_GAIN_MAX_PERMILLE,
        )
        gamma = require_json_int(
            calibration["gamma_x100"],
            "gamma_x100",
            LED_GAMMA_MIN_X100,
            LED_GAMMA_MAX_X100,
        )
        reply = self._must(
            f"v2hw rgb calibration set {order} {red} {green} {blue} {gamma}",
            timeout=3.0,
        )
        return parse_led_calibration_reply(reply), reply

    def led_calibration_reset(self) -> tuple[dict[str, Any], CommandReply]:
        reply = self._must("v2hw rgb calibration reset", timeout=3.0)
        return parse_led_calibration_reply(reply), reply

    def state(self) -> tuple[dict[str, Any] | None, CommandReply]:
        reply = self._must("v2hw servo state", timeout=2.0)
        sections: dict[str, dict[str, str]] = {}
        for line in reply.lines:
            for prefix in ("SERVO_STATE", "SERVO_POSE", "SERVO_TIMING", "SERVO_LEASE"):
                fields = protocol_fields(line, prefix)
                if fields is not None:
                    sections[prefix] = fields
                    break
        if set(sections) != {"SERVO_STATE", "SERVO_POSE", "SERVO_TIMING", "SERVO_LEASE"}:
            # Old firmware printed one overlong line. The shell could insert a
            # NUL and truncate it at arbitrary fields, so partial data must not
            # be presented as a valid state.
            return None, reply
        try:
            state_fields = sections["SERVO_STATE"]
            pose_fields = sections["SERVO_POSE"]
            timing_fields = sections["SERVO_TIMING"]
            lease_fields = sections["SERVO_LEASE"]
            sequence = protocol_int(state_fields, "seq")
            sequences = {
                sequence,
                protocol_int(pose_fields, "seq"),
                protocol_int(timing_fields, "seq"),
                protocol_int(lease_fields, "seq"),
            }
            if len(sequences) != 1 or protocol_int(state_fields, "status") != 0:
                return None, reply
            mode = state_fields["mode"]
            easing = timing_fields["easing"]
            if mode not in {"idle", "frame", "sweep"} or easing not in EASINGS:
                return None, reply
            return {
                "seq": sequence,
                "mode": mode,
                "active": protocol_int(state_fields, "active") == 1,
                "enabled_mask": protocol_int(state_fields, "enabled", 16),
                "current_us": protocol_pose(pose_fields, "current"),
                "target_us": protocol_pose(pose_fields, "target"),
                "duration_ms": protocol_int(timing_fields, "duration_ms"),
                "elapsed_ms": protocol_int(timing_fields, "elapsed_ms"),
                "easing": easing,
                "sweep_channel": protocol_int(timing_fields, "sweep_channel"),
                "watchdog_ms": protocol_int(lease_fields, "watchdog_ms"),
                "remaining_ms": protocol_int(lease_fields, "watchdog_remaining_ms"),
                "expired": protocol_int(lease_fields, "watchdog_expired") == 1,
            }, reply
        except (KeyError, ValidationError):
            return None, reply

    def stop_motion(self) -> CommandReply:
        return self._must("v2hw servo stop", timeout=2.0)

    def all_off(self) -> CommandReply:
        return self._must("v2hw servo all-off", timeout=2.0)

    def watchdog(self, timeout_ms: int) -> CommandReply:
        timeout_ms = require_int(timeout_ms, "watchdog_ms", 0, 5000)
        if timeout_ms not in {0} and timeout_ms < 200:
            raise ValidationError("watchdog_ms 必须为 0 或 200..5000")
        return self._must(f"v2hw servo watchdog {timeout_ms}", timeout=2.0)

    def rgb_release(self) -> CommandReply:
        return self._must("v2hw rgb release", timeout=2.0)

    def safe_off(self) -> CommandReply:
        """Best-effort ordered release; always attempt all three operations."""
        first_error: StudioError | None = None
        last_reply: CommandReply | None = None
        for operation in (self.all_off, lambda: self.watchdog(0), self.rgb_release):
            try:
                last_reply = operation()
            except StudioError as exc:
                if first_error is None:
                    first_error = exc
        if first_error is not None:
            raise first_error
        assert last_reply is not None
        return last_reply

    def _must(self, command: str, timeout: float) -> CommandReply:
        reply = self.shell.exchange(command, timeout=timeout)
        if not reply.ok:
            raise StudioError(f"设备拒绝命令 ({reply.error_code}): {command}")
        return reply


class MotionStudio:
    """Thread-safe motion state machine used by HTTP handlers and tests."""

    def __init__(
        self,
        protocol: ServoProtocol,
        store: ProjectStore | None = None,
        *,
        allow_power_override: bool = False,
        client_grace_s: float = 2.0,
        imu_poll_interval_s: float = IMU_POLL_INTERVAL_S,
    ) -> None:
        self.protocol = protocol
        self.store = store or ProjectStore()
        self.allow_power_override = allow_power_override
        self.client_grace_s = client_grace_s
        interval = float(imu_poll_interval_s)
        if not math.isfinite(interval):
            interval = IMU_POLL_INTERVAL_S
        self.imu_poll_interval_s = min(10.0, max(IMU_MIN_POLL_INTERVAL_S, interval))
        self.imu_fresh_after_s = max(IMU_FRESH_AFTER_S, self.imu_poll_interval_s * 3.0)
        self.lock = threading.RLock()
        # Serial transport locking only prevents interleaved text commands; it
        # does not define which high-level operation wins.  Keep every output-
        # owning transaction behind one lock so an emergency safe_off cannot
        # be followed by a late arm/frame from a request that already passed
        # its state checks.
        self._actuation_op_lock = threading.Lock()
        self.running = False
        self.online = False
        self.error: str | None = None
        self.warnings: list[dict[str, Any]] = []
        self.chip_id: str | None = None
        self.connection_epoch = 0
        self._identity_epoch: int | None = None
        self._observed_chip_id: str | None = None
        self.board = BOARD_NAME
        self.firmware: str | None = None
        self.latency_ms: float | None = None
        self.armed = False
        self._armed_connection_epoch: int | None = None
        self._armed_chip_id: str | None = None
        self.estop = True
        self.power: dict[str, Any] | None = None
        self.command_seq = 0
        self.target_pose = default_pose()
        self.confirmed_pose: dict[str, Any] = {"seq": 0, **default_pose(), "confirmed_at": None}
        self.servo_state: dict[str, Any] | None = None
        self.recording: dict[str, Any] = {
            "state": "idle",
            "started_at": None,
            "duration_ms": 0,
            "frame_count": 0,
        }
        self._record_started_mono = 0.0
        self._record_frames: list[dict[str, Any]] = []
        self.playback: dict[str, Any] = {
            "state": "stopped",
            "action_id": None,
            "position_ms": 0,
            "duration_ms": 0,
            "index": -1,
            "rate": 1.0,
            "loop": False,
            "completed_loops": 0,
        }
        self._play_thread: threading.Thread | None = None
        self._play_stop = threading.Event()
        self._play_pause = threading.Event()
        self._latest_pose: tuple[int, dict[str, Any]] | None = None
        self._pose_event = threading.Event()
        self._pose_thread: threading.Thread | None = None
        self._monitor_thread: threading.Thread | None = None
        self._imu_thread: threading.Thread | None = None
        self._imu_stop = threading.Event()
        self._imu_poll_lock = threading.Lock()
        self._imu_filter = ImuComplementaryFilter()
        self._imu_filter_epoch = 0
        self._imu_sample_seq = 0
        self._imu_last_timestamp_ms = 0
        self._imu_last_attempt_mono: float | None = None
        self._imu_last_success_mono: float | None = None
        self._imu: dict[str, Any] = {
            "available": False,
            "fresh": False,
            "sample_seq": 0,
            "timestamp_ms": None,
            "connection_epoch": 0,
            "filter_epoch": 0,
            "chip_id": None,
            "sample_age_ms": None,
            "raw": None,
            "scaled": None,
            "attitude": None,
            "health": "offline",
            "error": "ADB shell 未连接",
            "source": "v2hw mpu",
            "poll_interval_ms": int(round(self.imu_poll_interval_s * 1000.0)),
            "latency_ms": None,
            "sensor": None,
        }
        self._subscribers: list[queue.Queue[dict[str, Any]]] = []
        self._client_count = 0
        self._last_client_seen = time.monotonic()
        self._connected_once = False
        self._safety_off_pending = True
        self._light_manual_active = False
        self._servo_test_active = False
        self._servo_test_cancel_requested = False

    def start(self) -> None:
        with self.lock:
            if self.running:
                return
            self.running = True
            self._imu_stop.clear()
        shell = getattr(self.protocol, "shell", None)
        if shell is not None and hasattr(shell, "start"):
            shell.start(self.on_connection, self.on_device_line)
        self._pose_thread = threading.Thread(target=self._pose_loop, name="motion-pose", daemon=True)
        self._pose_thread.start()
        self._monitor_thread = threading.Thread(target=self._monitor_loop, name="motion-safety", daemon=True)
        self._monitor_thread.start()
        self._imu_thread = threading.Thread(target=self._imu_loop, name="motion-imu", daemon=True)
        self._imu_thread.start()

    def shutdown(self) -> None:
        with self.lock:
            if not self.running:
                return
            self.running = False
            self.armed = False
            self._armed_connection_epoch = None
            self._armed_chip_id = None
            self.estop = True
            self._latest_pose = None
            self._play_stop.set()
            self._play_pause.clear()
            self._imu_stop.set()
            self._servo_test_cancel_requested = self._servo_test_active
        self._pose_event.set()
        with self._actuation_op_lock:
            try:
                if self.protocol.online:
                    self.protocol.safe_off()
            except StudioError:
                pass
        shell = getattr(self.protocol, "shell", None)
        if shell is not None and hasattr(shell, "stop"):
            shell.stop()
        for thread in (self._pose_thread, self._monitor_thread, self._imu_thread, self._play_thread):
            if thread is not None and thread is not threading.current_thread():
                thread.join(timeout=2.0)
        self.publish()

    def on_connection(self, online: bool, reason: str | None = None) -> None:
        new_online = bool(online)
        with self.lock:
            transitioned = new_online != self.online
            self.online = new_online
            if transitioned:
                # Every transport boundary invalidates identity and filter state;
                # a reconnect may represent a different physical board.
                self.connection_epoch += 1
                self._identity_epoch = None
                self._observed_chip_id = None
                self.chip_id = None
                self.firmware = None
                self._imu_filter_epoch += 1
                self._imu_last_attempt_mono = None
            current_epoch = self.connection_epoch
            self.error = None if new_online else (reason or "ADB 已断开")
            if not new_online:
                self.armed = False
                self._armed_connection_epoch = None
                self._armed_chip_id = None
                self.estop = True
                self._latest_pose = None
                self._safety_off_pending = True
                self._play_stop.set()
                self.playback["state"] = "stopped"
                self.recording["state"] = "idle"
                self._light_manual_active = False
                self._servo_test_cancel_requested = self._servo_test_active
            else:
                self._connected_once = True
            self._imu["available"] = False
            self._imu["fresh"] = False
            self._imu["health"] = "initializing" if new_online else "offline"
            self._imu["error"] = None if new_online else (reason or "ADB shell 已断开")
        if transitioned:
            self._imu_filter.reset()
        self.publish()
        if new_online and transitioned:
            threading.Thread(
                target=self._initialize_connection,
                args=(current_epoch,),
                name="motion-init",
                daemon=True,
            ).start()

    def on_device_line(self, line: str) -> None:
        with self.lock:
            for pattern in CHIP_ID_PATTERNS:
                match = pattern.search(line)
                if match:
                    # Passive output is diagnostic only. Calibration identity is
                    # established exclusively by the transport-bound one-shot.
                    self._observed_chip_id = match.group("id").lower()
            match = FIRMWARE_RE.search(line)
            if match:
                self.firmware = match.group("version")

    def _connection_is_current(self, epoch: int) -> bool:
        with self.lock:
            return bool(self.connection_epoch == epoch and self.online and self.protocol.online)

    def _initialize_connection(self, epoch: int) -> None:
        with self._actuation_op_lock:
            self._initialize_connection_exclusive(epoch)

    def _initialize_connection_exclusive(self, epoch: int) -> None:
        try:
            reply = self.protocol.safe_off()
            if not self._connection_is_current(epoch):
                return
            chip_id = require_chip_id(self.protocol.identity())
            if not self._connection_is_current(epoch):
                return
            power, power_reply = self.protocol.power()
            if not self._connection_is_current(epoch):
                return
            servo_state, state_reply = self.protocol.state()
            with self.lock:
                if self.connection_epoch != epoch or not self.online or not self.protocol.online:
                    return
                self.chip_id = chip_id
                self._identity_epoch = epoch
                self.latency_ms = max(reply.elapsed_ms, power_reply.elapsed_ms, state_reply.elapsed_ms)
                self.power = power
                self.servo_state = servo_state
                self._safety_off_pending = False
                self.error = None
        except StudioError as exc:
            with self.lock:
                if self.connection_epoch == epoch:
                    self.chip_id = None
                    self._identity_epoch = None
                    self.error = str(exc)
        self.publish()

    def _identity_valid_locked(self) -> bool:
        return bool(
            self.online
            and self.protocol.online
            and self.chip_id is not None
            and self._identity_epoch == self.connection_epoch
        )

    def _current_calibration_locked(self) -> dict[str, Any] | None:
        if not self._identity_valid_locked():
            return None
        assert self.chip_id is not None
        record = self.store.get_calibration(self.chip_id)
        if record is None or record["board"] != self.board:
            return None
        return record

    def _motion_profile_locked(self) -> dict[str, Any]:
        """Build execution limits without trusting project-owned servo ranges."""
        profile = default_device_profile()
        project_led = self.store.project.get("device_profile", {}).get("led")
        if isinstance(project_led, dict):
            profile["led"] = copy.deepcopy(project_led)
        record = self._current_calibration_locked()
        if record is None:
            return profile
        for index, calibration in enumerate(record["servo_calibration"]):
            profile["servos"][index].update(
                {
                    "min_us": calibration["min_us"],
                    "center_us": calibration["center_us"],
                    "max_us": calibration["max_us"],
                    "invert": calibration["reverse"],
                    # trim_deg belongs to angle-to-pulse authoring; pulse
                    # commands are already expressed in calibrated microseconds.
                    "trim_us": 0,
                }
            )
        return profile

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            recording = copy.deepcopy(self.recording)
            if recording["state"] == "recording":
                recording["duration_ms"] = int((time.monotonic() - self._record_started_mono) * 1000)
            recording["active"] = recording["state"] == "recording"
            transport_id = self.protocol.transport_id
            profile = self._motion_profile_locked()
            active_calibration = self._current_calibration_locked()
            identity_valid = self._identity_valid_locked()
            return {
                "app": APP_NAME,
                "online": self.online and self.protocol.online,
                "running": self.running,
                "error": self.error,
                "warnings": copy.deepcopy(self.warnings[-20:]),
                "transport_id": transport_id,
                "chip_id": self.chip_id,
                "connection_epoch": self.connection_epoch,
                "identity_epoch": self._identity_epoch,
                "identity_valid": identity_valid,
                "filter_epoch": self._imu_filter_epoch,
                "sample_seq": self._imu_sample_seq,
                "timestamp_ms": self._imu.get("timestamp_ms"),
                "imu_filter_epoch": self._imu_filter_epoch,
                "imu_sample_seq": self._imu_sample_seq,
                "imu_timestamp_ms": self._imu.get("timestamp_ms"),
                "board": self.board,
                "firmware": self.firmware,
                "latency_ms": None if self.latency_ms is None else round(self.latency_ms, 1),
                "armed": self.armed,
                "estop": self.estop,
                "power": copy.deepcopy(self.power),
                "servo_limits": {
                    "min_us": min(item["min_us"] for item in profile["servos"]),
                    "center_us": SERVO_CENTER_US,
                    "max_us": max(item["max_us"] for item in profile["servos"]),
                    "hard_min_us": SERVO_MIN_US,
                    "hard_max_us": SERVO_MAX_US,
                    "frequency_hz": 50,
                    "pads": list(SERVO_PADS),
                    "source": "device_calibration" if active_calibration is not None else "conservative_default",
                    "chip_id": active_calibration["chip_id"] if active_calibration is not None else None,
                    "channels": copy.deepcopy(profile["servos"]),
                },
                "target_pose": copy.deepcopy(self.target_pose),
                "confirmed_pose": copy.deepcopy(self.confirmed_pose),
                "servo_state": copy.deepcopy(self.servo_state),
                "recording": recording,
                "playback": copy.deepcopy(self.playback),
                "project": self.store.snapshot(),
                "command_seq": self.command_seq,
                "client_count": self._client_count,
                "safety_off_pending": self._safety_off_pending,
                "light_manual_active": self._light_manual_active,
                "servo_test_active": self._servo_test_active,
                "power_override_allowed": self.allow_power_override,
            }

    def imu_snapshot(self) -> dict[str, Any]:
        """Return cached telemetry without issuing a device command."""
        with self.lock:
            result = copy.deepcopy(self._imu)
            last_success = self._imu_last_success_mono
            studio_online = self.online
            current_connection_epoch = self.connection_epoch
            current_filter_epoch = self._imu_filter_epoch
            current_chip_id = self.chip_id
            identity_epoch = self._identity_epoch
        device_online = bool(self.protocol.online)
        online = studio_online and device_online
        result["current_connection_epoch"] = current_connection_epoch
        result["current_filter_epoch"] = current_filter_epoch
        result["current_chip_id"] = current_chip_id
        result["identity_valid"] = bool(
            online and current_chip_id is not None and identity_epoch == current_connection_epoch
        )
        if last_success is None:
            age_ms: int | None = None
        else:
            age_ms = max(0, int(round((time.monotonic() - last_success) * 1000.0)))
        result["sample_age_ms"] = age_ms
        if not online:
            result["available"] = False
            result["fresh"] = False
            result["health"] = "offline"
            if not result.get("error"):
                result["error"] = "ADB shell 未连接"
        elif not result.get("available") or result.get("timestamp_ms") is None:
            result["available"] = False
            result["fresh"] = False
            if not result.get("error"):
                result["health"] = "initializing"
        elif result.get("error"):
            result["fresh"] = False
            result["health"] = "error"
        elif age_ms is None or age_ms > int(self.imu_fresh_after_s * 1000.0):
            result["fresh"] = False
            result["health"] = "stale"
            result["error"] = "IMU 数据已过期"
        else:
            result["fresh"] = True
        return result

    def poll_imu_once(self, *, force: bool = False) -> dict[str, Any]:
        """Perform one rate-limited read; failures only degrade IMU telemetry."""
        attempt_mono = time.monotonic()
        with self.lock:
            online = self.online and self.protocol.online
            poll_connection_epoch = self.connection_epoch
            last_attempt = self._imu_last_attempt_mono
            if not force and last_attempt is not None and attempt_mono - last_attempt < self.imu_poll_interval_s:
                return self.imu_snapshot()
            self._imu_last_attempt_mono = attempt_mono
        if not online:
            self._record_imu_failure(
                DeviceOfflineError("ADB shell 未连接"),
                offline=True,
                expected_connection_epoch=poll_connection_epoch,
            )
            return self.imu_snapshot()

        with self._imu_poll_lock:
            try:
                with self.lock:
                    if (
                        self.connection_epoch != poll_connection_epoch
                        or not self.online
                        or not self.protocol.online
                    ):
                        return self.imu_snapshot()
                raw, reply = self.protocol.imu()
                sampled_mono = time.monotonic()
                raw_payload = {
                    "accel": copy.deepcopy(raw["accel"]),
                    "gyro": copy.deepcopy(raw["gyro"]),
                    "temp": int(raw["temp"]),
                }
                scaled = {
                    "accel_g": {
                        axis: round(float(raw_payload["accel"][axis]) / IMU_ACCEL_LSB_PER_G, 6)
                        for axis in ("x", "y", "z")
                    },
                    "gyro_dps": {
                        axis: round(float(raw_payload["gyro"][axis]) / IMU_GYRO_LSB_PER_DPS, 6)
                        for axis in ("x", "y", "z")
                    },
                    "temperature_c": round(float(raw_payload["temp"]) / 340.0 + 36.53, 3),
                }
                with self.lock:
                    # The read ran without the state lock. Discard it before it
                    # can touch the filter if a disconnect/reconnect occurred.
                    if (
                        self.connection_epoch != poll_connection_epoch
                        or not self.online
                        or not self.protocol.online
                    ):
                        return self.imu_snapshot()
                    attitude = self._imu_filter.update(raw, monotonic_s=sampled_mono)
                    if attitude["filter_gap_reset"]:
                        self._imu_filter_epoch += 1
                    self._imu_sample_seq += 1
                    timestamp_ms = max(time.time_ns() // 1_000_000, self._imu_last_timestamp_ms + 1)
                    self._imu_last_timestamp_ms = timestamp_ms
                    sample_chip_id = (
                        self.chip_id if self._identity_epoch == poll_connection_epoch else None
                    )
                    self._imu_last_success_mono = sampled_mono
                    self._imu.update(
                        {
                            "available": True,
                            "fresh": True,
                            "sample_seq": self._imu_sample_seq,
                            "timestamp_ms": timestamp_ms,
                            "connection_epoch": poll_connection_epoch,
                            "filter_epoch": self._imu_filter_epoch,
                            "chip_id": sample_chip_id,
                            "sample_age_ms": 0,
                            "raw": raw_payload,
                            "scaled": scaled,
                            "attitude": attitude,
                            "health": "ok" if attitude["accel_trusted"] else "degraded",
                            "error": None,
                            "latency_ms": round(reply.elapsed_ms, 1),
                            "sensor": {
                                "bus": f"i2c{raw['bus']}",
                                "address": f"0x{raw['address']:02x}",
                                "who_am_i": f"0x{raw['who_am_i']:02x}",
                                "device_status": raw["device_status"],
                            },
                        }
                    )
            except DeviceOfflineError as exc:
                self._record_imu_failure(
                    exc,
                    offline=True,
                    expected_connection_epoch=poll_connection_epoch,
                )
            except (StudioError, OSError, KeyError, TypeError, ValueError) as exc:
                self._record_imu_failure(
                    exc,
                    offline=False,
                    expected_connection_epoch=poll_connection_epoch,
                )
            except Exception as exc:  # keep the telemetry worker alive on an unexpected adapter failure
                self._record_imu_failure(
                    StudioError(f"IMU 采样异常: {exc}"),
                    offline=False,
                    expected_connection_epoch=poll_connection_epoch,
                )
        return self.imu_snapshot()

    def _record_imu_failure(
        self,
        exc: Exception,
        *,
        offline: bool,
        expected_connection_epoch: int | None = None,
    ) -> None:
        message = str(exc).strip() or exc.__class__.__name__
        with self.lock:
            if expected_connection_epoch is not None and self.connection_epoch != expected_connection_epoch:
                return
            self._imu["available"] = False if offline else self._imu["timestamp_ms"] is not None
            self._imu["fresh"] = False
            self._imu["health"] = "offline" if offline else "error"
            self._imu["error"] = message[:300]
            if offline:
                self._imu_filter_epoch += 1
                self._imu_filter.reset()

    def subscribe(self) -> queue.Queue[dict[str, Any]]:
        channel: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=2)
        with self.lock:
            self._subscribers.append(channel)
            self._client_count += 1
            self._last_client_seen = time.monotonic()
        channel.put_nowait(self.snapshot())
        self.publish()
        return channel

    def unsubscribe(self, channel: queue.Queue[dict[str, Any]]) -> None:
        with self.lock:
            if channel in self._subscribers:
                self._subscribers.remove(channel)
                self._client_count = max(0, self._client_count - 1)
            self._last_client_seen = time.monotonic()
        self.publish()

    def publish(self) -> None:
        snapshot = self.snapshot()
        with self.lock:
            subscribers = list(self._subscribers)
        for channel in subscribers:
            try:
                if channel.full():
                    channel.get_nowait()
                channel.put_nowait(snapshot)
            except queue.Empty:
                pass

    def arm(self, *, override_power: bool = False) -> dict[str, Any]:
        if not self._actuation_op_lock.acquire(blocking=False):
            raise StudioError("已有设备输出操作正在执行，暂不能解锁")
        try:
            return self._arm_exclusive(override_power=override_power)
        finally:
            self._actuation_op_lock.release()

    def _arm_exclusive(self, *, override_power: bool = False) -> dict[str, Any]:
        with self.lock:
            if not (self.online and self.protocol.online):
                raise DeviceOfflineError("设备未在线")
            if not self._identity_valid_locked():
                raise StudioError("正在识别当前设备 chip_id，请稍后再解锁")
            if self._servo_test_active:
                raise StudioError("舵机安全测试尚未结束，禁止解锁")
            arm_epoch = self.connection_epoch
            arm_chip_id = self.chip_id
            if self.armed:
                return self.snapshot()
        power, reply = self.protocol.power()
        if not power["ready"] and not (override_power and self.allow_power_override):
            with self.lock:
                self.power = power
                self.error = "舵机电源预检未通过，禁止解锁"
            self.publish()
            raise StudioError(self.error)
        if not power["ready"]:
            with self.lock:
                self.warnings.append(
                    {
                        "time": utc_now(),
                        "code": "external-power-override",
                        "message": "VBAT/servo 预检未通过；用户已明确确认外部舵机电源",
                    }
                )
        self.protocol.safe_off()
        watchdog_reply = self.protocol.watchdog(SERVO_LEASE_MS)
        with self.lock:
            identity_still_valid = bool(
                self._identity_valid_locked()
                and self.connection_epoch == arm_epoch
                and self.chip_id == arm_chip_id
            )
            if not identity_still_valid:
                self.armed = False
                self._armed_connection_epoch = None
                self._armed_chip_id = None
                self.estop = True
                self._safety_off_pending = True
            else:
                self.power = power
                self.latency_ms = max(reply.elapsed_ms, watchdog_reply.elapsed_ms)
                self.armed = True
                self._armed_connection_epoch = arm_epoch
                self._armed_chip_id = arm_chip_id
                self.estop = False
                self.error = None
                self._last_client_seen = time.monotonic()
        if not identity_still_valid:
            try:
                if self.protocol.online:
                    self.protocol.safe_off()
            except StudioError:
                pass
            raise DeviceOfflineError("解锁期间设备连接或 chip_id 已变更")
        self.publish()
        return self.snapshot()

    def disarm(self, reason: str = "user") -> dict[str, Any]:
        # Cancellation is signalled before waiting for the operation lock, so
        # a bounded posture transition wakes within its 20 ms polling cadence.
        with self.lock:
            self._servo_test_cancel_requested = self._servo_test_active
        with self._actuation_op_lock:
            return self._disarm_exclusive(reason)

    def _disarm_exclusive(self, reason: str = "user") -> dict[str, Any]:
        with self.lock:
            self.armed = False
            self._armed_connection_epoch = None
            self._armed_chip_id = None
            self.estop = reason in {"estop", "disconnect", "client-timeout", "command-failed"}
            self._latest_pose = None
            self._play_stop.set()
            self._servo_test_cancel_requested = self._servo_test_active
            self.playback["state"] = "stopped"
            if self.recording["state"] == "recording":
                duration_ms = int((time.monotonic() - self._record_started_mono) * 1000)
                self.recording = {
                    "state": "idle",
                    "active": False,
                    "started_at": None,
                    "duration_ms": max(0, duration_ms),
                    "frame_count": len(self._record_frames),
                    "interrupted": True,
                    "interrupt_reason": reason,
                }
                # An emergency or output lock must never silently create an
                # action. The operator can start a named recording again once
                # the hardware has been made safe.
                self._record_frames = []
            # Clear ownership before issuing the device command so a failed
            # release is reported once rather than retried every monitor tick.
            self._light_manual_active = False
            # Publish/return must not claim a confirmed safe state while the
            # ordered board-side release is still in flight.
            self._safety_off_pending = True
        self._pose_event.set()
        try:
            if self.protocol.online:
                reply = self.protocol.safe_off()
                servo_state, state_reply = self.protocol.state()
                if servo_state is None:
                    raise StudioError("设备已接受断开命令，但未返回完整舵机安全状态")
                outputs_safe = bool(
                    servo_state.get("enabled_mask") == 0
                    and not servo_state.get("active", False)
                    and servo_state.get("watchdog_ms") == 0
                )
                with self.lock:
                    # A complete hardware snapshot is more trustworthy than
                    # synthesizing zeros from the requested operation.
                    self.servo_state = copy.deepcopy(servo_state)
                    self.latency_ms = max(reply.elapsed_ms, state_reply.elapsed_ms)
                if not outputs_safe:
                    raise StudioError("设备断开后仍报告舵机输出或看门狗处于启用状态")
                with self.lock:
                    self._safety_off_pending = False
            else:
                with self.lock:
                    self._safety_off_pending = True
        except StudioError as exc:
            with self.lock:
                self.error = str(exc)
                self._safety_off_pending = True
        self.publish()
        return self.snapshot()

    def emergency_stop(self) -> dict[str, Any]:
        return self.disarm("estop")

    def servo_test(self, payload: Any) -> dict[str, Any]:
        if not self._actuation_op_lock.acquire(blocking=False):
            raise StudioError("已有设备输出操作正在执行")
        try:
            return self._servo_test_exclusive(payload)
        finally:
            self._actuation_op_lock.release()

    def _servo_test_exclusive(self, payload: Any) -> dict[str, Any]:
        """Run one bounded channel jog while the normal motion path stays locked."""
        failure: StudioError | None = None
        result: dict[str, Any] | None = None
        output_may_have_changed = False
        expected_epoch = 0
        expected_chip_id: str | None = None
        expected_board = ""
        sequence = 0
        with self.lock:
            if not (self.online and self.protocol.online):
                raise DeviceOfflineError("设备未在线")
            if not self._identity_valid_locked():
                raise StudioError("正在识别当前设备 chip_id，禁止点动")
            request = normalize_servo_test_request(payload, self._motion_profile_locked())
            if request["connection_epoch"] != self.connection_epoch:
                raise ValidationError("舵机点动 connection_epoch 已过期，请重新读取当前设备")
            if request["chip_id"] != self.chip_id:
                raise ValidationError("舵机点动 chip_id 与当前设备不匹配")
            if request["board"] != self.board:
                raise ValidationError("舵机点动 board 与当前设备不匹配")
            if self.armed:
                raise StudioError("舵机点动要求上位机保持锁定，请先关闭输出")
            if self.playback["state"] != "stopped":
                raise StudioError("动作播放未停止，禁止舵机点动")
            if self.recording["state"] != "idle":
                raise StudioError("动作录制未停止，禁止舵机点动")
            if self._servo_test_active:
                raise StudioError("已有单通道舵机点动正在执行")

            expected_epoch = self.connection_epoch
            expected_chip_id = self.chip_id
            expected_board = self.board
            self._servo_test_active = True
            self._servo_test_cancel_requested = False
            self.error = None
        self.publish()

        def same_identity_locked() -> bool:
            return bool(
                self._identity_valid_locked()
                and self.connection_epoch == expected_epoch
                and self.chip_id == expected_chip_id
                and self.board == expected_board
            )

        try:
            # Do not hold MotionStudio.lock while exchanging lines. The ADB
            # reader delivers device lines through on_device_line() before it
            # queues the command reply, and that callback also needs the lock.
            power, power_reply = self.protocol.power()
            with self.lock:
                if not same_identity_locked():
                    raise DeviceOfflineError("舵机点动电源预检期间设备连接或 chip_id 已变更")
                self.power = copy.deepcopy(power)
                self.latency_ms = power_reply.elapsed_ms
                if not power.get("ready", False):
                    raise StudioError("舵机点动电源预检未通过，禁止输出")
                if self._servo_test_cancel_requested:
                    raise StudioError("舵机点动已由安全锁定取消")
                if self.armed:
                    raise StudioError("舵机点动要求上位机保持锁定，请先关闭输出")
                if self.playback["state"] != "stopped" or self.recording["state"] != "idle":
                    raise StudioError("舵机点动准备期间动作状态已变化")
                self._safety_off_pending = True
                self.command_seq += 1
                sequence = self.command_seq
            self.publish()

            output_may_have_changed = True
            jog_reply = self.protocol.servo_test(
                request["channel"],
                request["pulse_us"],
                request["hold_ms"],
            )
            servo_state, state_reply = self.protocol.state()
            if servo_state is None:
                raise StudioError("舵机点动后设备未返回完整舵机状态")
            with self.lock:
                if not same_identity_locked():
                    raise DeviceOfflineError("舵机点动期间设备连接或 chip_id 已变更")
                self.servo_state = copy.deepcopy(servo_state)
                self.latency_ms = max(power_reply.elapsed_ms, jog_reply.elapsed_ms, state_reply.elapsed_ms)
                if self._servo_test_cancel_requested:
                    raise StudioError("舵机点动已由安全锁定取消")
                outputs_safe = bool(
                    servo_state.get("enabled_mask") == 0
                    and not servo_state.get("active", False)
                )
                if not outputs_safe:
                    raise StudioError("舵机点动结束后指定通道未自动关闭")
                self._safety_off_pending = False
                self.error = None
                self._last_client_seen = time.monotonic()
                result = {
                    "accepted": True,
                    "seq": sequence,
                    "channel": request["channel"],
                    "pad": SERVO_PADS[request["channel"]],
                    "pulse_us": request["pulse_us"],
                    "hold_ms": request["hold_ms"],
                    "safe_min_us": request["safe_min_us"],
                    "safe_max_us": request["safe_max_us"],
                    "outputs_safe": True,
                    "servo_state": copy.deepcopy(servo_state),
                }
        except Exception as exc:
            primary = exc if isinstance(exc, StudioError) else StudioError(
                f"舵机点动失败: {str(exc).strip() or exc.__class__.__name__}"
            )
            cleanup_error: str | None = None
            cleanup_safe = False
            if output_may_have_changed:
                recovered_state: dict[str, Any] | None = None
                off_reply: CommandReply | None = None
                recovered_reply: CommandReply | None = None
                try:
                    if not self.protocol.online:
                        raise DeviceOfflineError("设备已离线，无法确认安全断开")
                    off_reply = self.protocol.safe_off()
                    recovered_state, recovered_reply = self.protocol.state()
                    if recovered_state is None:
                        raise StudioError("安全断开后设备未返回完整舵机状态")
                    with self.lock:
                        if not same_identity_locked():
                            raise DeviceOfflineError("安全断开期间设备连接或 chip_id 已变更")
                    cleanup_safe = bool(
                        recovered_state.get("enabled_mask") == 0
                        and not recovered_state.get("active", False)
                    )
                    if not cleanup_safe:
                        raise StudioError("安全断开后设备仍报告舵机输出启用")
                except Exception as cleanup_exc:
                    cleanup_error = str(cleanup_exc).strip() or cleanup_exc.__class__.__name__
                with self.lock:
                    if recovered_state is not None and same_identity_locked():
                        self.servo_state = copy.deepcopy(recovered_state)
                        if off_reply is not None and recovered_reply is not None:
                            self.latency_ms = max(off_reply.elapsed_ms, recovered_reply.elapsed_ms)
                    if cleanup_safe:
                        released_led = default_led()
                        self.target_pose["led"] = copy.deepcopy(released_led)
                        self.confirmed_pose["led"] = copy.deepcopy(released_led)
                        self.confirmed_pose["seq"] = sequence
                        self.confirmed_pose["confirmed_at"] = time.time()
                        self._light_manual_active = False
                    self._safety_off_pending = not cleanup_safe
            message = str(primary)
            if output_may_have_changed:
                if cleanup_safe:
                    message += "；已执行全路安全断开并确认输出关闭"
                elif cleanup_error:
                    message += f"；安全断开仍未确认: {cleanup_error}"
            with self.lock:
                self.error = message
            failure = (
                DeviceOfflineError(message)
                if isinstance(primary, DeviceOfflineError)
                else StudioError(message)
            )
        finally:
            with self.lock:
                self._servo_test_active = False
                self._servo_test_cancel_requested = False
        self.publish()
        if failure is not None:
            raise failure
        assert result is not None
        return result

    def posture_test(self, payload: Any) -> dict[str, Any]:
        if not self._actuation_op_lock.acquire(blocking=False):
            raise StudioError("已有设备输出操作正在执行")
        try:
            return self._posture_test_exclusive(payload)
        finally:
            self._actuation_op_lock.release()

    def _posture_test_exclusive(self, payload: Any) -> dict[str, Any]:
        """Move all four channels together while keeping the studio locked.

        This is intentionally synchronous and server-owned.  A browser must
        not implement the safety boundary as arm -> async pose -> timer ->
        estop: closing the page would strand that sequence halfway through.
        Here the board watchdog is enabled before the one frame, and every
        exit path performs and verifies a complete hardware release.
        """
        failure: StudioError | None = None
        result: dict[str, Any] | None = None
        output_may_have_changed = False
        expected_epoch = 0
        expected_chip_id: str | None = None
        expected_board = ""
        sequence = 0

        with self.lock:
            if not (self.online and self.protocol.online):
                raise DeviceOfflineError("设备未在线")
            if not self._identity_valid_locked():
                raise StudioError("正在识别当前设备 chip_id，禁止四腿同步姿态测试")
            request = normalize_posture_test_request(payload, self._motion_profile_locked())
            if request["connection_epoch"] != self.connection_epoch:
                raise ValidationError("同步姿态 connection_epoch 已过期，请重新读取当前设备")
            if request["chip_id"] != self.chip_id:
                raise ValidationError("同步姿态 chip_id 与当前设备不匹配")
            if request["board"] != self.board:
                raise ValidationError("同步姿态 board 与当前设备不匹配")
            if self.armed:
                raise StudioError("四腿同步姿态要求上位机保持锁定，请先关闭输出")
            if self.playback["state"] != "stopped":
                raise StudioError("动作播放未停止，禁止四腿同步姿态测试")
            if self.recording["state"] != "idle":
                raise StudioError("动作录制未停止，禁止四腿同步姿态测试")
            if self._servo_test_active:
                raise StudioError("已有舵机安全测试正在执行")
            if self._safety_off_pending:
                raise StudioError("上一次输出安全断开尚未确认，禁止四腿同步姿态")

            expected_epoch = self.connection_epoch
            expected_chip_id = self.chip_id
            expected_board = self.board
            self._servo_test_active = True
            self._servo_test_cancel_requested = False
            self.error = None
        self.publish()

        def same_identity_locked() -> bool:
            return bool(
                self._identity_valid_locked()
                and self.connection_epoch == expected_epoch
                and self.chip_id == expected_chip_id
                and self.board == expected_board
            )

        try:
            power, power_reply = self.protocol.power()
            with self.lock:
                if not same_identity_locked():
                    raise DeviceOfflineError("同步姿态电源预检期间设备连接或 chip_id 已变更")
                self.power = copy.deepcopy(power)
                self.latency_ms = power_reply.elapsed_ms
                if not power.get("ready", False):
                    raise StudioError("四腿同步姿态电源预检未通过，禁止输出")
                if self._servo_test_cancel_requested:
                    raise StudioError("四腿同步姿态已由安全锁定取消")
                if self.armed:
                    raise StudioError("四腿同步姿态要求上位机保持锁定")
                if self.playback["state"] != "stopped" or self.recording["state"] != "idle":
                    raise StudioError("同步姿态准备期间动作状态已变化")
                self._safety_off_pending = True
                self.command_seq += 1
                sequence = self.command_seq
            self.publish()

            # Establish a known all-off baseline before granting a short board
            # lease.  The lease outlives the requested transition by only a
            # narrow margin and remains the last resort if this process dies.
            # Once an output-side operation starts, every failure path must
            # perform and verify cleanup.  Even a host timeout can mean the
            # preceding command reached the board.
            output_may_have_changed = True
            preclear_reply = self.protocol.safe_off()
            with self.lock:
                if not same_identity_locked():
                    raise DeviceOfflineError("同步姿态预清除期间设备连接或 chip_id 已变更")
                if self._servo_test_cancel_requested:
                    raise StudioError("四腿同步姿态已由急停取消")
            if not self.protocol.online:
                raise DeviceOfflineError("同步姿态预清除后设备已离线")
            preclear_state, preclear_state_reply = self.protocol.state()
            if preclear_state is None:
                raise StudioError("四腿同步姿态预清除后未返回完整舵机状态")
            preclear_safe = bool(
                preclear_state.get("enabled_mask") == 0
                and not preclear_state.get("active", False)
                and preclear_state.get("watchdog_ms") == 0
            )
            with self.lock:
                if not same_identity_locked():
                    raise DeviceOfflineError("同步姿态预清除复读期间设备身份已变更")
                self.servo_state = copy.deepcopy(preclear_state)
                if self._servo_test_cancel_requested:
                    raise StudioError("四腿同步姿态已由急停取消")
            if not preclear_safe:
                raise StudioError("四腿同步姿态预清除后仍有 PWM 或看门狗未关闭")
            watchdog_ms = min(
                5_000,
                max(SERVO_LEASE_MS, request["duration_ms"] + POSTURE_TEST_WATCHDOG_MARGIN_MS),
            )
            # Mark cleanup mandatory before the command: an ACK timeout does
            # not prove that the board failed to enable the lease.
            output_may_have_changed = True
            watchdog_reply = self.protocol.watchdog(watchdog_ms)
            with self.lock:
                if not same_identity_locked():
                    raise DeviceOfflineError("同步姿态看门狗设置期间设备连接或 chip_id 已变更")
                if self._servo_test_cancel_requested:
                    raise StudioError("四腿同步姿态已由急停取消")
            if not self.protocol.online:
                raise DeviceOfflineError("同步姿态看门狗设置后设备已离线")
            pose = {
                "servos_us": list(request["servos_us"]),
                "duration_ms": request["duration_ms"],
                "easing": "smooth",
                # safe_off releases the RGB owner as part of the same safety
                # transaction, so the confirmed logical pose must also report
                # the strip as off rather than retain a stale illuminated LED.
                "led": default_led(),
                "source": "posture-test",
            }
            frame_reply = self.protocol.servo_frame(
                request["servos_us"], request["duration_ms"], "smooth"
            )

            transition_deadline = time.monotonic() + request["duration_ms"] / 1000.0
            while True:
                with self.lock:
                    if not same_identity_locked():
                        raise DeviceOfflineError("四腿同步运动期间设备连接或 chip_id 已变更")
                    if self._servo_test_cancel_requested:
                        raise StudioError("四腿同步姿态已由急停取消")
                if not self.protocol.online:
                    raise DeviceOfflineError("四腿同步运动期间设备已离线")
                remaining = transition_deadline - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(0.02, remaining))

            # Host elapsed time is not proof that the board reached the pose.
            # Require interpolation to finish and all four exact PWM values to
            # match before any success path is allowed to release the outputs.
            completion_deadline = (
                time.monotonic() + POSTURE_TEST_COMPLETION_GRACE_MS / 1000.0
            )
            completion_state: dict[str, Any] | None = None
            completion_reply: CommandReply | None = None
            while True:
                completion_state, completion_reply = self.protocol.state()
                if completion_state is None:
                    raise StudioError("四腿同步运动未返回完整到位状态")
                with self.lock:
                    if not same_identity_locked():
                        raise DeviceOfflineError("四腿同步到位复读期间设备身份已变更")
                    self.servo_state = copy.deepcopy(completion_state)
                    if self._servo_test_cancel_requested:
                        raise StudioError("四腿同步姿态已由急停取消")
                if not self.protocol.online:
                    raise DeviceOfflineError("四腿同步到位复读期间设备已离线")
                reached = bool(
                    not completion_state.get("active", False)
                    and completion_state.get("current_us") == request["servos_us"]
                    and completion_state.get("target_us") == request["servos_us"]
                )
                if reached:
                    break
                if not completion_state.get("active", False):
                    raise StudioError("四腿同步插值已停止，但四路未到达请求目标")
                if time.monotonic() >= completion_deadline:
                    raise StudioError("四腿同步插值超时，未确认四路到位")
                time.sleep(POSTURE_TEST_STATE_POLL_MS / 1000.0)

            off_reply = self.protocol.safe_off()
            servo_state, state_reply = self.protocol.state()
            if servo_state is None:
                raise StudioError("四腿同步姿态结束后设备未返回完整舵机状态")
            with self.lock:
                if not same_identity_locked():
                    raise DeviceOfflineError("同步姿态安全断开期间设备连接或 chip_id 已变更")
                self.servo_state = copy.deepcopy(servo_state)
                self.latency_ms = max(
                    power_reply.elapsed_ms,
                    preclear_reply.elapsed_ms,
                    preclear_state_reply.elapsed_ms,
                    watchdog_reply.elapsed_ms,
                    frame_reply.elapsed_ms,
                    completion_reply.elapsed_ms if completion_reply is not None else 0.0,
                    off_reply.elapsed_ms,
                    state_reply.elapsed_ms,
                )
                outputs_safe = bool(
                    servo_state.get("enabled_mask") == 0
                    and not servo_state.get("active", False)
                    and servo_state.get("watchdog_ms") == 0
                )
                if not outputs_safe:
                    raise StudioError("四腿同步姿态结束后未确认所有 PWM 与看门狗已关闭")
                if servo_state.get("current_us") != request["servos_us"]:
                    raise StudioError("四腿同步姿态断开后的最终脉宽与请求目标不一致")
                if self._servo_test_cancel_requested:
                    raise StudioError("四腿同步姿态已由急停取消")
                confirmed = copy.deepcopy(pose)
                confirmed.update({"seq": sequence, "confirmed_at": time.time()})
                self.target_pose = copy.deepcopy(pose)
                self.confirmed_pose = confirmed
                self._light_manual_active = False
                self._safety_off_pending = False
                self.error = None
                self._last_client_seen = time.monotonic()
                result = {
                    "accepted": True,
                    "seq": sequence,
                    "servos_us": list(request["servos_us"]),
                    "duration_ms": request["duration_ms"],
                    "easing": "smooth",
                    "safe_ranges": copy.deepcopy(request["safe_ranges"]),
                    "target_reached": True,
                    "outputs_safe": True,
                    "completion_state": copy.deepcopy(completion_state),
                    "servo_state": copy.deepcopy(servo_state),
                }
        except Exception as exc:
            primary = exc if isinstance(exc, StudioError) else StudioError(
                f"四腿同步姿态失败: {str(exc).strip() or exc.__class__.__name__}"
            )
            cleanup_error: str | None = None
            cleanup_safe = False
            if output_may_have_changed:
                recovered_state: dict[str, Any] | None = None
                off_reply: CommandReply | None = None
                recovered_reply: CommandReply | None = None
                try:
                    if not self.protocol.online:
                        raise DeviceOfflineError("设备已离线，无法确认安全断开")
                    off_reply = self.protocol.safe_off()
                    recovered_state, recovered_reply = self.protocol.state()
                    if recovered_state is None:
                        raise StudioError("安全断开后设备未返回完整舵机状态")
                    with self.lock:
                        if not same_identity_locked():
                            raise DeviceOfflineError("安全断开期间设备连接或 chip_id 已变更")
                    cleanup_safe = bool(
                        recovered_state.get("enabled_mask") == 0
                        and not recovered_state.get("active", False)
                        and recovered_state.get("watchdog_ms") == 0
                    )
                    if not cleanup_safe:
                        raise StudioError("安全断开后设备仍报告舵机输出或看门狗启用")
                except Exception as cleanup_exc:
                    cleanup_error = str(cleanup_exc).strip() or cleanup_exc.__class__.__name__
                with self.lock:
                    if recovered_state is not None and same_identity_locked():
                        self.servo_state = copy.deepcopy(recovered_state)
                        if off_reply is not None and recovered_reply is not None:
                            self.latency_ms = max(off_reply.elapsed_ms, recovered_reply.elapsed_ms)
                    if cleanup_safe:
                        released_led = default_led()
                        self.target_pose["led"] = copy.deepcopy(released_led)
                        self.confirmed_pose["led"] = copy.deepcopy(released_led)
                        self.confirmed_pose["seq"] = sequence
                        self.confirmed_pose["confirmed_at"] = time.time()
                        self._light_manual_active = False
                    self._safety_off_pending = not cleanup_safe
            message = str(primary)
            if output_may_have_changed:
                if cleanup_safe:
                    message += "；已执行全路安全断开并确认输出关闭"
                elif cleanup_error:
                    message += f"；安全断开仍未确认: {cleanup_error}"
            with self.lock:
                self.error = message
            failure = (
                DeviceOfflineError(message)
                if isinstance(primary, DeviceOfflineError)
                else StudioError(message)
            )
        finally:
            with self.lock:
                self._servo_test_active = False
                self._servo_test_cancel_requested = False
        self.publish()
        if failure is not None:
            raise failure
        assert result is not None
        return result

    def submit_pose(self, payload: Any) -> dict[str, Any]:
        with self.lock:
            if not self.armed:
                raise StudioError("舵机已锁定，先完成电源预检并解锁")
            if (
                not self._identity_valid_locked()
                or self._armed_connection_epoch != self.connection_epoch
                or self._armed_chip_id != self.chip_id
            ):
                raise StudioError("当前设备身份已变更，请重新解锁")
            if self.playback["state"] in {"playing", "paused"}:
                raise StudioError("动作正在播放，先停止回放再手动调节")
            pose = normalize_pose(payload, self._motion_profile_locked())
            self.command_seq += 1
            sequence = self.command_seq
            pose["source"] = str(payload.get("source", "manual"))[:32] if isinstance(payload, dict) else "manual"
            self.target_pose = copy.deepcopy(pose)
            self._latest_pose = (sequence, pose)
        self._pose_event.set()
        self.publish()
        return {"accepted": True, "seq": sequence, "pose": copy.deepcopy(pose)}

    def set_light(self, payload: Any) -> dict[str, Any]:
        if not self._actuation_op_lock.acquire(blocking=False):
            raise StudioError("已有设备输出操作正在执行，暂不能更新灯板")
        try:
            return self._set_light_exclusive(payload)
        finally:
            self._actuation_op_lock.release()

    def _set_light_exclusive(self, payload: Any) -> dict[str, Any]:
        with self.lock:
            if not (self.online and self.protocol.online):
                raise DeviceOfflineError("设备未在线")
            led_profile = self.store.project["device_profile"].get("led", {})
            default_count = int(led_profile.get("pixel_count", LED_DEFAULT_COUNT))
            led = normalize_led(payload, default_count)
            self.command_seq += 1
            sequence = self.command_seq
        reply = self.protocol.light(led)
        # Independent lighting is intentionally available while servos remain
        # locked. Give it the same board-side disconnect lease: watchdog expiry
        # turns the strip off without ever enabling a servo channel.
        with self.lock:
            servo_armed = self.armed
        watchdog_reply = None if servo_armed else self.protocol.watchdog(SERVO_LEASE_MS)
        with self.lock:
            self.target_pose["led"] = copy.deepcopy(led)
            self.confirmed_pose["led"] = copy.deepcopy(led)
            self.confirmed_pose["seq"] = sequence
            self.confirmed_pose["confirmed_at"] = time.time()
            self.latency_ms = max(reply.elapsed_ms, watchdog_reply.elapsed_ms if watchdog_reply is not None else 0.0)
            self.error = None
            self._light_manual_active = True
            self._last_client_seen = time.monotonic()
            if self.recording["state"] == "recording":
                self._append_record_frame_locked(self.confirmed_pose)
        self.publish()
        return {"accepted": True, "seq": sequence, "led": copy.deepcopy(led)}

    def record_start(self) -> dict[str, Any]:
        with self.lock:
            if not self.armed:
                raise StudioError("录制前必须先解锁舵机")
            if self.playback["state"] != "stopped":
                raise StudioError("播放期间不能录制")
            if self.recording["state"] == "recording":
                raise StudioError("已在录制")
            self._record_started_mono = time.monotonic()
            self._record_frames = []
            self.recording = {"state": "recording", "started_at": time.time(), "duration_ms": 0, "frame_count": 0}
            self._append_record_frame_locked(self.confirmed_pose)
        self.publish()
        return copy.deepcopy(self.recording)

    def record_stop(self, name: str = "新动作", action_id: str | None = None) -> dict[str, Any]:
        with self.lock:
            if self.recording["state"] != "recording":
                raise StudioError("当前没有在录制")
            frames = copy.deepcopy(self._record_frames)
            if not frames:
                self._append_record_frame_locked(self.confirmed_pose)
                frames = copy.deepcopy(self._record_frames)
            self.recording = {
                "state": "idle",
                "started_at": None,
                "duration_ms": frames[-1]["time_ms"] if frames else 0,
                "frame_count": len(frames),
            }
            if action_id is not None:
                current = self.store.get_action(require_identifier(action_id, "action_id"))
                current["name"] = require_name(name or current["name"], "action.name")
                current["keyframes"] = frames
                current["description"] = current.get("description") or "由实时控制录制"
                action = self.store.update_action(action_id, current, self._motion_profile_locked())
            else:
                action_payload = {
                    "name": require_name(name, "action.name"),
                    "description": "由实时控制录制",
                    "default_rate": 1.0,
                    "end_behavior": "hold",
                    "keyframes": frames,
                }
                action = self.store.create_action(action_payload, self._motion_profile_locked())
        self.publish()
        return action

    def create_action(self, payload: Any) -> dict[str, Any]:
        with self.lock:
            action = self.store.create_action(payload, self._motion_profile_locked())
        self.publish()
        return action

    def update_action(self, action_id: str, payload: Any) -> dict[str, Any]:
        with self.lock:
            action = self.store.update_action(action_id, payload, self._motion_profile_locked())
        self.publish()
        return action

    def delete_action(self, action_id: str) -> None:
        with self.lock:
            if self.playback["action_id"] == action_id and self.playback["state"] != "stopped":
                raise StudioError("不能删除正在播放的动作")
            self.store.delete_action(action_id)
        self.publish()

    def replace_project(self, payload: Any) -> dict[str, Any]:
        with self.lock:
            if self.armed or self.playback["state"] != "stopped":
                raise StudioError("导入项目前必须锁定舵机并停止播放")
            project = self.store.replace(payload)
        self.publish()
        return project

    def _led_calibration_context_locked(self) -> dict[str, Any]:
        online = bool(self.online and self.protocol.online)
        identity_valid = bool(
            online
            and self.chip_id is not None
            and self._identity_epoch == self.connection_epoch
        )
        if not online:
            reason = "device_offline"
        elif not identity_valid:
            reason = "chip_id_unavailable"
        else:
            reason = "query_required"
        return {
            "available": False,
            "reason": reason,
            "connection_epoch": self.connection_epoch,
            "chip_id": self.chip_id,
            "board": self.board,
            "identity_valid": identity_valid,
            "calibration": None,
        }

    def led_calibration_snapshot(self) -> dict[str, Any]:
        """Read persistent RGB calibration without mutating the main state."""
        with self.lock:
            result = self._led_calibration_context_locked()
            if not result["identity_valid"]:
                return result
            expected_epoch = self.connection_epoch
            expected_chip_id = self.chip_id
        status_reader = getattr(self.protocol, "led_calibration_status", None)
        if not callable(status_reader):
            result["reason"] = "unsupported"
            result["error"] = "当前固件不支持灯板校色"
            return result
        try:
            calibration, reply = status_reader()
        except DeviceOfflineError as exc:
            with self.lock:
                result = self._led_calibration_context_locked()
            result["reason"] = "device_offline"
            result["error"] = str(exc)[:300]
            return result
        except ValidationError as exc:
            result["reason"] = "invalid_response"
            result["error"] = str(exc)[:300]
            return result
        except StudioError as exc:
            # Old firmware rejects the new sub-command.  Capability discovery
            # must degrade locally instead of poisoning Motion Studio state.
            result["reason"] = "unsupported"
            result["error"] = str(exc)[:300]
            return result
        with self.lock:
            result = self._led_calibration_context_locked()
            if not (
                result["identity_valid"]
                and self.connection_epoch == expected_epoch
                and self.chip_id == expected_chip_id
            ):
                result["reason"] = "identity_changed"
                return result
            self.latency_ms = reply.elapsed_ms
        result.update(
            {
                "available": True,
                "reason": "available",
                "calibration": copy.deepcopy(calibration),
            }
        )
        return result

    def save_led_calibration(self, payload: Any) -> dict[str, Any]:
        if not self._actuation_op_lock.acquire(blocking=False):
            raise StudioError("已有设备输出操作正在执行，暂不能保存灯板校色")
        try:
            return self._save_led_calibration_exclusive(payload)
        finally:
            self._actuation_op_lock.release()

    def _save_led_calibration_exclusive(self, payload: Any) -> dict[str, Any]:
        request = normalize_led_calibration_request(payload)
        with self.lock:
            if not (self.online and self.protocol.online):
                raise DeviceOfflineError("设备离线，不能保存灯板校色")
            if request["connection_epoch"] != self.connection_epoch:
                raise ValidationError("灯板校色 connection_epoch 已过期，请重新读取当前设备")
            if not self._identity_valid_locked():
                raise ValidationError("当前设备 chip_id 尚未识别")
            if request["chip_id"] != self.chip_id:
                raise ValidationError("灯板校色 chip_id 与当前设备不一致")
            if request["board"] != self.board:
                raise ValidationError("灯板校色 board 与当前设备不一致")
            if self.armed or self.playback["state"] != "stopped" or self.recording["state"] == "recording":
                raise StudioError("保存灯板校色前必须锁定舵机并停止播放、录制")
            expected_epoch = self.connection_epoch
            expected_chip_id = self.chip_id
            logical_led = copy.deepcopy(self.target_pose["led"])

        if request["reset"]:
            resetter = getattr(self.protocol, "led_calibration_reset", None)
            if not callable(resetter):
                raise StudioError("当前固件不支持灯板校色")
            calibration, calibration_reply = resetter()
        else:
            setter = getattr(self.protocol, "led_calibration_set", None)
            if not callable(setter):
                raise StudioError("当前固件不支持灯板校色")
            calibration, calibration_reply = setter(request)
        if not calibration["persistent"]:
            raise StudioError("设备未确认灯板校色已持久化")

        with self.lock:
            if not (
                self._identity_valid_locked()
                and self.connection_epoch == expected_epoch
                and self.chip_id == expected_chip_id
            ):
                raise DeviceOfflineError("灯板校色期间设备连接或 chip_id 已变更")

        # Calibration is output-side device state.  Re-send the current
        # logical color verbatim; never rewrite a project/keyframe #RRGGBB.
        light_reply = self.protocol.light(logical_led)
        watchdog_reply = self.protocol.watchdog(SERVO_LEASE_MS)

        with self.lock:
            if not (
                self._identity_valid_locked()
                and self.connection_epoch == expected_epoch
                and self.chip_id == expected_chip_id
            ):
                raise DeviceOfflineError("灯板重发期间设备连接或 chip_id 已变更")
            self.command_seq += 1
            sequence = self.command_seq
            self.confirmed_pose["led"] = copy.deepcopy(logical_led)
            self.confirmed_pose["seq"] = sequence
            self.confirmed_pose["confirmed_at"] = time.time()
            self.latency_ms = max(
                calibration_reply.elapsed_ms,
                light_reply.elapsed_ms,
                watchdog_reply.elapsed_ms,
            )
            self.error = None
            self._light_manual_active = True
            self._last_client_seen = time.monotonic()
            result = self._led_calibration_context_locked()
        result.update(
            {
                "available": True,
                "reason": "available",
                "calibration": copy.deepcopy(calibration),
                "reset": request["reset"],
                "light_resent": True,
                "logical_led": copy.deepcopy(logical_led),
            }
        )
        self.publish()
        return result

    @staticmethod
    def _calibration_steps(record: dict[str, Any] | None) -> dict[str, bool]:
        return {
            "servo": record is not None,
            "home": record is not None and record.get("home_pose_us") is not None,
            "imu": record is not None and record.get("imu_mounting_baseline_deg") is not None,
        }

    def calibration_snapshot(self) -> dict[str, Any]:
        """Return identity/applicability without ever applying stale records."""
        with self.lock:
            online = bool(self.online and self.protocol.online)
            chip_id = self.chip_id
            board = self.board
            connection_epoch = self.connection_epoch
            identity_epoch = self._identity_epoch
            identity_valid = bool(
                online and chip_id is not None and identity_epoch == connection_epoch
            )
            filter_epoch = self._imu_filter_epoch
            sample_seq = self._imu_sample_seq
            timestamp_ms = self._imu.get("timestamp_ms")
            record = self.store.get_calibration(chip_id) if identity_valid else None
        if not online:
            applicable, reason = False, "device_offline"
        elif not identity_valid:
            applicable, reason = False, "chip_id_unavailable"
        elif record is None:
            applicable, reason = False, "calibration_not_found"
        elif record["board"] != board:
            applicable, reason = False, "board_mismatch"
        else:
            applicable, reason = True, "applicable"
        steps = self._calibration_steps(record if applicable else None)
        return {
            "format": CALIBRATION_FORMAT,
            "version": CALIBRATION_VERSION,
            "connection_epoch": connection_epoch,
            "filter_epoch": filter_epoch,
            "sample_seq": sample_seq,
            "timestamp_ms": timestamp_ms,
            "current": {
                "online": online,
                "chip_id": chip_id,
                "board": board,
                "mounting_revision": record["mounting_revision"] if applicable else None,
                "connection_epoch": connection_epoch,
                "identity_epoch": identity_epoch,
                "identity_valid": identity_valid,
                "filter_epoch": filter_epoch,
                "sample_seq": sample_seq,
                "timestamp_ms": timestamp_ms,
            },
            "applicable": applicable,
            "complete": all(steps.values()),
            "completed_steps": steps,
            "reason": reason,
            "record": copy.deepcopy(record) if applicable else None,
        }

    def save_calibration(self, payload: Any) -> dict[str, Any]:
        if not self._actuation_op_lock.acquire(blocking=False):
            raise StudioError("已有设备输出操作正在执行，暂不能保存舵机标定")
        try:
            return self._save_calibration_exclusive(payload)
        finally:
            self._actuation_op_lock.release()

    def _save_calibration_exclusive(self, payload: Any) -> dict[str, Any]:
        if not isinstance(payload, dict):
            raise ValidationError("校准请求必须是对象")
        if "connection_epoch" not in payload:
            raise ValidationError("校准请求缺少 connection_epoch")
        request_epoch = require_json_int(payload["connection_epoch"], "connection_epoch", 1, 2_147_483_647)
        record_payload = copy.deepcopy(payload)
        record_payload.pop("connection_epoch")
        with self.lock:
            if not (self.online and self.protocol.online):
                raise DeviceOfflineError("设备离线，不能保存校准")
            if request_epoch != self.connection_epoch:
                raise ValidationError("校准 connection_epoch 已过期，请重新读取当前设备")
            if self.chip_id is None or self._identity_epoch != self.connection_epoch:
                raise ValidationError("当前设备 chip_id 尚未识别")
            if self.armed or self.playback["state"] != "stopped" or self.recording["state"] == "recording":
                raise StudioError("保存校准前必须锁定舵机并停止播放、录制")
            record = normalize_calibration_record(record_payload)
            if record["chip_id"] != self.chip_id:
                raise ValidationError("校准 chip_id 与当前设备不一致")
            if record["board"] != self.board:
                raise ValidationError("校准 board 与当前设备不一致")
            self.store.put_calibration(record)
            result = self.calibration_snapshot()
        self.publish()
        return result

    def playback_start(
        self,
        action_id: str,
        *,
        rate: float | None = None,
        loop: bool | None = None,
        loop_count: int | None = None,
    ) -> dict[str, Any]:
        with self.lock:
            if not self.armed:
                raise StudioError("播放动作前必须先解锁")
            if (
                not self._identity_valid_locked()
                or self._armed_connection_epoch != self.connection_epoch
                or self._armed_chip_id != self.chip_id
            ):
                raise StudioError("当前设备身份已变更，请重新解锁")
            if self.recording["state"] == "recording":
                raise StudioError("录制期间不能播放")
            if self.playback["state"] != "stopped":
                raise StudioError("已有动作在播放")
            action = self.store.get_action(action_id)
            action = normalize_action(
                action,
                self._motion_profile_locked(),
                existing_id=action["id"],
            )
            if not action["keyframes"]:
                raise StudioError("动作没有关键帧")
            selected_rate = require_float(
                action["default_rate"] if rate is None else rate,
                "rate",
                PLAYBACK_MIN_RATE,
                PLAYBACK_MAX_RATE,
            )
            selected_loop = action["loop"]["enabled"] if loop is None else bool(loop)
            selected_count = action["loop"]["count"] if loop_count is None else require_int(loop_count, "loop_count", 0, 10_000)
            self._latest_pose = None
            self._play_stop.clear()
            self._play_pause.clear()
            self.playback = {
                "state": "playing",
                "action_id": action_id,
                "position_ms": 0,
                "duration_ms": action["duration_ms"],
                "index": -1,
                "rate": selected_rate,
                "loop": selected_loop,
                "loop_count": selected_count,
                "completed_loops": 0,
            }
            self._play_thread = threading.Thread(
                target=self._playback_loop,
                args=(action, selected_rate, selected_loop, selected_count),
                name="motion-playback",
                daemon=True,
            )
            self._play_thread.start()
        self.publish()
        return copy.deepcopy(self.playback)

    def playback_pause(self) -> dict[str, Any]:
        with self.lock:
            if self.playback["state"] != "playing":
                raise StudioError("当前不在播放")
            self.playback["state"] = "paused"
            self._play_pause.set()
        with self._actuation_op_lock:
            return self._playback_pause_exclusive()

    def _playback_pause_exclusive(self) -> dict[str, Any]:
        try:
            self.protocol.stop_motion()
        except StudioError as exc:
            with self.lock:
                self.error = str(exc)
        self.publish()
        return copy.deepcopy(self.playback)

    def playback_resume(self) -> dict[str, Any]:
        with self.lock:
            if self.playback["state"] != "paused":
                raise StudioError("当前没有暂停的动作")
            self.playback["state"] = "playing"
            self._play_pause.clear()
        self.publish()
        return copy.deepcopy(self.playback)

    def playback_stop(self, *, off: bool = False) -> dict[str, Any]:
        with self.lock:
            self._play_stop.set()
            self._play_pause.clear()
            self.playback["state"] = "stopped"
        with self._actuation_op_lock:
            return self._playback_stop_exclusive(off=off)

    def _playback_stop_exclusive(self, *, off: bool = False) -> dict[str, Any]:
        try:
            if self.protocol.online:
                if off:
                    # This is an output-off operation while the UI remains
                    # armed. Keep its watchdog lease enabled; only disarm and
                    # emergency-stop disable the watchdog itself.
                    self.protocol.all_off()
                else:
                    self.protocol.stop_motion()
        except StudioError as exc:
            with self.lock:
                self.error = str(exc)
        self.publish()
        return copy.deepcopy(self.playback)

    def _pose_loop(self) -> None:
        next_send = 0.0
        while True:
            self._pose_event.wait(timeout=0.20)
            self._pose_event.clear()
            with self.lock:
                if not self.running:
                    return
                pending = self._latest_pose
                self._latest_pose = None
            if pending is None:
                continue
            delay = next_send - time.monotonic()
            if delay > 0:
                time.sleep(delay)
                with self.lock:
                    if self._latest_pose is not None:
                        pending = self._latest_pose
                        self._latest_pose = None
            sequence, pose = pending
            self._apply_pose(sequence, pose)
            next_send = time.monotonic() + SERVO_LIVE_INTERVAL_S

    def _apply_pose(self, sequence: int, pose: dict[str, Any]) -> bool:
        # A stale coalesced slider/playback frame must wait behind disarm and
        # then re-check `armed`; it must never land after an emergency safe_off.
        with self._actuation_op_lock:
            return self._apply_pose_exclusive(sequence, pose)

    def _apply_pose_exclusive(self, sequence: int, pose: dict[str, Any]) -> bool:
        with self.lock:
            if not (
                self.running
                and self.armed
                and self.online
                and self.protocol.online
                and self._identity_valid_locked()
                and self._armed_connection_epoch == self.connection_epoch
                and self._armed_chip_id == self.chip_id
            ):
                return False
            runtime_pose = normalize_pose(pose, self._motion_profile_locked())
            runtime_pose["source"] = str(pose.get("source", "manual"))[:32]
        try:
            frame_reply = self.protocol.frame(runtime_pose)
        except StudioError as exc:
            with self.lock:
                self.error = str(exc)
                self.armed = False
                self._armed_connection_epoch = None
                self._armed_chip_id = None
                self.estop = True
                self._safety_off_pending = True
                self._play_stop.set()
            try:
                if self.protocol.online:
                    self.protocol.safe_off()
                    with self.lock:
                        self._safety_off_pending = False
            except StudioError:
                pass
            self.publish()
            return False
        with self.lock:
            confirmed = copy.deepcopy(runtime_pose)
            confirmed.update({"seq": sequence, "confirmed_at": time.time()})
            self.confirmed_pose = confirmed
            self.latency_ms = frame_reply.elapsed_ms
            self.error = None
            if self.recording["state"] == "recording":
                self._append_record_frame_locked(confirmed)
        self.publish()
        return True

    def _append_record_frame_locked(self, pose: dict[str, Any]) -> None:
        elapsed_ms = max(0, int((time.monotonic() - self._record_started_mono) * 1000))
        if self._record_frames:
            elapsed_ms = max(elapsed_ms, self._record_frames[-1]["time_ms"] + 1)
            last = self._record_frames[-1]
            if (
                last["servos_us"] == pose["servos_us"]
                and last["led"] == pose["led"]
                and last["duration_ms"] == pose["duration_ms"]
                and last["easing"] == pose["easing"]
            ):
                return
        self._record_frames.append(
            {
                "id": uuid.uuid4().hex,
                "time_ms": elapsed_ms,
                "duration_ms": int(pose["duration_ms"]),
                "easing": str(pose["easing"]),
                "servos_us": list(pose["servos_us"]),
                "led": copy.deepcopy(pose["led"]),
            }
        )
        self.recording["frame_count"] = len(self._record_frames)
        self.recording["duration_ms"] = elapsed_ms

    def _playback_loop(self, action: dict[str, Any], rate: float, loop: bool, loop_count: int) -> None:
        completed = 0
        try:
            while not self._play_stop.is_set():
                loop_started = time.monotonic()
                paused_total = 0.0
                for index, frame in enumerate(action["keyframes"]):
                    target_seconds = frame["time_ms"] / 1000.0 / rate
                    while not self._play_stop.is_set():
                        if self._play_pause.is_set():
                            paused_at = time.monotonic()
                            while self._play_pause.is_set() and not self._play_stop.wait(0.05):
                                pass
                            paused_total += time.monotonic() - paused_at
                            continue
                        remaining = loop_started + paused_total + target_seconds - time.monotonic()
                        if remaining <= 0:
                            break
                        self._play_stop.wait(min(remaining, 0.02))
                    if self._play_stop.is_set():
                        break
                    with self.lock:
                        if not self.armed:
                            self._play_stop.set()
                            break
                        self.command_seq += 1
                        sequence = self.command_seq
                        pose = {
                            "servos_us": list(frame["servos_us"]),
                            "duration_ms": int(round(frame["duration_ms"] / rate)),
                            "easing": frame["easing"],
                            "led": copy.deepcopy(frame["led"]),
                            "source": "playback",
                        }
                        self.target_pose = copy.deepcopy(pose)
                        self.playback["index"] = index
                        self.playback["position_ms"] = frame["time_ms"]
                    if not self._apply_pose(sequence, pose):
                        self._play_stop.set()
                        break
                if self._play_stop.is_set():
                    break
                # A frame command is non-blocking: the final target may still
                # be interpolating after its timestamp.  Keep this cycle alive
                # until the normalized action duration so loop seams and the
                # UI duration match what the firmware is actually executing.
                target_seconds = action["duration_ms"] / 1000.0 / rate
                while not self._play_stop.is_set():
                    if self._play_pause.is_set():
                        paused_at = time.monotonic()
                        while self._play_pause.is_set() and not self._play_stop.wait(0.05):
                            pass
                        paused_total += time.monotonic() - paused_at
                        continue
                    remaining = loop_started + paused_total + target_seconds - time.monotonic()
                    if remaining <= 0:
                        break
                    self._play_stop.wait(min(remaining, 0.02))
                if self._play_stop.is_set():
                    break
                completed += 1
                with self.lock:
                    self.playback["completed_loops"] = completed
                    self.playback["position_ms"] = action["duration_ms"]
                if not loop or (loop_count > 0 and completed >= loop_count):
                    break
            if action.get("end_behavior") == "off" and self.protocol.online:
                with self._actuation_op_lock:
                    self.protocol.all_off()
        except StudioError as exc:
            with self.lock:
                self.error = str(exc)
                self.armed = False
                self._armed_connection_epoch = None
                self._armed_chip_id = None
                self.estop = True
        finally:
            with self.lock:
                self.playback["state"] = "stopped"
                self.playback["position_ms"] = 0
                self.playback["index"] = -1
            self.publish()

    def _monitor_loop(self) -> None:
        last_state_poll = 0.0
        while True:
            time.sleep(0.20)
            with self.lock:
                if not self.running:
                    return
                no_clients = self._client_count == 0
                client_expired = no_clients and time.monotonic() - self._last_client_seen > self.client_grace_s
                should_disarm = (self.armed or self._light_manual_active) and client_expired
                should_poll = (
                    (self.armed or self._light_manual_active)
                    and self.online
                    and time.monotonic() - last_state_poll >= 0.35
                )
            if should_disarm:
                self.disarm("client-timeout")
                continue
            if should_poll:
                last_state_poll = time.monotonic()
                try:
                    state, reply = self.protocol.state()
                    with self.lock:
                        self.servo_state = state
                        self.latency_ms = reply.elapsed_ms
                except StudioError as exc:
                    with self.lock:
                        self.error = str(exc)
                    self.disarm("command-failed")
                self.publish()

    def _imu_loop(self) -> None:
        while not self._imu_stop.is_set():
            with self.lock:
                running = self.running
                should_poll = self.online and self.protocol.online
            if not running:
                return
            # IMU is observational only. If another shell command owns the
            # transport, skip this slot instead of queueing behind motion or a
            # safety operation.
            if should_poll and not bool(getattr(self.protocol, "command_busy", False)):
                self.poll_imu_once()
            self._imu_stop.wait(self.imu_poll_interval_s)


STUDIO_HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ARCS Motion Studio</title>
<style>
:root{color-scheme:dark;--bg:#07090d;--panel:#0d1118;--panel2:#111722;--line:#202a38;
 --text:#edf2f8;--muted:#8491a4;--cyan:#42e8d1;--blue:#6695ff;--red:#ff5364;
 --amber:#ffbf5b;--shadow:0 24px 70px #0008}*{box-sizing:border-box}
html,body{height:100%}body{margin:0;background:radial-gradient(circle at 10% -20%,#183044 0,
 transparent 34%),radial-gradient(circle at 90% 0,#17233a 0,transparent 29%),var(--bg);
 color:var(--text);font:14px/1.45 Inter,-apple-system,BlinkMacSystemFont,"Segoe UI",
 "Noto Sans SC","Microsoft YaHei",sans-serif;letter-spacing:.01em}
button,input,select{font:inherit}button{color:inherit}.app{min-height:100%;display:grid;
 grid-template-rows:74px 1fr}.topbar{display:flex;align-items:center;gap:18px;padding:0 24px;
 border-bottom:1px solid #ffffff10;background:#080b10d9;backdrop-filter:blur(18px);position:sticky;
 top:0;z-index:20}.brand{display:flex;align-items:center;gap:12px;min-width:265px}.mark{width:36px;
 height:36px;border-radius:11px;background:linear-gradient(145deg,var(--cyan),var(--blue));
 box-shadow:0 0 34px #42e8d144;display:grid;place-items:center;color:#06120f;font-weight:900}
.brand strong{display:block;font-size:15px;letter-spacing:.08em}.brand small{display:block;color:var(--muted);
 font-size:10px;letter-spacing:.16em;margin-top:2px}.statusline{display:flex;gap:9px;align-items:center;
 flex:1}.pill{border:1px solid var(--line);background:#0d121a;padding:7px 11px;border-radius:999px;
 color:var(--muted);font-size:12px;display:flex;gap:7px;align-items:center}.dot{width:7px;height:7px;
 border-radius:50%;background:#465063}.pill.online .dot{background:var(--cyan);box-shadow:0 0 12px var(--cyan)}
.pill.armed{color:#fff;border-color:#38d3bb66;background:#112a28}.pill.armed .dot{background:var(--cyan)}
.estop{border:0;border-radius:12px;background:linear-gradient(180deg,#ff6270,#d73548);padding:12px 20px;
 font-weight:800;letter-spacing:.08em;box-shadow:0 9px 28px #ff405044;cursor:pointer}.estop:active{transform:translateY(1px)}
.workspace{display:grid;grid-template-columns:minmax(660px,1fr) 350px;gap:16px;padding:16px;
 max-width:1680px;width:100%;margin:auto}.stack{display:grid;gap:16px;align-content:start}.panel{background:linear-gradient(180deg,
 #111721f0,#0b0f16f4);border:1px solid #ffffff12;border-radius:18px;box-shadow:var(--shadow);overflow:hidden}
.panel-head{display:flex;align-items:center;justify-content:space-between;padding:17px 18px;border-bottom:1px solid #ffffff0e}
.panel-head h2{font-size:13px;margin:0;text-transform:uppercase;letter-spacing:.11em}.sub{color:var(--muted);font-size:12px}
.pose-grid{display:grid;grid-template-columns:repeat(2,minmax(260px,1fr));gap:12px;padding:16px}.servo{padding:16px;
 border:1px solid #ffffff0c;background:linear-gradient(145deg,#121a25,#0c1119);border-radius:15px;position:relative;
 overflow:hidden}.servo:before{content:"";position:absolute;inset:0 0 auto;height:2px;background:linear-gradient(90deg,
 transparent,var(--channel),transparent);opacity:.8}.servo-title{display:flex;align-items:flex-start;justify-content:space-between;
 margin-bottom:14px}.servo-title b{font-size:15px}.servo-title span{color:var(--muted);font:11px ui-monospace,monospace}
.readout{font:700 25px/1 ui-monospace,SFMono-Regular,Consolas,monospace;letter-spacing:-.05em}.readout small{
 font-size:11px;color:var(--muted);letter-spacing:0;margin-left:4px}.range-row{display:grid;grid-template-columns:1fr 64px;
 align-items:center;gap:12px;margin-top:17px}input[type=range]{appearance:none;width:100%;height:5px;border-radius:9px;
 background:linear-gradient(90deg,var(--channel) var(--fill,50%),#273140 var(--fill,50%));outline:0}
input[type=range]::-webkit-slider-thumb{appearance:none;width:19px;height:19px;border-radius:50%;background:#f7fbff;
 border:4px solid var(--channel);box-shadow:0 3px 14px #0009;cursor:grab}input[type=number],select,input[type=text]{
 border:1px solid var(--line);background:#0b1017;color:var(--text);border-radius:9px;padding:9px 10px;outline:none}
input:focus,select:focus{border-color:#56b8ff88;box-shadow:0 0 0 3px #4b9fff12}.micro{font-size:11px;color:var(--muted);
 display:flex;justify-content:space-between;margin-top:9px}.control-strip{padding:14px 16px;display:grid;grid-template-columns:1fr 1fr 1fr;
 gap:12px;border-top:1px solid #ffffff0e}.field label{display:block;color:var(--muted);font-size:11px;margin:0 0 6px}
.field input,.field select{width:100%}.light-wrap{padding:16px;display:grid;gap:15px}.light-top{display:grid;
 grid-template-columns:auto 1fr 74px 78px;gap:11px;align-items:end}.toggle{display:flex;align-items:center;gap:9px;padding-bottom:9px}
.toggle input{accent-color:var(--cyan);width:17px;height:17px}input[type=color]{width:100%;height:39px;border:1px solid var(--line);
 padding:3px;border-radius:9px;background:#0b1017}.actions{padding:12px;display:grid;gap:8px;max-height:335px;overflow:auto}
.action{display:grid;grid-template-columns:1fr auto;gap:10px;padding:12px;border:1px solid transparent;border-radius:12px;
 background:#0d131c;cursor:pointer}.action:hover{border-color:#ffffff1d}.action.selected{border-color:#4be4cf77;
 background:#10201f}.action b{display:block}.action small{color:var(--muted)}.iconbtn,.btn{border:1px solid var(--line);
 background:#111824;border-radius:10px;padding:9px 12px;cursor:pointer}.btn.primary{border-color:#4be4cf55;
 background:linear-gradient(180deg,#183b39,#102825);color:#dffff9}.btn.warn{border-color:#ffbf5b44;color:#ffd994}
.btn:disabled,.iconbtn:disabled{opacity:.4;cursor:not-allowed}.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.rec{color:#ff8792}.rec.active{background:#3a151d;border-color:#ff5969;box-shadow:0 0 20px #ff405022}.timeline{padding:16px}
.rail{height:94px;background:linear-gradient(180deg,#0a0e15,#0c121b);border:1px solid var(--line);border-radius:12px;
 position:relative;overflow:hidden}.rail:before{content:"";position:absolute;inset:0;background:repeating-linear-gradient(90deg,
 transparent 0,transparent calc(10% - 1px),#ffffff0a calc(10% - 1px),#ffffff0a 10%)}.key{position:absolute;
 top:23px;width:11px;height:48px;border-radius:5px;background:linear-gradient(var(--cyan),var(--blue));transform:translateX(-50%);
 box-shadow:0 0 12px #4ce1d755}.playhead{position:absolute;top:0;bottom:0;width:2px;background:#fff;box-shadow:0 0 10px #fff8}
.safety{padding:16px;display:grid;gap:12px}.armbox{display:flex;justify-content:space-between;align-items:center;padding:13px;
 border:1px solid #ffffff10;background:#0d131c;border-radius:12px}.armbox b{display:block}.armbox small{color:var(--muted)}
.switch{position:relative;width:48px;height:27px}.switch input{display:none}.switch span{position:absolute;inset:0;background:#252e3c;
 border-radius:99px;cursor:pointer;transition:.2s}.switch span:after{content:"";position:absolute;width:21px;height:21px;top:3px;left:3px;
 border-radius:50%;background:#e8edf5;transition:.2s}.switch input:checked+span{background:#1d8f7f}.switch input:checked+span:after{
 transform:translateX(21px)}.power{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.metric{padding:10px;
 border-radius:10px;background:#0b1017;border:1px solid #ffffff0b}.metric small{display:block;color:var(--muted);font-size:10px}
.metric b{font:600 14px ui-monospace,monospace}.notice{padding:10px 12px;border-radius:10px;background:#271a0d;
 border:1px solid #ffbe4c33;color:#ffd99c;font-size:12px}.notice.error{background:#2b1016;border-color:#ff506044;color:#ffabb4}
.empty{padding:28px;text-align:center;color:var(--muted)}.footerline{display:flex;justify-content:space-between;color:var(--muted);
 font-size:11px;padding:0 2px}.hidden{display:none!important}@media(max-width:1050px){.workspace{grid-template-columns:1fr}.right{
 grid-row:1}.statusline .pill:nth-child(n+3){display:none}}@media(max-width:680px){.workspace{padding:8px}.topbar{padding:0 10px}
.brand{min-width:0}.brand small{display:none}.pose-grid{grid-template-columns:1fr}.control-strip{grid-template-columns:1fr 1fr}.light-top{
 grid-template-columns:1fr 1fr}.estop{padding:11px}.statusline{display:none}}
</style>
</head>
<body>
<div class="app">
 <header class="topbar">
  <div class="brand"><div class="mark">A</div><div><strong>ARCS MOTION STUDIO</strong><small>QUADRUPED CONTROL SYSTEM</small></div></div>
  <div class="statusline"><div id="onlinePill" class="pill"><i class="dot"></i><span id="onlineText">离线</span></div>
   <div id="armedPill" class="pill"><i class="dot"></i><span id="armedText">安全锁定</span></div>
   <div class="pill"><span id="deviceText">transport —</span></div><div class="pill"><span id="latencyText">延迟 —</span></div></div>
  <button class="estop" id="estop">STOP · 急停</button>
 </header>
 <main class="workspace">
  <section class="stack">
   <article class="panel">
    <div class="panel-head"><div><h2>四足姿态控制</h2><div class="sub">四路同步目标 · 50 Hz PWM · 板端平滑插值</div></div>
     <div class="toolbar"><button class="btn" id="centerAll">全部居中</button><button class="btn" id="sendPose">下发姿态</button></div></div>
    <div class="pose-grid" id="poseGrid"></div>
    <div class="control-strip"><div class="field"><label>过渡时间（速度）</label><input id="duration" type="number" min="0" max="60000" step="20" value="320"></div>
     <div class="field"><label>缓动曲线</label><select id="easing"><option value="smooth">平滑加减速</option><option value="linear">线性</option><option value="ease-in">缓入</option><option value="ease-out">缓出</option></select></div>
     <div class="field"><label>控制模式</label><select id="liveMode"><option value="live">实时跟手</option><option value="release">松手下发</option></select></div></div>
   </article>
   <article class="panel">
    <div class="panel-head"><div><h2>灯板场景</h2><div class="sub">WS2812 / PA08 · 与姿态帧同步录制</div></div><button class="btn" id="lightApply">应用灯光</button></div>
    <div class="light-wrap"><div class="light-top"><label class="toggle"><input id="lightOn" type="checkbox"><span>启用灯板</span></label>
     <div class="field"><label>颜色</label><input id="lightColor" type="color" value="#20d6c7"></div>
     <div class="field"><label>亮度 %</label><input id="brightness" type="number" min="0" max="100" value="35"></div>
     <div class="field"><label>灯数</label><input id="pixelCount" type="number" min="1" max="64" value="8"></div></div></div>
   </article>
   <article class="panel">
    <div class="panel-head"><div><h2>动作时间轴</h2><div class="sub" id="timelineMeta">未选择动作</div></div>
     <div class="toolbar"><button class="btn primary" id="playBtn">▶ 播放</button><button class="btn" id="pauseBtn">暂停</button><button class="btn" id="stopBtn">停止</button>
      <select id="rate"><option value="0.5">0.5×</option><option value="0.75">0.75×</option><option value="1" selected>1.0×</option><option value="1.5">1.5×</option><option value="2">2.0×</option></select>
      <label class="toggle"><input id="loop" type="checkbox"><span>循环</span></label></div></div>
    <div class="timeline"><div class="rail" id="rail"><div class="playhead" id="playhead" style="left:0"></div></div></div>
   </article>
  </section>
  <aside class="stack right">
   <article class="panel"><div class="panel-head"><div><h2>设备安全</h2><div class="sub">解锁前必须完成供电预检</div></div></div>
    <div class="safety"><div class="armbox"><div><b>舵机输出</b><small id="armHint">当前锁定</small></div><label class="switch"><input id="arm" type="checkbox"><span></span></label></div>
     <label class="toggle"><input id="externalPower" type="checkbox"><span>已确认外部舵机电源</span></label>
     <div class="power"><div class="metric"><small>VBAT / SERVO</small><b id="voltage">—</b></div><div class="metric"><small>看门狗</small><b id="watchdog">未激活</b></div></div>
     <div id="notice" class="notice">连接设备后，上位机会先执行全路断开。</div></div>
   </article>
   <article class="panel"><div class="panel-head"><div><h2>动作库</h2><div class="sub" id="projectName">ARCS-MINI 动作项目</div></div>
     <div class="toolbar"><button class="iconbtn" id="exportBtn">导出</button><button class="iconbtn" id="importBtn">导入</button><input id="importFile" type="file" accept=".json,.arcsmotion.json" class="hidden"></div></div>
    <div class="actions" id="actions"><div class="empty">录制第一个动作</div></div>
    <div class="panel-head"><div class="toolbar"><button class="btn rec" id="recordBtn">● 开始录制</button><button class="btn" id="recordStop" disabled>结束录制</button></div><span class="sub" id="recordMeta">0 帧</span></div>
   </article>
   <div class="footerline"><span>ARCS V2.1 · HEADLESS</span><span id="clock"></span></div>
  </aside>
 </main>
</div>
<script>
const $=id=>document.getElementById(id);let state=null,selectedAction=null,poseTimer=null,dragging=false;
const servoColors=['#42e8d1','#6695ff','#bd7dff','#ffbf5b'];
function esc(v){return String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function envelope(ok,error=null,result=null,s=null){return {ok,error,result,state:s}}
async function api(path,method='GET',body=null){const opt={method,headers:{}};if(body!==null){opt.headers['Content-Type']='application/json';opt.body=JSON.stringify(body)}
 const r=await fetch(path,opt);let data;try{data=await r.json()}catch(e){throw new Error('上位机返回了无效响应')}if(!r.ok||!data.ok)throw new Error(data.error||('HTTP '+r.status));if(data.state)render(data.state);return data}
function makeServos(){const names=['通道 0','通道 1','通道 2','通道 3'];$('poseGrid').innerHTML=names.map((n,i)=>`<section class="servo" style="--channel:${servoColors[i]}">
 <div class="servo-title"><div><b id="sname${i}">${n}</b><span id="spin${i}">PA0${i+4} · CH${i+4}</span></div><div class="readout"><span id="sv${i}">1500</span><small>µs</small></div></div>
 <div class="range-row"><input id="s${i}" type="range" min="1000" max="2000" value="1500"><input id="sn${i}" type="number" min="1000" max="2000" value="1500"></div>
 <div class="micro"><span id="smin${i}">1000 µs</span><span>HOME 1500</span><span id="smax${i}">2000 µs</span></div></section>`).join('');
 for(let i=0;i<4;i++){const range=$('s'+i),num=$('sn'+i);const sync=(v,send)=>{v=Math.max(+range.min,Math.min(+range.max,+v||1500));range.value=v;num.value=v;$('sv'+i).textContent=v;range.style.setProperty('--fill',((v-range.min)/(range.max-range.min)*100)+'%');if(send)schedulePose()};
 range.addEventListener('pointerdown',()=>dragging=true);range.addEventListener('pointerup',()=>{dragging=false;if($('liveMode').value==='release')sendPose()});range.oninput=()=>sync(range.value,$('liveMode').value==='live');num.onchange=()=>sync(num.value,true);sync(1500,false)}}
function currentPose(source='manual'){return {servos_us:[0,1,2,3].map(i=>+$('s'+i).value),duration_ms:+$('duration').value,easing:$('easing').value,
 led:{enabled:$('lightOn').checked,color:$('lightColor').value,brightness:+$('brightness').value,pixel_count:+$('pixelCount').value},source}}
function schedulePose(){if(!state?.armed)return;if(poseTimer)return;poseTimer=setTimeout(()=>{poseTimer=null;sendPose()},40)}
async function sendPose(){try{await api('/api/pose','POST',currentPose())}catch(e){notice(e.message,true)}}
function notice(message,error=false){const n=$('notice');n.textContent=message;n.className='notice'+(error?' error':'')}
function fmt(ms){ms=Math.max(0,+ms||0);return Math.floor(ms/60000)+':'+String(Math.floor(ms/1000)%60).padStart(2,'0')+'.'+String(Math.floor(ms/100)%10)}
function render(s){state=s;const online=!!s.online;$('onlinePill').className='pill '+(online?'online':'');$('onlineText').textContent=online?'设备在线':'设备离线';
 $('armedPill').className='pill '+(s.armed?'armed':'');$('armedText').textContent=s.armed?'输出已解锁':'安全锁定';$('deviceText').textContent='transport '+(s.transport_id??'—');$('latencyText').textContent='延迟 '+(s.latency_ms==null?'—':s.latency_ms+' ms');
 $('arm').checked=!!s.armed;$('arm').disabled=!online;$('armHint').textContent=s.armed?'板端安全租约已启动':'当前锁定 · PWM低电平';
 const p=s.power;$('voltage').textContent=p?p.volt.toFixed(3)+' V':'—';const ss=s.servo_state;$('watchdog').textContent=ss?.watchdog_ms?ss.remaining_ms+' / '+ss.watchdog_ms+' ms':(s.armed?'刷新中':'未激活');
 if(s.error)notice(s.error,true);else if(s.safety_off_pending)notice('等待设备确认全路断开',true);else notice(s.armed?'控制权有效；关闭页面或断开ADB会自动锁定。':'安全状态：四路PWM已断开。');
 const limits=s.servo_limits?.channels||[];for(let i=0;i<4;i++){const l=limits[i]||{min_us:1000,center_us:1500,max_us:2000,name:'通道 '+i,pin:'PA0'+(i+4)};const r=$('s'+i),n=$('sn'+i);r.min=n.min=l.min_us;r.max=n.max=l.max_us;$('sname'+i).textContent=l.name;$('spin'+i).textContent=l.pin+' · CH'+(i+4);$('smin'+i).textContent=l.min_us+' µs';$('smax'+i).textContent=l.max_us+' µs';if(document.activeElement!==r&&document.activeElement!==n&&!dragging){const v=s.target_pose?.servos_us?.[i]??l.center_us;r.value=n.value=v;$('sv'+i).textContent=v;r.style.setProperty('--fill',((v-l.min_us)/(l.max_us-l.min_us)*100)+'%')}}
 if(!dragging&&s.target_pose){$('duration').value=s.target_pose.duration_ms;$('easing').value=s.target_pose.easing;const led=s.target_pose.led||{};$('lightOn').checked=!!led.enabled;$('lightColor').value=led.color||'#20d6c7';$('brightness').value=led.brightness??35;$('pixelCount').value=led.pixel_count??8}
 $('projectName').textContent=s.project?.name||'动作项目';renderActions(s.project?.actions||[]);const rec=s.recording||{};$('recordBtn').disabled=!s.armed||rec.state==='recording';$('recordStop').disabled=rec.state!=='recording';$('recordBtn').className='btn rec'+(rec.state==='recording'?' active':'');$('recordMeta').textContent=(rec.frame_count||0)+' 帧 · '+fmt(rec.duration_ms||0);
 const pb=s.playback||{};$('playBtn').disabled=!s.armed||!selectedAction||pb.state!=='stopped';$('pauseBtn').disabled=pb.state!=='playing';$('stopBtn').disabled=pb.state==='stopped';renderTimeline(s.project?.actions||[],pb)}
function renderActions(actions){if(selectedAction&&!actions.some(a=>a.id===selectedAction))selectedAction=null;const box=$('actions');if(!actions.length){box.innerHTML='<div class="empty">录制第一个动作</div>';return}box.innerHTML=actions.map(a=>`<div class="action ${a.id===selectedAction?'selected':''}" data-id="${esc(a.id)}"><div><b>${esc(a.name)}</b><small>${a.keyframes.length} 帧 · ${fmt(a.duration_ms)}</small></div><button class="iconbtn del" data-id="${esc(a.id)}">删除</button></div>`).join('');box.querySelectorAll('.action').forEach(x=>x.onclick=e=>{if(e.target.classList.contains('del'))return;selectedAction=x.dataset.id;render(state)});box.querySelectorAll('.del').forEach(x=>x.onclick=async e=>{e.stopPropagation();if(confirm('删除这个动作？'))try{await api('/api/actions/'+x.dataset.id,'DELETE')}catch(err){notice(err.message,true)}})}
function renderTimeline(actions,pb){const action=actions.find(a=>a.id===selectedAction),rail=$('rail');rail.querySelectorAll('.key').forEach(x=>x.remove());if(!action){$('timelineMeta').textContent='未选择动作';$('playhead').style.left='0';return}$('timelineMeta').textContent=action.name+' · '+action.keyframes.length+' 帧 · '+fmt(action.duration_ms);const duration=Math.max(1,action.duration_ms);for(const f of action.keyframes){const k=document.createElement('i');k.className='key';k.style.left=(f.time_ms/duration*100)+'%';k.title=fmt(f.time_ms)+' · '+f.servos_us.join(' / ');rail.appendChild(k)}$('playhead').style.left=((pb.action_id===action.id?pb.position_ms:0)/duration*100)+'%'}
async function armChanged(){try{if($('arm').checked)await api('/api/arm','POST',{armed:true,external_power_confirmed:$('externalPower').checked});else await api('/api/arm','POST',{armed:false})}catch(e){$('arm').checked=false;notice(e.message,true)}}
async function playback(op){try{const body={operation:op};if(op==='play'){body.action_id=selectedAction;body.rate=+$('rate').value;body.loop=$('loop').checked;body.loop_count=0}await api('/api/playback','POST',body)}catch(e){notice(e.message,true)}}
makeServos();$('arm').onchange=armChanged;$('estop').onclick=()=>api('/api/estop','POST',{}).catch(e=>notice(e.message,true));$('sendPose').onclick=sendPose;$('lightApply').onclick=()=>api('/api/light','POST',currentPose().led).catch(e=>notice(e.message,true));
$('centerAll').onclick=()=>{for(let i=0;i<4;i++){const l=state?.servo_limits?.channels?.[i];$('s'+i).value=$('sn'+i).value=l?.center_us||1500;$('sv'+i).textContent=$('s'+i).value}sendPose()};
$('recordBtn').onclick=()=>api('/api/record/start','POST',{}).catch(e=>notice(e.message,true));$('recordStop').onclick=async()=>{const name=prompt('动作名称','新动作');if(name)try{const r=await api('/api/record/stop','POST',{name});selectedAction=r.result?.id||selectedAction}catch(e){notice(e.message,true)}};
$('playBtn').onclick=()=>playback('play');$('pauseBtn').onclick=()=>playback('pause');$('stopBtn').onclick=()=>playback('stop');
$('exportBtn').onclick=()=>{if(!state?.project)return;const b=new Blob([JSON.stringify(state.project,null,2)],{type:'application/json'}),a=document.createElement('a');a.href=URL.createObjectURL(b);a.download=(state.project.name||'motion')+'.arcsmotion.json';a.click();URL.revokeObjectURL(a.href)};
$('importBtn').onclick=()=>$('importFile').click();$('importFile').onchange=async e=>{const f=e.target.files[0];if(!f)return;try{await api('/api/project','PUT',JSON.parse(await f.text()));selectedAction=null}catch(err){notice(err.message,true)}e.target.value=''};
document.addEventListener('keydown',e=>{if(e.key==='Escape'){e.preventDefault();$('estop').click()}if(e.code==='Space'&&!/INPUT|SELECT|TEXTAREA/.test(e.target.tagName)){e.preventDefault();const pb=state?.playback?.state;if(pb==='playing')playback('pause');else if(pb==='paused')playback('resume');else if(selectedAction)playback('play')}});
const events=new EventSource('/api/events');events.onmessage=e=>{try{const d=JSON.parse(e.data);if(d.state)render(d.state)}catch(err){notice('状态流解析失败',true)}};events.onerror=()=>notice('与上位机状态流中断，正在重连…',true);
setInterval(()=>{$('clock').textContent=new Date().toLocaleTimeString()},1000);api('/api/state').catch(e=>notice(e.message,true));
</script>
</body></html>"""


def response_envelope(
    *, ok: bool, error: str | None = None, state: dict[str, Any] | None = None, result: Any = None
) -> dict[str, Any]:
    payload: dict[str, Any] = {"ok": bool(ok), "error": error}
    if state is not None:
        payload["state"] = state
    if result is not None:
        payload["result"] = result
    return payload


class StudioHandler(BaseHTTPRequestHandler):
    studio: MotionStudio

    def _json(self, payload: dict[str, Any], status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _success(self, result: Any = None, status: int = 200, include_state: bool = True) -> None:
        self._json(
            response_envelope(
                ok=True,
                state=self.studio.snapshot() if include_state else None,
                result=result,
            ),
            status,
        )

    def _failure(self, exc: Exception, status: int | None = None) -> None:
        if status is None:
            status = HTTPStatus.SERVICE_UNAVAILABLE if isinstance(exc, DeviceOfflineError) else HTTPStatus.BAD_REQUEST
        self._json(response_envelope(ok=False, error=str(exc), state=self.studio.snapshot()), int(status))

    def _read_json(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ValidationError("Content-Length 无效") from exc
        if length <= 0:
            return {}
        if length > HTTP_MAX_BODY:
            raise ValidationError(f"JSON 不能超过 {HTTP_MAX_BODY} 字节")
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValidationError("JSON 格式无效") from exc
        if not isinstance(payload, dict):
            raise ValidationError("JSON 根节点必须是对象")
        return payload

    @property
    def route(self) -> str:
        return unquote(urlparse(self.path).path).rstrip("/") or "/"

    def do_GET(self) -> None:  # noqa: N802
        try:
            route = self.route
            if route == "/":
                index = ASSET_DIR / "index.html"
                if index.is_file():
                    self._serve_file(index, "text/html; charset=utf-8", cache="no-store")
                else:
                    self._serve_bytes(
                        STUDIO_HTML.encode("utf-8"),
                        "text/html; charset=utf-8",
                        cache="no-store",
                        inline_fallback=True,
                    )
                return
            if route.startswith("/assets/"):
                relative = route[len("/assets/") :]
                if not relative or any(part in {"", ".", ".."} for part in Path(relative).parts):
                    self.send_error(404)
                    return
                candidate = (ASSET_DIR / relative).resolve()
                try:
                    candidate.relative_to(ASSET_DIR.resolve())
                except ValueError:
                    self.send_error(404)
                    return
                content_type = ASSET_TYPES.get(candidate.suffix.lower())
                if content_type is None or not candidate.is_file():
                    self.send_error(404)
                    return
                self._serve_file(candidate, content_type, cache="no-cache")
                return
            if route in {"/api/state", "/api/status"}:
                self._success()
                return
            if route == "/api/imu":
                self._success(self.studio.imu_snapshot(), include_state=False)
                return
            if route == "/api/led-calibration":
                self._success(self.studio.led_calibration_snapshot(), include_state=False)
                return
            if route == "/api/calibration":
                self._success(self.studio.calibration_snapshot(), include_state=False)
                return
            if route in {"/api/events", "/events"}:
                self._serve_events()
                return
            if route == "/api/project":
                self._success(self.studio.store.snapshot())
                return
            if route == "/api/actions":
                self._success(copy.deepcopy(self.studio.store.project["actions"]))
                return
            if route.startswith("/api/actions/"):
                action_id = require_identifier(route.rsplit("/", 1)[1], "action_id")
                self._success(self.studio.store.get_action(action_id))
                return
            self.send_error(404)
        except (StudioError, ValueError) as exc:
            self._failure(exc)

    def do_POST(self) -> None:  # noqa: N802
        try:
            route = self.route
            payload = self._read_json()
            if route == "/api/arm":
                if bool(payload.get("armed", True)):
                    result = self.studio.arm(override_power=bool(payload.get("external_power_confirmed", False)))
                else:
                    result = self.studio.disarm("user")
                self._success(result)
                return
            if route == "/api/estop":
                self._success(self.studio.emergency_stop())
                return
            if route == "/api/servo-test":
                self._success(self.studio.servo_test(payload))
                return
            if route == "/api/posture-test":
                self._success(self.studio.posture_test(payload))
                return
            if route == "/api/pose":
                result = self.studio.submit_pose(payload)
                self._success(result, status=HTTPStatus.ACCEPTED)
                return
            if route == "/api/light":
                self._success(self.studio.set_light(payload), status=HTTPStatus.ACCEPTED)
                return
            if route == "/api/record/start":
                self._success(self.studio.record_start())
                return
            if route == "/api/record/stop":
                action_id = payload.get("action_id")
                self._success(
                    self.studio.record_stop(
                        str(payload.get("name", "新动作")),
                        None if action_id in {None, ""} else str(action_id),
                    )
                )
                return
            if route == "/api/actions":
                self._success(self.studio.create_action(payload), status=HTTPStatus.CREATED)
                return
            if route == "/api/playback" or route.startswith("/api/playback/"):
                operation = route.rsplit("/", 1)[1] if route != "/api/playback" else str(payload.get("operation", ""))
                self._handle_playback(operation, payload)
                return
            self.send_error(404)
        except (StudioError, ValueError) as exc:
            self._failure(exc)

    def do_PUT(self) -> None:  # noqa: N802
        try:
            route = self.route
            payload = self._read_json()
            if route == "/api/project":
                self._success(self.studio.replace_project(payload))
                return
            if route == "/api/calibration":
                self._success(self.studio.save_calibration(payload), include_state=False)
                return
            if route == "/api/led-calibration":
                self._success(self.studio.save_led_calibration(payload), include_state=False)
                return
            if route.startswith("/api/actions/"):
                action_id = require_identifier(route.rsplit("/", 1)[1], "action_id")
                self._success(self.studio.update_action(action_id, payload))
                return
            self.send_error(404)
        except (StudioError, ValueError) as exc:
            self._failure(exc)

    def do_DELETE(self) -> None:  # noqa: N802
        try:
            route = self.route
            if route.startswith("/api/actions/"):
                action_id = require_identifier(route.rsplit("/", 1)[1], "action_id")
                self.studio.delete_action(action_id)
                self._success({"deleted": action_id})
                return
            self.send_error(404)
        except (StudioError, ValueError) as exc:
            self._failure(exc)

    def _handle_playback(self, operation: str, payload: dict[str, Any]) -> None:
        if operation == "play":
            action_id = require_identifier(payload.get("action_id"), "action_id")
            result = self.studio.playback_start(
                action_id,
                rate=payload.get("rate"),
                loop=payload.get("loop"),
                loop_count=payload.get("loop_count"),
            )
        elif operation == "pause":
            result = self.studio.playback_pause()
        elif operation == "resume":
            result = self.studio.playback_resume()
        elif operation == "stop":
            result = self.studio.playback_stop(off=bool(payload.get("off", False)))
        else:
            raise ValidationError("operation 必须是 play/pause/resume/stop")
        self._success(result)

    def _serve_events(self) -> None:
        channel = self.studio.subscribe()
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache, no-transform")
            self.send_header("Connection", "keep-alive")
            self.send_header("X-Accel-Buffering", "no")
            self.end_headers()
            while self.studio.running:
                try:
                    snapshot = channel.get(timeout=12.0)
                    payload = json.dumps(
                        response_envelope(ok=True, state=snapshot), ensure_ascii=False, separators=(",", ":")
                    )
                    self.wfile.write(f"data:{payload}\n\n".encode("utf-8"))
                except queue.Empty:
                    self.wfile.write(b":keepalive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.studio.unsubscribe(channel)

    def _serve_file(self, path: Path, content_type: str, *, cache: str) -> None:
        self._serve_bytes(path.read_bytes(), content_type, cache=cache)

    def _serve_bytes(
        self,
        body: bytes,
        content_type: str,
        *,
        cache: str,
        inline_fallback: bool = False,
    ) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", cache)
        if content_type.startswith("text/html"):
            # The formal UI uses a few data-driven CSS custom properties on
            # elements (pixel swatches/timeline positions). Permit inline
            # styles, while JavaScript remains external-only in normal mode.
            style_src = "'unsafe-inline'" if inline_fallback else "'self' 'unsafe-inline'"
            script_src = "'unsafe-inline'" if inline_fallback else "'self'"
            self.send_header(
                "Content-Security-Policy",
                f"default-src 'self'; style-src {style_src}; script-src {script_src}; connect-src 'self'; "
                "img-src 'self' data:; font-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
            )
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: Any) -> None:
        return


def handler_for(studio: MotionStudio) -> type[StudioHandler]:
    class BoundStudioHandler(StudioHandler):
        pass

    BoundStudioHandler.studio = studio
    return BoundStudioHandler


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ARCS-MINI V2.1 四足动作录制与控制上位机")
    parser.add_argument("--adb", default="adb", help="adb 可执行文件")
    parser.add_argument("--transport", type=int, default=None, help="ADB transport_id；只有一块板时可省略")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8780)
    parser.add_argument("--project", type=Path, default=None, help="可选的项目自动保存 JSON 路径")
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument(
        "--allow-external-power-override",
        action="store_true",
        help="允许页面在 VBAT 预检失败时明确确认外部舵机电源",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    shell = AdbPersistentShell(args.adb, args.transport)
    studio = MotionStudio(
        ServoProtocol(shell),
        ProjectStore(args.project),
        allow_power_override=args.allow_external_power_override,
    )
    studio.start()
    server = ThreadingHTTPServer((args.host, args.port), handler_for(studio))
    server.daemon_threads = True
    url = f"http://{args.host}:{server.server_address[1]}/"
    print(f"{APP_NAME}: {url}", flush=True)
    print("安全默认：已锁定；解锁前会检查 VBAT/servo 并启动 1000 ms 板端看门狗。", flush=True)
    if args.host not in {"127.0.0.1", "localhost", "::1"}:
        print("警告：服务当前对网络开放且无身份认证，只能用于可信隔离网络。", flush=True)
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
        server.shutdown()
        server.server_close()
        studio.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
