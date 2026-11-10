#!/usr/bin/env python3
from __future__ import annotations

import json
import math
import re
import shutil
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path
from typing import Any
from unittest import mock

from tools import quadruped_motion_studio as motion


def reply(*lines: str, elapsed: float = 1.0) -> motion.CommandReply:
    return motion.CommandReply(True, tuple(lines or ("ok",)), elapsed)


class FakeProtocol:
    def __init__(self, *, power_ready: bool = True) -> None:
        self.online = True
        self.transport_id = 17
        self.power_ready = power_ready
        self.calls: list[Any] = []
        self.frames: list[dict[str, Any]] = []
        self.identity_value = "0123456789abcdef"
        self.identity_error: Exception | None = None
        self.imu_error: Exception | None = None
        self.led_calibration_error: Exception | None = None
        self.led_calibration = {
            "version": 1,
            "color_order": "GRB",
            "r_gain_permille": 1000,
            "g_gain_permille": 1000,
            "b_gain_permille": 1000,
            "gamma_x100": 100,
            "persistent": True,
        }
        self.imu_sample = {
            "bus": 0,
            "address": 0x68,
            "who_am_i": 0x68,
            "device_status": "ok",
            "accel": {"x": 0, "y": 0, "z": 16384},
            "gyro": {"x": 0, "y": 0, "z": 0},
            "temp": 0,
        }
        self.state_data = {
            "seq": 0,
            "mode": "idle",
            "active": False,
            "enabled_mask": 0,
            "current_us": [1500] * 4,
            "target_us": [1500] * 4,
            "duration_ms": 0,
            "elapsed_ms": 0,
            "easing": "linear",
            "watchdog_ms": 1000,
            "remaining_ms": 900,
            "expired": False,
        }

    def identity(self):
        self.calls.append("identity")
        if self.identity_error is not None:
            raise self.identity_error
        return self.identity_value

    def power(self):
        self.calls.append("power")
        millivolt = 3800 if self.power_ready else 0
        battery = "present" if self.power_ready else "no-battery"
        return {
            "millivolt": millivolt,
            "servo_millivolt": millivolt,
            "vbat_mv": millivolt,
            "volt": millivolt / 1000,
            "level": "80%" if self.power_ready else "n/a",
            "battery": battery,
            "battery_label": battery,
            "status": battery,
            "present": self.power_ready,
            "servo_power_present": self.power_ready,
            "ready": self.power_ready,
            "checked_at": time.time(),
        }, reply("power", "ok", elapsed=2.0)

    def frame(self, pose):
        self.calls.append("frame")
        self.frames.append(json.loads(json.dumps(pose)))
        self.state_data["seq"] += 1
        self.state_data["target_us"] = list(pose["servos_us"])
        return reply("MOTION_ACK", "ok", elapsed=2.5)

    def servo_frame(self, servos_us, duration_ms, easing="smooth"):
        self.calls.append("servo-frame")
        self.frames.append(
            {
                "servos_us": list(servos_us),
                "duration_ms": duration_ms,
                "easing": easing,
            }
        )
        self.state_data["seq"] += 1
        self.state_data.update(
            {
                "mode": "idle",
                "active": False,
                "enabled_mask": 0x0F,
                "current_us": list(servos_us),
                "target_us": list(servos_us),
                "duration_ms": duration_ms,
                "elapsed_ms": duration_ms,
                "easing": easing,
            }
        )
        return reply("SERVO_ACK action=frame status=0", "ok", elapsed=2.5)

    def servo_test(self, channel, pulse_us, hold_ms):
        self.calls.append(("servo-test", channel, pulse_us, hold_ms))
        self.state_data["seq"] += 1
        current = list(self.state_data["current_us"])
        current[channel] = pulse_us
        self.state_data.update(
            {
                "mode": "idle",
                "active": False,
                "enabled_mask": 0,
                "current_us": current,
                "target_us": list(current),
                "duration_ms": hold_ms,
                "elapsed_ms": hold_ms,
            }
        )
        return reply("SERVO_ACK action=test status=0", "ok", elapsed=float(hold_ms))

    def state(self):
        self.calls.append("state")
        return dict(self.state_data), reply("SERVO_STATE", "ok")

    def imu(self):
        self.calls.append("imu")
        if self.imu_error is not None:
            raise self.imu_error
        return json.loads(json.dumps(self.imu_sample)), reply("mpu6050", "ok", elapsed=4.0)

    def light(self, led):
        self.calls.append(("light", json.loads(json.dumps(led))))
        return reply("RGB_ACK", "ok")

    def led_calibration_status(self):
        self.calls.append("led-calibration-status")
        if self.led_calibration_error is not None:
            raise self.led_calibration_error
        return json.loads(json.dumps(self.led_calibration)), reply("RGB_CAL", "ok")

    def led_calibration_set(self, calibration):
        settings = {
            key: calibration[key]
            for key in (
                "color_order",
                "r_gain_permille",
                "g_gain_permille",
                "b_gain_permille",
                "gamma_x100",
            )
        }
        self.calls.append(("led-calibration-set", json.loads(json.dumps(settings))))
        if self.led_calibration_error is not None:
            raise self.led_calibration_error
        self.led_calibration.update(settings)
        self.led_calibration["persistent"] = True
        return json.loads(json.dumps(self.led_calibration)), reply("RGB_CAL", "ok")

    def led_calibration_reset(self):
        self.calls.append("led-calibration-reset")
        if self.led_calibration_error is not None:
            raise self.led_calibration_error
        self.led_calibration.update(
            {
                "color_order": "GRB",
                "r_gain_permille": 1000,
                "g_gain_permille": 1000,
                "b_gain_permille": 1000,
                "gamma_x100": 100,
                "persistent": True,
            }
        )
        return json.loads(json.dumps(self.led_calibration)), reply("RGB_CAL", "ok")

    def stop_motion(self):
        self.calls.append("stop")
        return reply("SERVO_ACK action=stop status=0", "ok")

    def all_off(self):
        self.calls.append("all-off")
        return reply("SERVO_ACK action=all-off status=0", "ok")

    def watchdog(self, timeout_ms):
        self.calls.append(("watchdog", timeout_ms))
        self.state_data["watchdog_ms"] = timeout_ms
        self.state_data["remaining_ms"] = timeout_ms
        self.state_data["expired"] = False
        return reply("SERVO_ACK action=watchdog status=0", "ok")

    def rgb_release(self):
        self.calls.append("rgb-release")
        return reply("RGB_ACK action=release status=0", "ok")

    def safe_off(self):
        self.calls.extend(["all-off", ("watchdog", 0), "rgb-release"])
        self.state_data.update(
            {
                "mode": "idle",
                "active": False,
                "enabled_mask": 0,
                "duration_ms": 0,
                "elapsed_ms": 0,
                "watchdog_ms": 0,
                "remaining_ms": 0,
                "expired": False,
            }
        )
        return reply("ok")


class FakeShell:
    def __init__(self) -> None:
        self.online = True
        self.transport_id = 8
        self.commands: list[str] = []
        self.timeouts: list[float] = []

    def exchange(self, command: str, timeout: float = 3.0):
        self.commands.append(command)
        self.timeouts.append(timeout)
        if command == "v2hw power":
            return reply("VBAT/servo pin2=3850mV level=75% battery=present", "ok")
        if command == "v2hw servo state":
            return reply(
                "SERVO_STATE status=0 seq=9 mode=frame active=1 enabled=0x0F",
                "SERVO_POSE seq=9 current=1500,1501,1502,1503 target=1600,1601,1602,1603",
                "SERVO_TIMING seq=9 duration_ms=300 elapsed_ms=40 easing=smooth sweep_channel=-1",
                "SERVO_LEASE seq=9 watchdog_ms=1000 watchdog_remaining_ms=811 watchdog_expired=0",
                "ok",
            )
        if command == "v2hw mpu":
            return reply(
                "mpu6050 i2c0/0x68: status=0 (ok) who=0x68 "
                "accel=-123,456,16384 temp=-340 gyro=131,-262,393",
                "ok",
            )
        if command == "v2hw rgb calibration status":
            return reply(
                "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 b_gain=1000 gamma=100 persistent=true",
                "ok",
            )
        if command.startswith("v2hw rgb calibration set "):
            _, _, _, _, order, red, green, blue, gamma = command.split()
            return reply(
                f"RGB_CAL version=1 order={order} r_gain={red} g_gain={green} "
                f"b_gain={blue} gamma={gamma} persistent=1",
                "ok",
            )
        if command == "v2hw rgb calibration reset":
            return reply(
                "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 b_gain=1000 gamma=100 persistent=true",
                "ok",
            )
        return reply("ok")


def wait_until(predicate, timeout: float = 2.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return bool(predicate())


def create_started_studio(protocol: FakeProtocol, **kwargs: Any) -> motion.MotionStudio:
    client_grace_s = float(kwargs.pop("client_grace_s", 30.0))
    result = motion.MotionStudio(protocol, client_grace_s=client_grace_s, **kwargs)
    result.start()
    with result.lock:
        result.online = True
        result.connection_epoch = 1
        result.chip_id = protocol.identity_value
        result._identity_epoch = result.connection_epoch
    return result


def calibration_payload(
    *,
    chip_id: str = "0123456789abcdef",
    home_pose_us: list[int] | None = None,
    imu_baseline: dict[str, float] | None = None,
) -> dict[str, Any]:
    return {
        "chip_id": chip_id,
        "board": motion.BOARD_NAME,
        "mounting_revision": 1,
        "servo_calibration": [
            {
                "channel": index,
                "min_us": 1000 + index * 10,
                "center_us": 1500,
                "max_us": 2000 - index * 10,
                "reverse": index % 2 == 1,
                "trim_deg": [-3, 2, 0, 5][index],
            }
            for index in range(motion.SERVO_COUNT)
        ],
        "home_pose_us": home_pose_us,
        "imu_mounting_baseline_deg": imu_baseline,
    }


def establish_test_identity(studio: motion.MotionStudio, chip_id: str = "0123456789abcdef") -> int:
    """Mark the manually-started fake transport as actively identified."""
    with studio.lock:
        if studio.connection_epoch == 0:
            studio.connection_epoch = 1
        studio.chip_id = chip_id
        studio._identity_epoch = studio.connection_epoch
        return studio.connection_epoch


def calibration_api_payload(studio: motion.MotionStudio, **kwargs: Any) -> dict[str, Any]:
    payload = calibration_payload(**kwargs)
    payload["connection_epoch"] = studio.connection_epoch
    return payload


def led_calibration_api_payload(studio: motion.MotionStudio, **overrides: Any) -> dict[str, Any]:
    payload = {
        "connection_epoch": studio.connection_epoch,
        "chip_id": studio.chip_id,
        "board": studio.board,
        "color_order": "GRB",
        "r_gain_permille": 820,
        "g_gain_permille": 640,
        "b_gain_permille": 910,
        "gamma_x100": 220,
    }
    payload.update(overrides)
    return payload


def servo_test_api_payload(studio: motion.MotionStudio, **overrides: Any) -> dict[str, Any]:
    payload = {
        "connection_epoch": studio.connection_epoch,
        "chip_id": studio.chip_id,
        "board": studio.board,
        "channel": 2,
        "pulse_us": 1420,
        "hold_ms": 250,
        "safety_confirmed": True,
    }
    payload.update(overrides)
    return payload


def posture_test_api_payload(studio: motion.MotionStudio, **overrides: Any) -> dict[str, Any]:
    payload = {
        "connection_epoch": studio.connection_epoch,
        "chip_id": studio.chip_id,
        "board": studio.board,
        "servos_us": [1750, 900, 1750, 1250],
        "duration_ms": motion.POSTURE_TEST_MIN_DURATION_MS,
        "safety_confirmed": True,
    }
    payload.update(overrides)
    return payload


def install_posture_test_calibration(studio: motion.MotionStudio) -> None:
    """Give the fake device the fixture's identity-bound 900..2100 us limits."""
    record = calibration_payload(home_pose_us=[2000, 1000, 2000, 1000])
    for channel in record["servo_calibration"]:
        channel.update({"min_us": 900, "center_us": 1500, "max_us": 2100})
    studio.store.put_calibration(record)
    studio.protocol.state_data.update(
        {
            "active": False,
            "enabled_mask": 0,
            "watchdog_ms": 0,
            "remaining_ms": 0,
            "expired": False,
        }
    )
    with studio.lock:
        studio.servo_state = json.loads(json.dumps(studio.protocol.state_data))
        studio._safety_off_pending = False


class ValidationTest(unittest.TestCase):
    def test_chip_identity_parser_requires_one_unambiguous_16_hex_value(self) -> None:
        self.assertEqual(
            motion.parse_chip_id_output("device_id: 0123456789ABCDEF\nok\n"),
            "0123456789abcdef",
        )
        with self.assertRaises(motion.ValidationError):
            motion.parse_chip_id_output("no identity")
        with self.assertRaises(motion.ValidationError):
            motion.parse_chip_id_output("0123456789abcdef fedcba9876543210")
        with self.assertRaises(motion.ValidationError):
            motion.parse_chip_id_output("0123456789abcdef\x00")

    def test_default_profile_uses_conservative_soft_limits(self) -> None:
        profile = motion.default_device_profile()
        self.assertEqual([item["min_us"] for item in profile["servos"]], [1000] * 4)
        self.assertEqual([item["center_us"] for item in profile["servos"]], [1500] * 4)
        self.assertEqual([item["max_us"] for item in profile["servos"]], [2000] * 4)

    def test_servo_electrical_boundaries_accept_edges_and_reject_neighbors(self) -> None:
        self.assertEqual((motion.SERVO_MIN_US, motion.SERVO_MAX_US), (500, 2500))
        calibration = calibration_payload()["servo_calibration"]
        for servo in calibration:
            servo["min_us"] = motion.SERVO_MIN_US
            servo["center_us"] = motion.SERVO_CENTER_US
            servo["max_us"] = motion.SERVO_MAX_US

        normalized = motion.normalize_servo_calibration(calibration)
        self.assertEqual(
            [(servo["min_us"], servo["max_us"]) for servo in normalized],
            [(motion.SERVO_MIN_US, motion.SERVO_MAX_US)] * motion.SERVO_COUNT,
        )

        outside_edges = (
            ("min_us", motion.SERVO_MIN_US - 1),
            ("max_us", motion.SERVO_MAX_US + 1),
        )
        for field, value in outside_edges:
            invalid = json.loads(json.dumps(calibration))
            invalid[0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(motion.ValidationError):
                motion.normalize_servo_calibration(invalid)

    def test_snapshot_reports_hard_limits_without_widening_default_soft_limits(self) -> None:
        limits = motion.MotionStudio(FakeProtocol()).snapshot()["servo_limits"]
        self.assertEqual((limits["hard_min_us"], limits["hard_max_us"]), (500, 2500))
        self.assertEqual((limits["min_us"], limits["max_us"]), (1000, 2000))
        self.assertEqual(
            [(servo["min_us"], servo["max_us"]) for servo in limits["channels"]],
            [(1000, 2000)] * motion.SERVO_COUNT,
        )

    def test_pose_is_clamped_to_calibration_and_hard_limits(self) -> None:
        profile = motion.default_device_profile()
        profile["servos"][1]["min_us"] = 1100
        profile["servos"][2]["max_us"] = 1900
        pose = motion.normalize_pose(
            {
                "servos_us": [-1, 1001, 2444, 9000],
                "duration_ms": 120,
                "easing": "linear",
                "led": {"enabled": True, "color": "#12abEF", "brightness": 55, "pixel_count": 12},
            },
            profile,
        )
        self.assertEqual(pose["servos_us"], [1000, 1100, 1900, 2000])
        self.assertEqual(pose["led"]["color"], "#12ABEF")

    def test_pose_rejects_wrong_shape_and_bad_easing(self) -> None:
        with self.assertRaises(motion.ValidationError):
            motion.normalize_pose({"servos_us": [1500] * 3})
        with self.assertRaises(motion.ValidationError):
            motion.normalize_pose({"servos_us": [1500] * 4, "easing": "bounce"})

    def test_action_requires_strictly_increasing_timestamps(self) -> None:
        frame = {
            "time_ms": 10,
            "duration_ms": 20,
            "easing": "linear",
            "servos_us": [1500] * 4,
            "led": motion.default_led(),
        }
        with self.assertRaises(motion.ValidationError):
            motion.normalize_action(
                {"name": "bad", "keyframes": [dict(frame), dict(frame)]}, motion.default_device_profile()
            )

    def test_led_limits_are_strict(self) -> None:
        with self.assertRaises(motion.ValidationError):
            motion.normalize_led({"enabled": True, "color": "red", "brightness": 20, "pixel_count": 1})
        with self.assertRaises(motion.ValidationError):
            motion.normalize_led({"enabled": True, "color": "#FFFFFF", "brightness": 20, "pixel_count": 65})

    def test_led_defaults_to_the_eight_pixel_board(self) -> None:
        self.assertEqual(motion.LED_DEFAULT_COUNT, 8)
        self.assertEqual(motion.default_led()["pixel_count"], 8)
        self.assertEqual(motion.default_device_profile()["led"]["pixel_count"], 8)

    def test_led_calibration_parser_requires_complete_bounded_single_line(self) -> None:
        parsed = motion.parse_led_calibration_line(
            "RGB_CAL version=1 order=RBG r_gain=900 g_gain=750 b_gain=1000 gamma=220 persistent=1"
        )
        self.assertEqual(
            parsed,
            {
                "version": 1,
                "color_order": "RBG",
                "r_gain_permille": 900,
                "g_gain_permille": 750,
                "b_gain_permille": 1000,
                "gamma_x100": 220,
                "persistent": True,
            },
        )
        for token, expected in (("true", True), ("1", True), ("false", False), ("0", False)):
            with self.subTest(persistent=token):
                self.assertIs(
                    motion.parse_led_calibration_line(
                        "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 "
                        f"b_gain=1000 gamma=100 persistent={token}"
                    )["persistent"],
                    expected,
                )
        for invalid in (
            "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 b_gain=1000 gamma=100",
            "RGB_CAL version=1 order=XYZ r_gain=1000 g_gain=1000 b_gain=1000 gamma=100 persistent=true",
            "RGB_CAL version=1 order=GRB r_gain=1001 g_gain=1000 b_gain=1000 gamma=100 persistent=true",
            "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 b_gain=1000 gamma=99 persistent=true",
            "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 b_gain=1000 gamma=100 persistent=true\x00",
        ):
            with self.subTest(invalid=invalid), self.assertRaises(motion.ValidationError):
                motion.parse_led_calibration_line(invalid)

    def test_led_calibration_request_is_identity_bound_and_strict(self) -> None:
        studio = motion.MotionStudio(FakeProtocol())
        studio.connection_epoch = 4
        studio.chip_id = "0123456789abcdef"
        valid = led_calibration_api_payload(studio, color_order="bgr", r_gain_permille=0, gamma_x100=300)
        normalized = motion.normalize_led_calibration_request(valid)
        self.assertEqual(normalized["color_order"], "BGR")
        self.assertEqual(normalized["r_gain_permille"], 0)
        self.assertEqual(normalized["gamma_x100"], 300)
        self.assertFalse(normalized["reset"])

        reset = motion.normalize_led_calibration_request(
            {
                "connection_epoch": 4,
                "chip_id": "0123456789ABCDEF",
                "board": motion.BOARD_NAME,
                "reset": True,
            }
        )
        self.assertTrue(reset["reset"])
        self.assertEqual(reset["chip_id"], "0123456789abcdef")

        invalid_requests = []
        missing = dict(valid)
        missing.pop("gamma_x100")
        invalid_requests.append(missing)
        unknown = dict(valid, extra=True)
        invalid_requests.append(unknown)
        invalid_requests.append(dict(valid, r_gain_permille=1001))
        invalid_requests.append(dict(valid, gamma_x100=99))
        invalid_requests.append(dict(valid, connection_epoch=4.0))
        invalid_requests.append(dict(valid, color_order="XYZ"))
        invalid_requests.append(
            {
                "connection_epoch": 4,
                "chip_id": "0123456789abcdef",
                "board": motion.BOARD_NAME,
                "reset": False,
            }
        )
        for invalid in invalid_requests:
            with self.subTest(invalid=invalid), self.assertRaises(motion.ValidationError):
                motion.normalize_led_calibration_request(invalid)

    def test_servo_test_request_is_strict_identity_bound_and_narrow(self) -> None:
        studio = motion.MotionStudio(FakeProtocol())
        studio.connection_epoch = 4
        studio.chip_id = "0123456789abcdef"
        valid = servo_test_api_payload(
            studio,
            connection_epoch=4,
            pulse_us=1580,
            hold_ms=50,
        )
        profile = motion.default_device_profile()
        normalized = motion.normalize_servo_test_request(valid, profile)
        self.assertEqual(normalized["channel"], 2)
        self.assertEqual(normalized["pulse_us"], 1580)
        self.assertEqual(normalized["hold_ms"], 50)
        self.assertEqual(
            (normalized["safe_min_us"], normalized["safe_max_us"]),
            (
                max(motion.SERVO_TEST_MIN_US, profile["servos"][2]["min_us"]),
                min(motion.SERVO_TEST_MAX_US, profile["servos"][2]["max_us"]),
            ),
        )

        defaults = dict(valid)
        defaults.pop("pulse_us")
        defaults.pop("hold_ms")
        normalized = motion.normalize_servo_test_request(defaults, motion.default_device_profile())
        self.assertEqual(normalized["pulse_us"], motion.SERVO_CENTER_US)
        self.assertEqual(normalized["hold_ms"], motion.SERVO_TEST_DEFAULT_HOLD_MS)

        invalid_requests = [
            dict(valid, safety_confirmed=False),
            dict(valid, pulse_us=motion.SERVO_TEST_MIN_US - 1),
            dict(valid, pulse_us=motion.SERVO_TEST_MAX_US + 1),
            dict(valid, hold_ms=motion.SERVO_TEST_MAX_HOLD_MS + 1),
            dict(valid, channel=4),
            dict(valid, channel=2.0),
            dict(valid, extra=True),
        ]
        for invalid in invalid_requests:
            with self.subTest(invalid=invalid), self.assertRaises(motion.ValidationError):
                motion.normalize_servo_test_request(invalid, motion.default_device_profile())

    def test_calibration_rejects_yaw_unknown_fields_and_non_integer_pulses(self) -> None:
        payload = calibration_payload(imu_baseline={"roll": 1.5, "pitch": -2.0})
        payload["imu_mounting_baseline_deg"]["yaw"] = 0.0
        with self.assertRaisesRegex(motion.ValidationError, "yaw"):
            motion.normalize_calibration_record(payload)
        payload = calibration_payload()
        payload["servo_calibration"][0]["center_us"] = 1500.5
        with self.assertRaises(motion.ValidationError):
            motion.normalize_calibration_record(payload)
        payload = calibration_payload()
        payload["unexpected"] = True
        with self.assertRaisesRegex(motion.ValidationError, "unexpected"):
            motion.normalize_calibration_record(payload)
        payload = calibration_payload(imu_baseline={"roll": "1.0", "pitch": 0.0})  # type: ignore[dict-item]
        with self.assertRaises(motion.ValidationError):
            motion.normalize_calibration_record(payload)


class ProjectStoreTest(unittest.TestCase):
    def test_crud_and_atomic_round_trip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "walk.arcsmotion.json"
            store = motion.ProjectStore(path)
            action = store.create_action(
                {
                    "name": "起步",
                    "keyframes": [
                        {
                            "time_ms": 0,
                            "duration_ms": 200,
                            "easing": "smooth",
                            "servos_us": [1400, 1600, 1400, 1600],
                            "led": {"enabled": True, "color": "#00CCFF", "brightness": 30, "pixel_count": 8},
                        }
                    ],
                }
            )
            self.assertTrue(path.is_file())
            loaded = motion.ProjectStore(path)
            self.assertEqual(loaded.get_action(action["id"])["name"], "起步")
            changed = dict(action)
            changed["name"] = "起步 2"
            loaded.update_action(action["id"], changed)
            self.assertEqual(loaded.get_action(action["id"])["name"], "起步 2")
            loaded.delete_action(action["id"])
            self.assertEqual(loaded.snapshot()["actions"], [])

    def test_import_rejects_unknown_format(self) -> None:
        store = motion.ProjectStore()
        bad = store.snapshot()
        bad["format"] = "other"
        with self.assertRaises(motion.ValidationError):
            store.replace(bad)

    def test_partial_calibration_round_trip_preserves_trim_and_progress(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "calibration.arcsmotion.json"
            store = motion.ProjectStore(path)
            first = store.put_calibration(calibration_payload())
            self.assertIsNone(first["home_pose_us"])
            self.assertIsNone(first["imu_mounting_baseline_deg"])
            self.assertEqual(first["servo_calibration"][0]["trim_deg"], -3)
            self.assertFalse(any(item.name.endswith(".tmp") for item in path.parent.iterdir()))

            restored = motion.ProjectStore(path)
            second_payload = restored.get_calibration("0123456789ABCDEF")
            assert second_payload is not None
            second_payload["home_pose_us"] = [1500, 1510, 1490, 1520]
            restored.put_calibration(second_payload)

            final = motion.ProjectStore(path).get_calibration("0123456789abcdef")
            assert final is not None
            self.assertEqual(final["home_pose_us"], [1500, 1510, 1490, 1520])
            self.assertIsNone(final["imu_mounting_baseline_deg"])
            self.assertEqual(
                motion.ProjectStore(path).snapshot()["device_calibrations"]["format"],
                motion.CALIBRATION_FORMAT,
            )

    def test_legacy_project_replace_keeps_local_device_calibrations(self) -> None:
        store = motion.ProjectStore()
        store.put_calibration(calibration_payload())
        legacy = motion.default_project()
        legacy.pop("device_calibrations")
        store.replace(legacy)
        self.assertIsNotNone(store.get_calibration("0123456789abcdef"))

    def test_failed_atomic_replace_keeps_previous_file_and_memory_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "safe.arcsmotion.json"
            store = motion.ProjectStore(path)
            original = store.put_calibration(calibration_payload())
            original_bytes = path.read_bytes()
            changed = dict(original)
            changed["home_pose_us"] = [1500, 1510, 1490, 1520]
            with mock.patch.object(motion.os, "replace", side_effect=OSError("disk failure")):
                with self.assertRaises(OSError):
                    store.put_calibration(changed)
            self.assertEqual(path.read_bytes(), original_bytes)
            self.assertIsNone(store.get_calibration("0123456789abcdef")["home_pose_us"])
            self.assertFalse(any(item.name.endswith(".tmp") for item in path.parent.iterdir()))


class ProtocolTest(unittest.TestCase):
    def test_persistent_shell_accepts_self_framed_rgb_calibration_reply(self) -> None:
        shell = motion.AdbPersistentShell(adb="adb")
        process = mock.Mock()
        process.poll.return_value = None
        process.stdin = mock.Mock()
        shell._process = process
        shell._connected.set()
        responses = mock.Mock()
        responses.get_nowait.side_effect = motion.queue.Empty
        responses.get.return_value = (
            "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 "
            "b_gain=1000 gamma=100 persistent=true"
        )
        shell._responses = responses

        result = shell.exchange("v2hw rgb calibration status", timeout=0.1)

        self.assertTrue(result.ok)
        self.assertEqual(len(result.lines), 1)
        process.stdin.write.assert_called_once_with("v2hw rgb calibration status\n")

    def test_adb_identity_uses_transport_bound_one_shot_selector(self) -> None:
        shell = motion.AdbPersistentShell(adb="/opt/adb")
        selected = {"transport_id": 42, "serial": "serial", "usb": "1-2"}
        completed = mock.Mock(returncode=0, stdout="device id=0123456789ABCDEF\n")
        with mock.patch.object(motion.subprocess, "run", return_value=completed) as run:
            self.assertEqual(shell._probe_identity(selected, timeout=1.25), "0123456789abcdef")
        self.assertEqual(
            run.call_args.args[0],
            ["/opt/adb", "-t", "42", "shell", "device", "get_device_id"],
        )
        self.assertEqual(run.call_args.kwargs["timeout"], 1.25)
        self.assertFalse(run.call_args.kwargs["check"])

    def test_adb_identity_reports_timeout_and_cached_transport_change(self) -> None:
        shell = motion.AdbPersistentShell(adb="adb")
        selected = {"transport_id": 7, "serial": "serial", "usb": "1-2"}
        timeout_error = motion.subprocess.TimeoutExpired(cmd="adb", timeout=0.01)
        with mock.patch.object(motion.subprocess, "run", side_effect=timeout_error):
            with self.assertRaisesRegex(motion.StudioError, "chip_id"):
                shell._probe_identity(selected, timeout=0.01)

        shell.transport_id = 7
        shell._identity_transport_id = 7
        shell._cached_chip_id = "0123456789abcdef"
        shell._connected.set()
        with mock.patch.object(motion.subprocess, "run") as run:
            self.assertEqual(shell.identity(), "0123456789abcdef")
            run.assert_not_called()
        shell.transport_id = 8
        with self.assertRaises(motion.DeviceOfflineError):
            shell.identity()

    def test_connection_manager_probes_identity_before_opening_persistent_shell(self) -> None:
        shell = motion.AdbPersistentShell(adb="adb")
        shell._running = True
        selected = {"transport_id": 11, "serial": "serial", "usb": "2-3"}
        order: list[str] = []

        def probe(item: dict[str, Any]) -> str:
            self.assertIs(item, selected)
            order.append("identity-one-shot")
            return "0123456789abcdef"

        def open_persistent(item: dict[str, Any], chip_id: str) -> None:
            self.assertIs(item, selected)
            self.assertEqual(chip_id, "0123456789abcdef")
            order.append("persistent-popen")
            shell._running = False

        with (
            mock.patch.object(motion, "discover_transports", return_value=[selected]),
            mock.patch.object(shell, "_probe_identity", side_effect=probe),
            mock.patch.object(shell, "_open", side_effect=open_persistent),
        ):
            shell._connection_loop()
        self.assertEqual(order, ["identity-one-shot", "persistent-popen"])

    def test_persistent_open_binds_preprobed_identity_without_second_shell(self) -> None:
        shell = motion.AdbPersistentShell(adb="/opt/adb")
        selected = {"transport_id": 13, "serial": "serial-13", "usb": "3-4"}
        process = mock.Mock()
        reader = mock.Mock()
        with (
            mock.patch.object(motion.subprocess, "Popen", return_value=process) as popen,
            mock.patch.object(motion.threading, "Thread", return_value=reader),
            mock.patch.object(motion.subprocess, "run") as run,
        ):
            shell._open(selected, "0123456789abcdef")
            self.assertEqual(shell.identity(), "0123456789abcdef")
            run.assert_not_called()
        self.assertEqual(
            popen.call_args.args[0],
            ["/opt/adb", "-t", "13", "shell"],
        )
        reader.start.assert_called_once_with()
        self.assertEqual(shell._identity_transport_id, 13)

    def test_identity_cache_missing_or_transport_mismatch_is_rejected(self) -> None:
        shell = motion.AdbPersistentShell(adb="adb")
        shell.transport_id = 7
        shell._connected.set()
        with self.assertRaises(motion.DeviceOfflineError):
            shell.identity()
        shell._cached_chip_id = "0123456789abcdef"
        shell._identity_transport_id = 8
        with self.assertRaises(motion.DeviceOfflineError):
            shell.identity()

    def test_combined_motion_frame_contains_led_and_four_servos(self) -> None:
        shell = FakeShell()
        protocol = motion.ServoProtocol(shell)
        protocol.frame(
            {
                "servos_us": [1100, 1300, 1700, 1900],
                "duration_ms": 400,
                "easing": "smooth",
                "led": {"enabled": True, "color": "#12ABEF", "brightness": 44, "pixel_count": 9},
            }
        )
        self.assertEqual(
            shell.commands[-1],
            "v2hw motion frame 400 smooth 1100 1300 1700 1900 9 18 171 239 44",
        )

    def test_locked_posture_frame_is_one_servo_only_four_channel_command(self) -> None:
        shell = FakeShell()
        protocol = motion.ServoProtocol(shell)

        protocol.servo_frame([1750, 900, 1750, 1250], 800, "smooth")

        self.assertEqual(
            shell.commands,
            ["v2hw servo frame 800 smooth 1750 900 1750 1250"],
        )
        self.assertEqual(shell.timeouts, [3.0])

    def test_bounded_servo_test_uses_single_channel_firmware_command(self) -> None:
        shell = FakeShell()
        protocol = motion.ServoProtocol(shell)
        protocol.servo_test(2, 1420, 250)
        self.assertEqual(shell.commands, ["v2hw servo test 2 1420 250"])
        self.assertEqual(shell.timeouts, [2.0])
        for args in (
            (4, 1500, 250),
            (2, motion.SERVO_TEST_MIN_US - 1, 250),
            (2, 1500, 1001),
        ):
            with self.subTest(args=args), self.assertRaises(motion.ValidationError):
                protocol.servo_test(*args)

    def test_disabled_led_is_transmitted_as_black(self) -> None:
        shell = FakeShell()
        protocol = motion.ServoProtocol(shell)
        pose = motion.default_pose()
        pose["led"] = {"enabled": False, "color": "#FFFFFF", "brightness": 99, "pixel_count": 4}
        protocol.frame(pose)
        self.assertTrue(shell.commands[-1].endswith("4 0 0 0 0"))

    def test_independent_light_uses_rgb_strip_only(self) -> None:
        shell = FakeShell()
        protocol = motion.ServoProtocol(shell)
        protocol.light({"enabled": True, "color": "#0102FF", "brightness": 31, "pixel_count": 7})
        self.assertEqual(shell.commands, ["v2hw rgb strip 7 1 2 255 31"])

    def test_led_calibration_protocol_status_set_and_reset_commands(self) -> None:
        shell = FakeShell()
        protocol = motion.ServoProtocol(shell)
        initial, _ = protocol.led_calibration_status()
        changed, _ = protocol.led_calibration_set(
            {
                "color_order": "BGR",
                "r_gain_permille": 810,
                "g_gain_permille": 620,
                "b_gain_permille": 990,
                "gamma_x100": 230,
            }
        )
        restored, _ = protocol.led_calibration_reset()
        self.assertEqual(
            shell.commands,
            [
                "v2hw rgb calibration status",
                "v2hw rgb calibration set BGR 810 620 990 230",
                "v2hw rgb calibration reset",
            ],
        )
        self.assertEqual(initial["color_order"], "GRB")
        self.assertEqual(changed["color_order"], "BGR")
        self.assertEqual(changed["g_gain_permille"], 620)
        self.assertEqual(changed["gamma_x100"], 230)
        self.assertTrue(changed["persistent"])
        self.assertEqual(restored["r_gain_permille"], 1000)

    def test_led_calibration_protocol_rejects_missing_or_ambiguous_status(self) -> None:
        for response in (
            reply("ok"),
            reply(
                "RGB_CAL version=1 order=GRB r_gain=1000 g_gain=1000 b_gain=1000 gamma=100 persistent=true",
                "RGB_CAL version=1 order=BGR r_gain=1000 g_gain=1000 b_gain=1000 gamma=100 persistent=true",
                "ok",
            ),
        ):
            shell = FakeShell()
            with mock.patch.object(shell, "exchange", return_value=response):
                with self.assertRaises(motion.ValidationError):
                    motion.ServoProtocol(shell).led_calibration_status()

    def test_safe_off_order(self) -> None:
        shell = FakeShell()
        protocol = motion.ServoProtocol(shell)
        protocol.safe_off()
        self.assertEqual(
            shell.commands,
            ["v2hw servo all-off", "v2hw servo watchdog 0", "v2hw rgb release"],
        )

    def test_power_and_extended_state_parser(self) -> None:
        protocol = motion.ServoProtocol(FakeShell())
        power, _ = protocol.power()
        state, _ = protocol.state()
        self.assertTrue(power["ready"])
        self.assertEqual(state["target_us"], [1600, 1601, 1602, 1603])
        self.assertEqual(state["watchdog_ms"], 1000)
        self.assertEqual(state["remaining_ms"], 811)
        self.assertFalse(state["expired"])

    def test_imu_uses_actual_read_only_command_and_parses_signed_raw_values(self) -> None:
        shell = FakeShell()
        sample, result = motion.ServoProtocol(shell).imu()
        self.assertEqual(shell.commands, ["v2hw mpu"])
        self.assertEqual(shell.timeouts, [motion.IMU_COMMAND_TIMEOUT_S])
        self.assertTrue(result.ok)
        self.assertEqual(sample["accel"], {"x": -123, "y": 456, "z": 16384})
        self.assertEqual(sample["gyro"], {"x": 131, "y": -262, "z": 393})
        self.assertEqual(sample["temp"], -340)
        self.assertEqual(sample["who_am_i"], 0x68)

    def test_imu_parser_rejects_truncated_wrong_identity_and_out_of_range_data(self) -> None:
        bad_lines = (
            "mpu6050 i2c0/0x68: status=0 (ok) who=0x68 accel=0,0,16384 temp=0 gyro=0,0",
            "mpu6050 i2c0/0x68: status=0 (ok) who=0x69 accel=0,0,16384 temp=0 gyro=0,0,0",
            "mpu6050 i2c0/0x68: status=0 (ok) who=0x68 accel=0,0,40000 temp=0 gyro=0,0,0",
        )
        for line in bad_lines:
            with self.subTest(line=line), self.assertRaises(motion.StudioError):
                motion.parse_mpu_reply(reply(line, "ok"))

    def test_imu_parser_ignores_unrelated_shell_output(self) -> None:
        parsed = motion.parse_mpu_reply(
            reply(
                "unrelated asynchronous log",
                "mpu6050 i2c0/0x68: status=0 (ok) who=0x68 "
                "accel=1, 2, 3 temp=4 gyro=-5, -6, -7",
                "ok",
            )
        )
        self.assertEqual(parsed["accel"], {"x": 1, "y": 2, "z": 3})
        self.assertEqual(parsed["gyro"], {"x": -5, "y": -6, "z": -7})

    def test_truncated_legacy_state_with_nul_is_never_accepted(self) -> None:
        class TruncatedShell(FakeShell):
            def exchange(self, command: str, timeout: float = 3.0):
                del command, timeout
                return reply(
                    "SERVO_STATE seq=11 mode=frame active=1 enabled=0x0f "
                    "current=1500,1500,1500,1500 target=1600,1600,1600,1600 duration_ms=300 elapsed_\x00",
                    "ok",
                )

        state, _ = motion.ServoProtocol(TruncatedShell()).state()
        self.assertIsNone(state)

    def test_segmented_state_requires_all_lines_with_same_sequence(self) -> None:
        class MissingLeaseShell(FakeShell):
            def exchange(self, command: str, timeout: float = 3.0):
                del command, timeout
                return reply(
                    "SERVO_STATE status=0 seq=4 mode=idle active=0 enabled=0x00",
                    "SERVO_POSE seq=4 current=1500,1500,1500,1500 target=1500,1500,1500,1500",
                    "SERVO_TIMING seq=5 duration_ms=0 elapsed_ms=0 easing=linear sweep_channel=-1",
                    "ok",
                )

        state, _ = motion.ServoProtocol(MissingLeaseShell()).state()
        self.assertIsNone(state)


class ImuComplementaryFilterTest(unittest.TestCase):
    @staticmethod
    def sample(*, accel=(0, 0, 16384), gyro=(0, 0, 0)) -> dict[str, Any]:
        return {
            "accel": {"x": accel[0], "y": accel[1], "z": accel[2]},
            "gyro": {"x": gyro[0], "y": gyro[1], "z": gyro[2]},
        }

    @staticmethod
    def roll_sample(angle_deg: float) -> dict[str, Any]:
        radians = math.radians(angle_deg)
        return ImuComplementaryFilterTest.sample(
            accel=(0, round(math.sin(radians) * 16384), round(math.cos(radians) * 16384))
        )

    def test_level_initialization_and_relative_yaw_integration(self) -> None:
        attitude_filter = motion.ImuComplementaryFilter()
        first = attitude_filter.update(self.sample(), monotonic_s=10.0)
        second = attitude_filter.update(self.sample(gyro=(0, 0, 131)), monotonic_s=11.0)
        self.assertAlmostEqual(first["roll_deg"], 0.0, places=3)
        self.assertAlmostEqual(first["pitch_deg"], 0.0, places=3)
        self.assertAlmostEqual(second["yaw_deg"], 1.0, places=3)
        self.assertFalse(second["yaw_absolute"])
        self.assertTrue(second["yaw_drift_expected"])

    def test_accelerometer_sets_roll_and_long_gap_is_not_integrated(self) -> None:
        attitude_filter = motion.ImuComplementaryFilter()
        first = attitude_filter.update(self.sample(accel=(0, 16384, 0)), monotonic_s=1.0)
        after_gap = attitude_filter.update(
            self.sample(accel=(0, 0, 16384), gyro=(0, 0, 13100)), monotonic_s=4.0
        )
        self.assertAlmostEqual(first["roll_deg"], 90.0, places=3)
        self.assertTrue(after_gap["filter_gap_reset"])
        self.assertAlmostEqual(after_gap["roll_deg"], 0.0, places=3)
        self.assertAlmostEqual(after_gap["yaw_deg"], 0.0, places=3)

    def test_wrapped_fusion_takes_short_arc_across_both_180_degree_directions(self) -> None:
        self.assertAlmostEqual(motion._fuse_wrapped_degrees(179.0, -179.0, 0.75), 179.5)
        self.assertAlmostEqual(motion._fuse_wrapped_degrees(-179.0, 179.0, 0.75), -179.5)

        positive = motion.ImuComplementaryFilter()
        positive.update(self.roll_sample(179.0), monotonic_s=1.0)
        toward_negative = positive.update(self.roll_sample(-179.0), monotonic_s=1.1)
        self.assertGreater(toward_negative["roll_deg"], 179.0)

        negative = motion.ImuComplementaryFilter()
        negative.update(self.roll_sample(-179.0), monotonic_s=1.0)
        toward_positive = negative.update(self.roll_sample(179.0), monotonic_s=1.1)
        self.assertLess(toward_positive["roll_deg"], -179.0)

    def test_boundary_noise_never_blends_roll_through_zero_and_state_stays_wrapped(self) -> None:
        attitude_filter = motion.ImuComplementaryFilter()
        outputs = [attitude_filter.update(self.roll_sample(179.0), monotonic_s=1.0)]
        for index, angle in enumerate((-179.0, 179.0, -179.0, 179.0, -179.0), start=1):
            outputs.append(attitude_filter.update(self.roll_sample(angle), monotonic_s=1.0 + index * 0.1))
        self.assertTrue(all(abs(sample["roll_deg"]) > 170.0 for sample in outputs))

        wrapped = attitude_filter.update(
            self.sample(accel=(0, 0, 32767), gyro=(26200, 26200, 26200)),
            monotonic_s=2.0,
        )
        self.assertTrue(all(-180.0 <= wrapped[key] < 180.0 for key in ("roll_deg", "pitch_deg", "yaw_deg")))
        self.assertTrue(-180.0 <= attitude_filter._roll_deg < 180.0)
        self.assertTrue(-180.0 <= attitude_filter._pitch_deg < 180.0)


class MotionStudioTest(unittest.TestCase):
    def tearDown(self) -> None:
        studio = getattr(self, "studio", None)
        if studio is not None:
            studio.shutdown()

    def test_connection_actively_identifies_chip_and_identity_failure_stays_unusable(self) -> None:
        protocol = FakeProtocol()
        self.studio = motion.MotionStudio(protocol)
        self.studio.on_connection(True)
        self.assertTrue(wait_until(lambda: self.studio.snapshot()["identity_valid"]))
        state = self.studio.snapshot()
        self.assertEqual(state["connection_epoch"], 1)
        self.assertEqual(state["identity_epoch"], 1)
        self.assertEqual(state["chip_id"], protocol.identity_value)
        self.assertIn("identity", protocol.calls)

        failed_protocol = FakeProtocol()
        failed_protocol.identity_error = motion.StudioError("identity command failed")
        failed = motion.MotionStudio(failed_protocol)
        failed.on_connection(True)
        self.assertTrue(wait_until(lambda: failed.error is not None))
        failed.on_device_line("Chip ID: fedcba9876543210")
        failed_state = failed.snapshot()
        self.assertFalse(failed_state["identity_valid"])
        self.assertIsNone(failed_state["chip_id"])
        self.assertEqual(failed.calibration_snapshot()["reason"], "chip_id_unavailable")

    def test_reconnect_changes_epoch_and_stale_epoch_cannot_save_calibration(self) -> None:
        protocol = FakeProtocol()
        self.studio = motion.MotionStudio(protocol)
        self.studio.on_connection(True)
        self.assertTrue(wait_until(lambda: self.studio.snapshot()["identity_valid"]))
        first_epoch = self.studio.connection_epoch
        stale_request = calibration_api_payload(self.studio)

        protocol.online = False
        self.studio.on_connection(False, "USB gone")
        self.assertEqual(self.studio.connection_epoch, first_epoch + 1)
        self.assertIsNone(self.studio.chip_id)
        protocol.online = True
        self.studio.on_connection(True)
        self.assertTrue(wait_until(lambda: self.studio.snapshot()["identity_valid"]))
        self.assertEqual(self.studio.connection_epoch, first_epoch + 2)
        with self.assertRaisesRegex(motion.ValidationError, "connection_epoch"):
            self.studio.save_calibration(stale_request)

    def test_imu_read_started_before_reconnect_is_discarded(self) -> None:
        class BlockingImuProtocol(FakeProtocol):
            def __init__(self) -> None:
                super().__init__()
                self.imu_started = threading.Event()
                self.imu_release = threading.Event()

            def imu(self):
                self.calls.append("imu")
                self.imu_started.set()
                if not self.imu_release.wait(2.0):
                    raise motion.StudioError("test IMU release timeout")
                return json.loads(json.dumps(self.imu_sample)), reply("mpu6050", "ok", elapsed=4.0)

        protocol = BlockingImuProtocol()
        self.studio = motion.MotionStudio(protocol)
        self.studio.on_connection(True)
        self.assertTrue(wait_until(lambda: self.studio.snapshot()["identity_valid"]))
        old_epoch = self.studio.connection_epoch
        worker = threading.Thread(target=lambda: self.studio.poll_imu_once(force=True))
        worker.start()
        self.assertTrue(protocol.imu_started.wait(1.0))
        protocol.online = False
        self.studio.on_connection(False, "reconnect race")
        protocol.online = True
        self.studio.on_connection(True)
        self.assertTrue(wait_until(lambda: self.studio.snapshot()["identity_valid"]))
        protocol.imu_release.set()
        worker.join(timeout=2.0)
        self.assertFalse(worker.is_alive())
        imu = self.studio.imu_snapshot()
        self.assertEqual(imu["sample_seq"], 0)
        self.assertIsNone(imu["raw"])
        self.assertGreater(imu["current_connection_epoch"], old_epoch)

    def test_imu_sequence_timestamp_and_filter_epoch_are_monotonic(self) -> None:
        protocol = FakeProtocol()
        self.studio = motion.MotionStudio(protocol)
        self.studio.on_connection(True)
        self.assertTrue(wait_until(lambda: self.studio.snapshot()["identity_valid"]))
        with mock.patch.object(motion.time, "time_ns", return_value=1_700_000_000_000_000_000):
            first = self.studio.poll_imu_once(force=True)
            with self.studio._imu_filter._lock:
                assert self.studio._imu_filter._last_monotonic is not None
                self.studio._imu_filter._last_monotonic -= motion.IMU_FILTER_RESET_GAP_S + 1.0
            second = self.studio.poll_imu_once(force=True)
        self.assertEqual(second["sample_seq"], first["sample_seq"] + 1)
        self.assertGreater(second["timestamp_ms"], first["timestamp_ms"])
        self.assertEqual(second["filter_epoch"], first["filter_epoch"] + 1)
        self.assertTrue(second["attitude"]["filter_gap_reset"])
        self.assertEqual(second["attitude"]["yaw_deg"], 0.0)
        state = self.studio.snapshot()
        calibration = self.studio.calibration_snapshot()
        self.assertEqual(state["sample_seq"], second["sample_seq"])
        self.assertEqual(state["filter_epoch"], second["filter_epoch"])
        self.assertEqual(state["timestamp_ms"], second["timestamp_ms"])
        self.assertEqual(state["imu_sample_seq"], second["sample_seq"])
        self.assertEqual(state["imu_filter_epoch"], second["filter_epoch"])
        self.assertEqual(calibration["sample_seq"], second["sample_seq"])
        self.assertEqual(calibration["filter_epoch"], second["filter_epoch"])

    def test_arm_checks_power_and_enables_watchdog(self) -> None:
        protocol = FakeProtocol(power_ready=True)
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        self.assertTrue(self.studio.armed)
        self.assertIn("power", protocol.calls)
        self.assertIn(("watchdog", motion.SERVO_LEASE_MS), protocol.calls)

    def test_arm_rejects_identity_pending_before_power_or_servo_commands(self) -> None:
        protocol = FakeProtocol()
        self.studio = motion.MotionStudio(protocol)
        with self.studio.lock:
            self.studio.online = True
            self.studio.connection_epoch = 1
            # A passive log value must not satisfy active identity.
            self.studio.chip_id = protocol.identity_value
            self.studio._identity_epoch = None
        with self.assertRaisesRegex(motion.StudioError, "chip_id"):
            self.studio.arm()
        self.assertNotIn("power", protocol.calls)
        self.assertNotIn(("watchdog", motion.SERVO_LEASE_MS), protocol.calls)

    @staticmethod
    def _widen_project_profile(studio: motion.MotionStudio) -> None:
        for servo in studio.store.project["device_profile"]["servos"]:
            servo["min_us"] = motion.SERVO_MIN_US
            servo["center_us"] = motion.SERVO_CENTER_US
            servo["max_us"] = motion.SERVO_MAX_US

    def test_old_wide_project_profile_cannot_expand_unrecorded_chip_pose_limits(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self._widen_project_profile(self.studio)
        self.studio.arm()
        result = self.studio.submit_pose(
            {
                "servos_us": [
                    motion.SERVO_MIN_US,
                    motion.SERVO_MAX_US,
                    motion.SERVO_MIN_US + 100,
                    motion.SERVO_MAX_US - 100,
                ],
                "duration_ms": 40,
                "easing": "linear",
            }
        )
        self.assertEqual(result["pose"]["servos_us"], [1000, 2000, 1000, 2000])
        state = self.studio.snapshot()
        self.assertEqual(state["servo_limits"]["source"], "conservative_default")
        self.assertEqual(
            [(item["min_us"], item["max_us"]) for item in state["servo_limits"]["channels"]],
            [(1000, 2000)] * 4,
        )

    def test_other_chip_calibration_is_never_applied(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self._widen_project_profile(self.studio)
        other = calibration_payload(chip_id="fedcba9876543210")
        for servo in other["servo_calibration"]:
            servo["min_us"] = 600
            servo["max_us"] = 2400
        self.studio.store.put_calibration(other)
        self.studio.arm()
        result = self.studio.submit_pose(
            {"servos_us": [600, 2400, 700, 2300], "duration_ms": 40, "easing": "linear"}
        )
        self.assertEqual(result["pose"]["servos_us"], [1000, 2000, 1000, 2000])
        self.assertEqual(self.studio.snapshot()["servo_limits"]["source"], "conservative_default")

    def test_current_chip_calibration_controls_pose_limits(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        current = calibration_payload(chip_id=protocol.identity_value)
        expected_limits = [(800, 2200), (850, 2150), (900, 2100), (950, 2050)]
        for servo, (low, high) in zip(current["servo_calibration"], expected_limits):
            servo["min_us"] = low
            servo["max_us"] = high
        self.studio.store.put_calibration(current)
        self.studio.arm()
        result = self.studio.submit_pose(
            {
                "servos_us": [motion.SERVO_MIN_US, motion.SERVO_MAX_US] * 2,
                "duration_ms": 40,
                "easing": "linear",
            }
        )
        self.assertEqual(result["pose"]["servos_us"], [800, 2150, 900, 2050])
        state = self.studio.snapshot()
        self.assertEqual(state["servo_limits"]["source"], "device_calibration")
        self.assertEqual(state["servo_limits"]["chip_id"], protocol.identity_value)

    def test_stale_action_frames_are_reclamped_at_playback(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self._widen_project_profile(self.studio)
        action = self.studio.store.create_action(
            {
                "name": "legacy wide action",
                "keyframes": [
                    {
                        "time_ms": 0,
                        "duration_ms": 20,
                        "easing": "linear",
                        "servos_us": [
                            motion.SERVO_MIN_US,
                            motion.SERVO_MAX_US,
                            motion.SERVO_MIN_US + 100,
                            motion.SERVO_MAX_US - 100,
                        ],
                        "led": motion.default_led(),
                    }
                ],
            }
        )
        self.studio.arm()
        self.studio.playback_start(action["id"])
        self.assertTrue(wait_until(lambda: any(frame.get("source") == "playback" for frame in protocol.frames)))
        played = next(frame for frame in protocol.frames if frame.get("source") == "playback")
        self.assertEqual(played["servos_us"], [1000, 2000, 1000, 2000])

    def test_imu_snapshot_contains_raw_scaled_and_fused_attitude(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        result = self.studio.poll_imu_once(force=True)
        self.assertTrue(result["available"])
        self.assertTrue(result["fresh"])
        self.assertIsInstance(result["timestamp_ms"], int)
        self.assertEqual(result["raw"]["accel"]["z"], 16384)
        self.assertEqual(result["scaled"]["accel_g"]["z"], 1.0)
        self.assertEqual(result["scaled"]["temperature_c"], 36.53)
        self.assertAlmostEqual(result["attitude"]["roll_deg"], 0.0, places=3)
        self.assertAlmostEqual(result["attitude"]["pitch_deg"], 0.0, places=3)
        self.assertTrue(result["attitude"]["yaw_drift_expected"])
        self.assertEqual(result["health"], "ok")
        self.assertIsNone(result["error"])

    def test_imu_failure_is_degraded_without_disarming_motion(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        protocol.imu_error = motion.StudioError("MPU read timed out")
        result = self.studio.poll_imu_once(force=True)
        self.assertFalse(result["fresh"])
        self.assertEqual(result["health"], "error")
        self.assertIn("timed out", result["error"])
        self.assertTrue(self.studio.armed)
        self.assertIsNone(self.studio.error)

    def test_imu_disconnect_keeps_last_sample_but_marks_it_unavailable(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.poll_imu_once(force=True)
        protocol.online = False
        self.studio.on_connection(False, "USB gone")
        result = self.studio.imu_snapshot()
        self.assertFalse(result["available"])
        self.assertFalse(result["fresh"])
        self.assertEqual(result["health"], "offline")
        self.assertEqual(result["error"], "USB gone")
        self.assertEqual(result["raw"]["accel"]["z"], 16384)
        protocol.online = True
        self.studio.on_connection(True)
        reconnecting = self.studio.imu_snapshot()
        self.assertFalse(reconnecting["available"])
        self.assertFalse(reconnecting["fresh"])
        self.assertEqual(reconnecting["health"], "initializing")

    def test_imu_poll_interval_has_a_conservative_floor(self) -> None:
        protocol = FakeProtocol()
        self.studio = motion.MotionStudio(protocol, imu_poll_interval_s=0.01)
        self.assertEqual(self.studio.imu_poll_interval_s, motion.IMU_MIN_POLL_INTERVAL_S)

    def test_arm_rejects_no_battery_without_explicit_override(self) -> None:
        protocol = FakeProtocol(power_ready=False)
        self.studio = create_started_studio(protocol, allow_power_override=True)
        with self.assertRaises(motion.StudioError):
            self.studio.arm()
        self.assertFalse(self.studio.armed)
        self.studio.arm(override_power=True)
        self.assertTrue(self.studio.armed)
        self.assertEqual(self.studio.warnings[-1]["code"], "external-power-override")

    def test_live_pose_coalesces_and_confirms_latest(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        for value in range(1500, 1525):
            self.studio.submit_pose({"servos_us": [value] * 4, "duration_ms": 100, "easing": "linear"})
        self.assertTrue(wait_until(lambda: self.studio.confirmed_pose["servos_us"] == [1524] * 4))
        self.assertLess(len(protocol.frames), 25)

    def test_recording_stores_confirmed_servo_and_led_frame(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        self.studio.record_start()
        self.studio.submit_pose(
            {
                "servos_us": [1200, 1400, 1600, 1800],
                "duration_ms": 220,
                "easing": "smooth",
                "led": {"enabled": True, "color": "#FF3300", "brightness": 61, "pixel_count": 10},
            }
        )
        self.assertTrue(wait_until(lambda: self.studio.confirmed_pose["servos_us"][0] == 1200))
        action = self.studio.record_stop("行走测试")
        self.assertGreaterEqual(len(action["keyframes"]), 2)
        self.assertEqual(action["keyframes"][-1]["led"]["pixel_count"], 10)
        self.assertEqual(action["keyframes"][-1]["led"]["color"], "#FF3300")

    def test_recorded_action_survives_studio_restart_with_project_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motion-project.json"
            protocol = FakeProtocol()
            self.studio = create_started_studio(protocol, store=motion.ProjectStore(path))
            self.studio.arm()
            self.studio.record_start()
            self.studio.submit_pose(
                {
                    "servos_us": [1320, 1440, 1680, 1760],
                    "duration_ms": 160,
                    "easing": "smooth",
                    "led": {"enabled": True, "color": "#35A7FF", "brightness": 25, "pixel_count": 8},
                }
            )
            self.assertTrue(wait_until(lambda: self.studio.confirmed_pose["servos_us"][0] == 1320))
            action = self.studio.record_stop("向前一步")
            self.studio.shutdown()
            self.studio = None

            restored = motion.ProjectStore(path)
            saved = restored.get_action(action["id"])
            self.assertEqual(saved["name"], "向前一步")
            self.assertEqual(saved["keyframes"][-1]["servos_us"], [1320, 1440, 1680, 1760])
            self.assertEqual(saved["keyframes"][-1]["led"]["pixel_count"], 8)

    def test_estop_interrupts_recording_without_saving_partial_action(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        before = len(self.studio.store.snapshot()["actions"])
        self.studio.record_start()

        state = self.studio.emergency_stop()

        self.assertFalse(state["armed"])
        self.assertTrue(state["estop"])
        self.assertEqual(state["recording"]["state"], "idle")
        self.assertTrue(state["recording"]["interrupted"])
        self.assertEqual(len(self.studio.store.snapshot()["actions"]), before)
        with self.assertRaises(motion.StudioError):
            self.studio.record_stop("不应被保存")

    def test_disarm_immediately_replaces_stale_servo_cache_with_verified_safe_state(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        stale = {
            **protocol.state_data,
            "mode": "frame",
            "active": True,
            "enabled_mask": 0x0F,
            "watchdog_ms": motion.SERVO_LEASE_MS,
            "remaining_ms": 777,
        }
        protocol.state_data = dict(stale)
        with self.studio.lock:
            self.studio.servo_state = dict(stale)
            self.studio._safety_off_pending = False
        protocol.calls.clear()

        state = self.studio.disarm()

        self.assertEqual(state["servo_state"]["enabled_mask"], 0)
        self.assertFalse(state["servo_state"]["active"])
        self.assertEqual(state["servo_state"]["watchdog_ms"], 0)
        self.assertEqual(state["servo_state"]["remaining_ms"], 0)
        self.assertFalse(state["safety_off_pending"])
        safety_calls = [
            item
            for item in protocol.calls
            if item in {"all-off", ("watchdog", 0), "rgb-release", "state"}
        ]
        self.assertEqual(safety_calls, ["all-off", ("watchdog", 0), "rgb-release", "state"])

    def test_disarm_does_not_claim_safe_when_hardware_state_remains_enabled(self) -> None:
        class UnsafeAfterSafeOffProtocol(FakeProtocol):
            def safe_off(self):
                self.calls.extend(["all-off", ("watchdog", 0), "rgb-release"])
                return reply("ok")

        protocol = UnsafeAfterSafeOffProtocol()
        self.studio = create_started_studio(protocol)
        unsafe = {
            **protocol.state_data,
            "mode": "frame",
            "active": True,
            "enabled_mask": 0x03,
            "watchdog_ms": motion.SERVO_LEASE_MS,
            "remaining_ms": 500,
        }
        protocol.state_data = dict(unsafe)
        with self.studio.lock:
            self.studio.servo_state = dict(unsafe)
            self.studio._safety_off_pending = False

        state = self.studio.emergency_stop()

        self.assertTrue(state["safety_off_pending"])
        self.assertIn("仍报告舵机输出", state["error"])
        self.assertEqual(state["servo_state"]["enabled_mask"], 0x03)
        self.assertTrue(state["servo_state"]["active"])
        self.assertEqual(state["servo_state"]["watchdog_ms"], motion.SERVO_LEASE_MS)

    def test_playback_loop_and_rate(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        action = self.studio.create_action(
            {
                "name": "two frames",
                "keyframes": [
                    {
                        "time_ms": 0,
                        "duration_ms": 20,
                        "easing": "linear",
                        "servos_us": [1300] * 4,
                        "led": motion.default_led(),
                    },
                    {
                        "time_ms": 30,
                        "duration_ms": 20,
                        "easing": "smooth",
                        "servos_us": [1700] * 4,
                        "led": {"enabled": True, "color": "#00FF00", "brightness": 20, "pixel_count": 2},
                    },
                ],
            }
        )
        self.studio.playback_start(action["id"], rate=2.0, loop=True, loop_count=2)
        self.assertTrue(wait_until(lambda: self.studio.playback["state"] == "stopped"))
        played = [item for item in protocol.frames if item.get("source") == "playback"]
        self.assertEqual(len(played), 4)
        self.assertEqual(played[-1]["duration_ms"], 10)

    def test_playback_waits_for_final_non_blocking_transition(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        action = self.studio.create_action(
            {
                "name": "final transition",
                "keyframes": [
                    {
                        "time_ms": 0,
                        "duration_ms": 180,
                        "easing": "ease-out",
                        "servos_us": [1600] * 4,
                        "led": motion.default_led(),
                    }
                ],
            }
        )
        started = time.monotonic()
        self.studio.playback_start(action["id"], rate=1.0, loop=False)
        self.assertTrue(wait_until(lambda: self.studio.playback["state"] == "stopped", timeout=1.0))
        self.assertGreaterEqual(time.monotonic() - started, 0.15)

    def test_disconnect_locks_and_reconnect_initializes_all_off(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.arm()
        protocol.online = False
        self.studio.on_connection(False, "USB gone")
        self.assertFalse(self.studio.armed)
        self.assertTrue(self.studio.estop)
        self.assertTrue(self.studio.safety_off_pending if hasattr(self.studio, "safety_off_pending") else self.studio._safety_off_pending)
        protocol.online = True
        before = protocol.calls.count("all-off")
        self.studio.on_connection(True)
        self.assertTrue(wait_until(lambda: protocol.calls.count("all-off") > before))

    def test_led_calibration_get_degrades_for_old_firmware_without_main_error(self) -> None:
        class LegacyProtocol(FakeProtocol):
            led_calibration_status = None

        protocol = LegacyProtocol()
        self.studio = create_started_studio(protocol)
        self.studio.error = "existing diagnostic"
        result = self.studio.led_calibration_snapshot()
        self.assertFalse(result["available"])
        self.assertEqual(result["reason"], "unsupported")
        self.assertTrue(result["identity_valid"])
        self.assertEqual(self.studio.error, "existing diagnostic")

    def test_led_calibration_save_resends_logical_light_without_rewriting_action(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        action = self.studio.create_action(
            {
                "name": "color contract",
                "keyframes": [
                    {
                        "time_ms": 0,
                        "duration_ms": 20,
                        "easing": "linear",
                        "servos_us": [1500] * 4,
                        "led": {
                            "enabled": True,
                            "color": "#8B5CF6",
                            "brightness": 38,
                            "pixel_count": 8,
                        },
                    }
                ],
            }
        )
        self.studio.set_light(
            {"enabled": True, "color": "#A13CF0", "brightness": 27, "pixel_count": 8}
        )
        project_before = self.studio.store.snapshot()
        result = self.studio.save_led_calibration(
            led_calibration_api_payload(
                self.studio,
                color_order="BGR",
                r_gain_permille=840,
                g_gain_permille=610,
                b_gain_permille=930,
                gamma_x100=225,
            )
        )
        self.assertTrue(result["available"])
        self.assertTrue(result["light_resent"])
        self.assertEqual(result["calibration"]["color_order"], "BGR")
        self.assertIn(
            (
                "led-calibration-set",
                {
                    "color_order": "BGR",
                    "r_gain_permille": 840,
                    "g_gain_permille": 610,
                    "b_gain_permille": 930,
                    "gamma_x100": 225,
                },
            ),
            protocol.calls,
        )
        resent = [item for item in protocol.calls if isinstance(item, tuple) and item[0] == "light"][-1][1]
        self.assertEqual(
            resent,
            {"enabled": True, "color": "#A13CF0", "brightness": 27, "pixel_count": 8},
        )
        self.assertEqual(self.studio.store.snapshot(), project_before)
        self.assertEqual(self.studio.store.get_action(action["id"])["keyframes"][0]["led"]["color"], "#8B5CF6")

    def test_led_calibration_save_rejects_stale_wrong_device_and_motion(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        with self.assertRaisesRegex(motion.ValidationError, "connection_epoch"):
            self.studio.save_led_calibration(
                led_calibration_api_payload(self.studio, connection_epoch=self.studio.connection_epoch + 1)
            )
        with self.assertRaisesRegex(motion.ValidationError, "chip_id"):
            self.studio.save_led_calibration(
                led_calibration_api_payload(self.studio, chip_id="fedcba9876543210")
            )
        with self.assertRaisesRegex(motion.ValidationError, "board"):
            self.studio.save_led_calibration(
                led_calibration_api_payload(self.studio, board="arcs_mini")
            )
        self.studio.armed = True
        with self.assertRaisesRegex(motion.StudioError, "锁定舵机"):
            self.studio.save_led_calibration(led_calibration_api_payload(self.studio))
        self.assertFalse(any(isinstance(item, tuple) and item[0] == "led-calibration-set" for item in protocol.calls))

    def test_led_calibration_reset_is_identity_bound_and_resends_light(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        result = self.studio.save_led_calibration(
            {
                "connection_epoch": self.studio.connection_epoch,
                "chip_id": self.studio.chip_id,
                "board": self.studio.board,
                "reset": True,
            }
        )
        self.assertTrue(result["reset"])
        self.assertIn("led-calibration-reset", protocol.calls)
        self.assertTrue(any(isinstance(item, tuple) and item[0] == "light" for item in protocol.calls))

    def test_light_is_allowed_while_locked_and_never_sends_servo_frame(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol)
        result = self.studio.set_light(
            {"enabled": True, "color": "#2244AA", "brightness": 33, "pixel_count": 5}
        )
        self.assertTrue(result["accepted"])
        self.assertFalse(self.studio.armed)
        self.assertFalse(protocol.frames)
        self.assertTrue(any(isinstance(item, tuple) and item[0] == "light" for item in protocol.calls))
        self.assertIn(("watchdog", motion.SERVO_LEASE_MS), protocol.calls)

    def test_locked_light_releases_once_when_last_sse_client_leaves(self) -> None:
        protocol = FakeProtocol()
        self.studio = create_started_studio(protocol, client_grace_s=0.05)
        channel = self.studio.subscribe()
        self.studio.set_light(
            {"enabled": True, "color": "#3377FF", "brightness": 30, "pixel_count": 4}
        )
        before = protocol.calls.count("all-off")
        self.studio.unsubscribe(channel)
        self.assertTrue(wait_until(lambda: protocol.calls.count("all-off") == before + 1, timeout=1.0))
        time.sleep(0.45)
        self.assertEqual(protocol.calls.count("all-off"), before + 1)
        self.assertFalse(self.studio.snapshot()["light_manual_active"])


class StaticAssetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.html = (motion.ASSET_DIR / "index.html").read_text(encoding="utf-8")
        cls.script = (motion.ASSET_DIR / "studio.js").read_text(encoding="utf-8")

    def test_motion_workflow_controls_have_unique_stable_ids(self) -> None:
        identifiers = re.findall(r'\bid="([^"]+)"', self.html)
        self.assertEqual(len(identifiers), len(set(identifiers)))
        self.assertTrue(
            {
                "previewBtn",
                "updateKeyframeBtn",
                "testPreviousFrameBtn",
                "testSelectedFrameBtn",
                "testNextFrameBtn",
                "timelineGrid",
                "imuStatus",
                "imuZeroBtn",
                "imuSafetyToggle",
                "actionTemplateGrid",
                "gaitParameters",
                "homeButton",
            }.issubset(identifiers)
        )

    def test_servo_electrical_limits_match_backend_and_static_assets(self) -> None:
        self.assertEqual((motion.SERVO_MIN_US, motion.SERVO_MAX_US), (500, 2500))
        self.assertIn("const SERVO_ELECTRICAL_MIN = 500;", self.script)
        self.assertIn("const SERVO_ELECTRICAL_MAX = 2500;", self.script)
        self.assertIn("500 ≤ MIN < CENTER < MAX ≤ 2500", self.script)
        self.assertIn("limits.hard_min_us ?? SERVO_ELECTRICAL_MIN", self.script)
        self.assertIn("limits.hard_max_us ?? SERVO_ELECTRICAL_MAX", self.script)
        self.assertIn("PWM 500–2500 µs", self.html)
        for index in range(motion.SERVO_COUNT):
            self.assertRegex(
                self.html,
                rf'id="cal{index}Min"[^>]*\bmin="500"[^>]*\bmax="2498"',
            )
            self.assertRegex(
                self.html,
                rf'id="cal{index}Center"[^>]*\bmin="501"[^>]*\bmax="2499"',
            )
            self.assertRegex(
                self.html,
                rf'id="cal{index}Max"[^>]*\bmin="502"[^>]*\bmax="2500"',
            )

        viewer = (motion.ASSET_DIR / "dog-viewer.js").read_text(encoding="utf-8")
        self.assertIn("[options.minUs=500]", viewer)
        self.assertIn("[options.maxUs=2500]", viewer)
        self.assertIn("finiteNumber(options.minUs, 500)", viewer)
        self.assertIn("finiteNumber(options.maxUs, 2500)", viewer)

    def test_home_button_uses_device_calibration_and_fixed_smooth_transition(self) -> None:
        self.assertIn('id="homeButton"', self.html)
        self.assertIn("const HOME_BUTTON_DURATION_MS = 500;", self.script)
        section = self.script[
            self.script.index("function homeCommandReadiness()") :
            self.script.index("const average =")
        ]
        self.assertIn("const home = calibratedHomePose();", section)
        self.assertIn('(studio.playback?.state || "stopped") !== "stopped"', section)
        self.assertIn("studio.recording", section)
        self.assertIn("studio.postureRoutine.active", section)
        self.assertIn("duration_ms: HOME_BUTTON_DURATION_MS", section)
        self.assertIn('easing: "smooth"', section)
        self.assertIn('source: "home-button"', section)
        self.assertIn("requestJson(API.pose", section)
        self.assertIn('$("homeButton").addEventListener("click", sendHomePose);', self.script)

    def test_action_creator_exposes_only_safe_supported_templates(self) -> None:
        templates = re.findall(r'data-action-template="([^"]+)"', self.html)
        self.assertEqual(
            templates,
            ["custom", "forward", "backward", "march", "turn-left", "turn-right"],
        )
        self.assertRegex(self.html, r'class="action-template-card risk-template-card"[^>]*\bdisabled\b')
        self.assertIn("4×1DOF", self.html)

    def test_preview_and_imu_paths_remain_non_ambiguous(self) -> None:
        self.assertIn('imu: "/api/imu"', self.script)
        self.assertIn("function sampleAction", self.script)
        self.assertIn("function makePoseTimelineTrack", self.script)
        self.assertIn("仅模型播放 · 不驱动真机", self.script)
        self.assertNotIn("applyFrame(frames[frames.length - 1], { send: studio.armed })", self.script)
        self.assertIn('"ease-in": "ease-in"', self.script)
        self.assertIn('"ease-out": "ease-out"', self.script)

    def test_real_device_keyframe_step_is_explicit_and_slow(self) -> None:
        section = self.script[
            self.script.index("function keyframeTestDuration(") :
            self.script.index("function renderKeyframeInspector()")
        ]
        self.assertIn("Math.max(1000, authoredMs / rate)", section)
        self.assertIn("/ 20) * 20", section)
        self.assertIn("servos_us: frame.servos_us.map", section)
        self.assertIn('frame.duration_ms === 0 ? "smooth"', section)
        self.assertIn("requestJson(API.pose", section)
        self.assertIn('source: "keyframe-test"', section)
        self.assertIn('studio.playback?.state || "stopped"', section)
        self.assertIn("window.setTimeout(resolve, durationMs + 120)", section)
        self.assertNotIn("schedulePose(", section)
        self.assertNotIn("API.light", section)

    def test_live_servo_sliders_are_throttled_and_use_short_linear_frames(self) -> None:
        self.assertIn("const POSE_COALESCE_MS = 40;", self.script)
        self.assertIn("const LIVE_DRAG_DURATION_MS = 40;", self.script)
        self.assertIn('source === "manual-live" || source === "recording-live"', self.script)
        self.assertIn('return { duration_ms: LIVE_DRAG_DURATION_MS, easing: "linear" };', self.script)
        self.assertIn("function queuePoseFlush()", self.script)
        scheduler = self.script[
            self.script.index("function queuePoseFlush()") :
            self.script.index("async function flushPose()")
        ]
        self.assertNotIn("clearTimeout", scheduler)
        self.assertIn("POSE_COALESCE_MS - elapsed", scheduler)
        self.assertIn('"recording-live" : "manual-live"', self.script)

    def test_imu_mounting_profile_loads_before_studio_and_has_executable_contract(self) -> None:
        mounting_script = motion.ASSET_DIR / "imu-mounting.js"
        self.assertTrue(mounting_script.is_file())
        self.assertLess(
            self.html.index('/assets/imu-mounting.js'),
            self.html.index('/assets/studio.js'),
        )
        self.assertIn("IMU_MOUNTING.displayedBodyAttitude", self.script)
        self.assertIn("studio.imuReference.sensorLevelBaseline", self.script)
        self.assertIn("const orientation = displayedImuAttitude();", self.script)
        self.assertEqual(self.script.count("const attitude = displayedImuAttitude();"), 2)
        self.assertIn("const attitude = telemetry.attitude", self.script)
        node = shutil.which("node")
        if node is None:
            self.skipTest("node is required for the browser mounting behavior contract")
        completed = subprocess.run(
            [node, str(motion.ASSET_DIR.parent / "test_quadruped_imu_mounting.js")],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)


class HttpApiTest(unittest.TestCase):
    def setUp(self) -> None:
        self.protocol = FakeProtocol()
        self.studio = create_started_studio(self.protocol)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), motion.handler_for(self.studio))
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)
        self.studio.shutdown()

    def request(self, path: str, method: str = "GET", payload: Any = None):
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            self.base + path,
            method=method,
            data=data,
            headers={"Content-Type": "application/json"} if data is not None else {},
        )
        try:
            response = urllib.request.urlopen(request, timeout=2.0)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            return response.status, json.loads(response.read().decode("utf-8"))

    def request_bytes(self, path: str):
        request = urllib.request.Request(self.base + path)
        try:
            response = urllib.request.urlopen(request, timeout=2.0)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            return response.status, response.headers.get("Content-Type", ""), response.read()

    def test_formal_index_css_and_js_assets_are_served(self) -> None:
        status, content_type, body = self.request_bytes("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", content_type)
        self.assertIn(b'class="app-shell"', body)
        self.assertIn(b'/assets/imu-mounting.js', body)
        self.assertIn(b'/assets/studio.js', body)
        status, content_type, body = self.request_bytes("/assets/studio.css")
        self.assertEqual(status, 200)
        self.assertIn("text/css", content_type)
        self.assertIn(b".app-shell", body)
        status, content_type, body = self.request_bytes("/assets/studio.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", content_type)
        self.assertIn(b'const API = Object.freeze', body)
        self.assertIn(b"toBackendProject(studio.project)", body)
        self.assertIn(b"fromBackendProject(snapshot.project", body)
        self.assertIn(b"power_override_allowed", body)
        status, content_type, body = self.request_bytes("/assets/imu-mounting.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", content_type)
        self.assertIn(b"arcs-mini-v2-1-mpu6050-rx-180", body)
        self.assertIn(b"displayedBodyAttitude", body)

    def test_local_3d_viewer_model_and_license_assets_are_served(self) -> None:
        status, content_type, body = self.request_bytes("/assets/dog-viewer.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", content_type)
        self.assertIn(b"mountDogViewer", body)
        self.assertIn(b"degToRad(-state.bodyOrientation.yawDeg)", body)
        self.assertNotIn(b"https://unpkg.com", body)
        self.assertNotIn(b"https://cdn", body)

        status, content_type, body = self.request_bytes("/assets/models/unitree-go1-animated.glb")
        self.assertEqual(status, 200)
        self.assertIn("model/gltf-binary", content_type)
        self.assertGreater(len(body), 1_000_000)
        self.assertEqual(body[:4], b"glTF")

        status, content_type, body = self.request_bytes("/assets/models/ATTRIBUTION.md")
        self.assertEqual(status, 200)
        self.assertIn("text/markdown", content_type)
        self.assertIn(b"CC BY 4.0", body)

        status, content_type, body = self.request_bytes("/assets/vendor/three/three.module.min.js")
        self.assertEqual(status, 200)
        self.assertIn("javascript", content_type)
        self.assertGreater(len(body), 100_000)

    def test_asset_path_traversal_is_rejected(self) -> None:
        status, _, _ = self.request_bytes("/assets/%2e%2e/quadruped_motion_studio.py")
        self.assertEqual(status, 404)

    def test_state_envelope_and_alias(self) -> None:
        for path in ("/api/state", "/api/status"):
            status, payload = self.request(path)
            self.assertEqual(status, 200)
            self.assertTrue(payload["ok"])
            self.assertIsNone(payload["error"])
            self.assertIn("state", payload)

    def test_imu_endpoint_returns_cached_read_only_contract(self) -> None:
        self.studio.poll_imu_once(force=True)
        calls_before = self.protocol.calls.count("imu")
        status, payload = self.request("/api/imu")
        self.assertEqual(status, 200)
        self.assertTrue(payload["ok"])
        self.assertIsNone(payload["error"])
        self.assertNotIn("state", payload)
        result = payload["result"]
        self.assertTrue(result["available"])
        self.assertTrue(result["fresh"])
        self.assertIn("timestamp_ms", result)
        self.assertIn("sample_seq", result)
        self.assertIn("connection_epoch", result)
        self.assertIn("filter_epoch", result)
        self.assertIn("chip_id", result)
        self.assertEqual(result["raw"]["accel"], {"x": 0, "y": 0, "z": 16384})
        self.assertIn("roll_deg", result["attitude"])
        self.assertIn("pitch_deg", result["attitude"])
        self.assertIn("yaw_deg", result["attitude"])
        self.assertTrue(result["attitude"]["yaw_drift_expected"])
        # GET reads the cache; it must not turn HTTP request rate into I2C rate.
        self.assertEqual(self.protocol.calls.count("imu"), calls_before)

    def test_imu_endpoint_degrades_cleanly_while_disconnected(self) -> None:
        self.protocol.online = False
        self.studio.on_connection(False, "ADB disconnected")
        status, payload = self.request("/api/imu")
        self.assertEqual(status, 200)
        self.assertTrue(payload["ok"])
        self.assertFalse(payload["result"]["available"])
        self.assertFalse(payload["result"]["fresh"])
        self.assertEqual(payload["result"]["health"], "offline")
        self.assertEqual(payload["result"]["error"], "ADB disconnected")

    def test_led_calibration_api_get_put_and_old_firmware_fallback(self) -> None:
        status, payload = self.request("/api/led-calibration")
        self.assertEqual(status, 200, payload)
        self.assertTrue(payload["ok"])
        self.assertNotIn("state", payload)
        self.assertTrue(payload["result"]["available"])
        self.assertEqual(payload["result"]["calibration"]["color_order"], "GRB")

        status, payload = self.request(
            "/api/led-calibration",
            "PUT",
            led_calibration_api_payload(
                self.studio,
                color_order="GBR",
                r_gain_permille=760,
                g_gain_permille=880,
                b_gain_permille=570,
                gamma_x100=210,
            ),
        )
        self.assertEqual(status, 200, payload)
        self.assertNotIn("state", payload)
        result = payload["result"]
        self.assertTrue(result["available"])
        self.assertTrue(result["light_resent"])
        self.assertEqual(result["calibration"]["color_order"], "GBR")
        self.assertEqual(result["calibration"]["b_gain_permille"], 570)

        self.protocol.led_calibration_error = motion.StudioError("device rejected new command")
        main_error = self.studio.error
        status, payload = self.request("/api/led-calibration")
        self.assertEqual(status, 200, payload)
        self.assertFalse(payload["result"]["available"])
        self.assertEqual(payload["result"]["reason"], "unsupported")
        self.assertEqual(self.studio.error, main_error)

    def test_led_calibration_api_rejects_stale_epoch_and_wrong_chip(self) -> None:
        stale = led_calibration_api_payload(
            self.studio, connection_epoch=self.studio.connection_epoch + 1
        )
        status, payload = self.request("/api/led-calibration", "PUT", stale)
        self.assertEqual(status, 400, payload)
        self.assertIn("connection_epoch", payload["error"])

        wrong = led_calibration_api_payload(
            self.studio, chip_id="fedcba9876543210"
        )
        status, payload = self.request("/api/led-calibration", "PUT", wrong)
        self.assertEqual(status, 400, payload)
        self.assertIn("chip_id", payload["error"])
        self.assertFalse(
            any(
                isinstance(item, tuple) and item[0] == "led-calibration-set"
                for item in self.protocol.calls
            )
        )

    def test_calibration_api_supports_progressive_atomic_record(self) -> None:
        establish_test_identity(self.studio)
        status, payload = self.request("/api/calibration")
        self.assertEqual(status, 200, payload)
        self.assertFalse(payload["result"]["applicable"])
        self.assertEqual(payload["result"]["reason"], "calibration_not_found")

        status, payload = self.request("/api/calibration", "PUT", calibration_api_payload(self.studio))
        self.assertEqual(status, 200, payload)
        result = payload["result"]
        self.assertTrue(result["applicable"])
        self.assertTrue(result["current"]["identity_valid"])
        self.assertEqual(result["connection_epoch"], self.studio.connection_epoch)
        self.assertFalse(result["complete"])
        self.assertEqual(result["completed_steps"], {"servo": True, "home": False, "imu": False})
        self.assertEqual(result["record"]["servo_calibration"][0]["trim_deg"], -3)

        record = result["record"]
        record["home_pose_us"] = [1500, 1510, 1490, 1520]
        record["connection_epoch"] = self.studio.connection_epoch
        status, payload = self.request("/api/calibration", "PUT", record)
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["result"]["completed_steps"], {"servo": True, "home": True, "imu": False})

        record = payload["result"]["record"]
        record["imu_mounting_baseline_deg"] = {"roll": 1.25, "pitch": -2.5}
        record["connection_epoch"] = self.studio.connection_epoch
        status, payload = self.request("/api/calibration", "PUT", record)
        self.assertEqual(status, 200, payload)
        self.assertTrue(payload["result"]["complete"])
        self.assertNotIn("yaw", payload["result"]["record"]["imu_mounting_baseline_deg"])

    def test_calibration_get_reports_disconnect_and_board_change_as_not_applicable(self) -> None:
        establish_test_identity(self.studio)
        status, payload = self.request("/api/calibration", "PUT", calibration_api_payload(self.studio))
        self.assertEqual(status, 200, payload)
        self.studio.board = "different_board"
        status, payload = self.request("/api/calibration")
        self.assertEqual(status, 200, payload)
        self.assertFalse(payload["result"]["applicable"])
        self.assertEqual(payload["result"]["reason"], "board_mismatch")
        self.studio.on_connection(False, "cable removed")
        status, payload = self.request("/api/calibration")
        self.assertEqual(status, 200, payload)
        self.assertFalse(payload["result"]["current"]["online"])
        self.assertIsNone(payload["result"]["current"]["chip_id"])
        self.assertEqual(payload["result"]["reason"], "device_offline")
        self.assertIsNone(payload["result"]["record"])

    def test_calibration_put_rejects_wrong_identity_yaw_and_motion_concurrency(self) -> None:
        establish_test_identity(self.studio)
        status, payload = self.request("/api/calibration", "PUT", calibration_payload())
        self.assertEqual(status, 400, payload)
        self.assertIn("connection_epoch", payload["error"])

        wrong = calibration_api_payload(self.studio, chip_id="fedcba9876543210")
        status, payload = self.request("/api/calibration", "PUT", wrong)
        self.assertEqual(status, 400, payload)
        self.assertIn("chip_id", payload["error"])

        yaw = calibration_api_payload(self.studio, imu_baseline={"roll": 0.0, "pitch": 0.0})
        yaw["imu_mounting_baseline_deg"]["yaw"] = 0.0
        status, payload = self.request("/api/calibration", "PUT", yaw)
        self.assertEqual(status, 400, payload)
        self.assertIn("yaw", payload["error"])

        self.studio.armed = True
        status, payload = self.request("/api/calibration", "PUT", calibration_api_payload(self.studio))
        self.assertEqual(status, 400, payload)
        self.assertIn("锁定舵机", payload["error"])

    def test_posture_test_api_sends_one_four_channel_frame_then_confirms_safe_off(self) -> None:
        install_posture_test_calibration(self.studio)
        self.protocol.calls.clear()
        self.protocol.frames.clear()
        request = posture_test_api_payload(self.studio)

        status, payload = self.request("/api/posture-test", "POST", request)

        self.assertEqual(status, 200, payload)
        result = payload["result"]
        self.assertTrue(result["accepted"])
        self.assertTrue(result["target_reached"])
        self.assertEqual(result["servos_us"], [1750, 900, 1750, 1250])
        self.assertEqual(result["duration_ms"], motion.POSTURE_TEST_MIN_DURATION_MS)
        self.assertTrue(result["outputs_safe"])
        self.assertEqual(result["servo_state"]["enabled_mask"], 0)
        self.assertFalse(result["servo_state"]["active"])
        self.assertEqual(result["servo_state"]["watchdog_ms"], 0)
        self.assertFalse(payload["state"]["armed"])
        self.assertFalse(payload["state"]["safety_off_pending"])
        self.assertEqual(payload["state"]["servo_state"], result["servo_state"])

        self.assertEqual(self.protocol.calls.count("servo-frame"), 1)
        self.assertEqual(len(self.protocol.frames), 1)
        self.assertEqual(self.protocol.frames[0]["servos_us"], [1750, 900, 1750, 1250])
        self.assertEqual(
            self.protocol.frames[0]["duration_ms"],
            motion.POSTURE_TEST_MIN_DURATION_MS,
        )
        frame_index = self.protocol.calls.index("servo-frame")
        all_off_index = self.protocol.calls.index("all-off", frame_index + 1)
        watchdog_off_index = self.protocol.calls.index(("watchdog", 0), all_off_index + 1)
        rgb_release_index = self.protocol.calls.index("rgb-release", watchdog_off_index + 1)
        state_index = self.protocol.calls.index("state", rgb_release_index + 1)
        self.assertLess(frame_index, all_off_index)
        self.assertLess(all_off_index, watchdog_off_index)
        self.assertLess(watchdog_off_index, rgb_release_index)
        self.assertLess(rgb_release_index, state_index)

    def test_posture_test_api_payload_is_strict_and_requires_explicit_confirmation(self) -> None:
        install_posture_test_calibration(self.studio)
        valid = posture_test_api_payload(self.studio)
        missing_confirmation = dict(valid)
        missing_confirmation.pop("safety_confirmed")
        invalid_payloads = {
            "missing safety confirmation": missing_confirmation,
            "false safety confirmation": dict(valid, safety_confirmed=False),
            "unexpected key": dict(valid, easing="smooth"),
            "wrong servo count": dict(valid, servos_us=[1750, 900, 1750]),
            "non-integer servo": dict(valid, servos_us=[1750, 900.5, 1750, 1250]),
        }

        for label, invalid in invalid_payloads.items():
            with self.subTest(label=label):
                frames_before = len(self.protocol.frames)
                status, payload = self.request("/api/posture-test", "POST", invalid)
                self.assertEqual(status, 400, payload)
                self.assertFalse(payload["ok"])
                self.assertTrue(payload["error"])
                self.assertEqual(len(self.protocol.frames), frames_before)

    def test_posture_test_api_rejects_stale_identity_and_armed_state_without_frame(self) -> None:
        install_posture_test_calibration(self.studio)
        identity_mismatches = {
            "connection_epoch": posture_test_api_payload(
                self.studio,
                connection_epoch=self.studio.connection_epoch + 1,
            ),
            "chip_id": posture_test_api_payload(
                self.studio,
                chip_id="fedcba9876543210",
            ),
            "board": posture_test_api_payload(self.studio, board="different_board"),
        }

        for field, request in identity_mismatches.items():
            with self.subTest(field=field):
                frames_before = len(self.protocol.frames)
                status, payload = self.request("/api/posture-test", "POST", request)
                self.assertEqual(status, 400, payload)
                self.assertIn(field, payload["error"])
                self.assertEqual(len(self.protocol.frames), frames_before)

        self.studio.armed = True
        status, payload = self.request(
            "/api/posture-test",
            "POST",
            posture_test_api_payload(self.studio),
        )
        self.assertEqual(status, 400, payload)
        self.assertIn("保持锁定", payload["error"])
        self.assertFalse(self.protocol.frames)

    def test_posture_test_api_requires_online_power_ready_and_idle_motion(self) -> None:
        install_posture_test_calibration(self.studio)
        request = posture_test_api_payload(self.studio)

        self.studio.online = False
        self.protocol.online = False
        status, payload = self.request("/api/posture-test", "POST", request)
        self.assertEqual(status, 503, payload)
        self.assertFalse(self.protocol.frames)

        self.studio.online = True
        self.protocol.online = True
        self.protocol.power_ready = False
        status, payload = self.request("/api/posture-test", "POST", request)
        self.assertEqual(status, 400, payload)
        self.assertIn("电源预检未通过", payload["error"])
        self.assertFalse(self.protocol.frames)

        self.protocol.power_ready = True
        self.studio.playback["state"] = "playing"
        status, payload = self.request("/api/posture-test", "POST", request)
        self.assertEqual(status, 400, payload)
        self.assertIn("播放未停止", payload["error"])
        self.assertFalse(self.protocol.frames)

        self.studio.playback["state"] = "stopped"
        self.studio.recording["state"] = "recording"
        status, payload = self.request("/api/posture-test", "POST", request)
        self.assertEqual(status, 400, payload)
        self.assertIn("录制未停止", payload["error"])
        self.assertFalse(self.protocol.frames)

    def test_posture_test_rejects_unconfirmed_safe_off_before_preflight(self) -> None:
        install_posture_test_calibration(self.studio)
        with self.studio.lock:
            self.studio._safety_off_pending = True
        self.protocol.calls.clear()
        self.protocol.frames.clear()

        status, payload = self.request(
            "/api/posture-test",
            "POST",
            posture_test_api_payload(self.studio),
        )

        self.assertEqual(status, 400, payload)
        self.assertIn("安全断开尚未确认", payload["error"])
        self.assertTrue(payload["state"]["safety_off_pending"])
        self.assertNotIn("power", self.protocol.calls)
        self.assertNotIn("servo-frame", self.protocol.calls)
        self.assertFalse(self.protocol.frames)

    def test_posture_test_rejects_frame_that_stops_before_reaching_target(self) -> None:
        install_posture_test_calibration(self.studio)
        self.protocol.calls.clear()
        self.protocol.frames.clear()
        requested = [1750, 900, 1750, 1250]
        stalled = [1500, 1500, 1500, 1500]
        confirmed_before = list(self.studio.confirmed_pose["servos_us"])
        target_before = list(self.studio.target_pose["servos_us"])

        def incomplete_servo_frame(servos_us, duration_ms, easing="smooth"):
            self.protocol.calls.append("servo-frame")
            self.protocol.frames.append(
                {
                    "servos_us": list(servos_us),
                    "duration_ms": duration_ms,
                    "easing": easing,
                }
            )
            self.protocol.state_data["seq"] += 1
            self.protocol.state_data.update(
                {
                    "mode": "idle",
                    "active": False,
                    "enabled_mask": 0x0F,
                    "current_us": list(stalled),
                    "target_us": list(servos_us),
                    "duration_ms": duration_ms,
                    "elapsed_ms": duration_ms,
                    "easing": easing,
                }
            )
            return reply("SERVO_ACK action=frame status=0", "ok", elapsed=2.5)

        with mock.patch.object(
            self.protocol,
            "servo_frame",
            side_effect=incomplete_servo_frame,
        ):
            status, payload = self.request(
                "/api/posture-test",
                "POST",
                posture_test_api_payload(self.studio, servos_us=requested),
            )

        self.assertEqual(status, 400, payload)
        self.assertIn("未到达请求目标", payload["error"])
        self.assertEqual(self.protocol.calls.count("servo-frame"), 1)
        self.assertEqual(len(self.protocol.frames), 1)
        self.assertEqual(self.studio.confirmed_pose["servos_us"], confirmed_before)
        self.assertEqual(self.studio.target_pose["servos_us"], target_before)
        self.assertNotEqual(self.studio.confirmed_pose["servos_us"], requested)
        self.assertEqual(payload["state"]["confirmed_pose"]["servos_us"], confirmed_before)
        self.assertFalse(payload["state"]["armed"])
        self.assertFalse(payload["state"]["safety_off_pending"])
        self.assertEqual(payload["state"]["servo_state"]["enabled_mask"], 0)
        self.assertFalse(payload["state"]["servo_state"]["active"])
        self.assertEqual(payload["state"]["servo_state"]["watchdog_ms"], 0)
        self.assertEqual(self.protocol.state_data["enabled_mask"], 0)
        self.assertFalse(self.protocol.state_data["active"])
        self.assertEqual(self.protocol.state_data["watchdog_ms"], 0)
        frame_index = self.protocol.calls.index("servo-frame")
        all_off_index = self.protocol.calls.index("all-off", frame_index + 1)
        state_index = self.protocol.calls.index("state", all_off_index + 1)
        self.assertLess(frame_index, all_off_index)
        self.assertLess(all_off_index, state_index)

    def test_posture_test_frame_failure_returns_400_after_verified_cleanup(self) -> None:
        install_posture_test_calibration(self.studio)
        self.protocol.calls.clear()
        self.protocol.frames.clear()

        def failed_servo_frame(servos_us, duration_ms, easing="smooth"):
            self.protocol.calls.append("servo-frame")
            self.protocol.frames.append(
                {
                    "servos_us": list(servos_us),
                    "duration_ms": duration_ms,
                    "easing": easing,
                }
            )
            self.protocol.state_data.update(
                {"active": True, "enabled_mask": 0x0F, "watchdog_ms": 1000}
            )
            raise motion.StudioError("posture frame failed")

        with mock.patch.object(
            self.protocol,
            "servo_frame",
            side_effect=failed_servo_frame,
        ):
            status, payload = self.request(
                "/api/posture-test",
                "POST",
                posture_test_api_payload(self.studio),
            )

        self.assertEqual(status, 400, payload)
        self.assertIn("posture frame failed", payload["error"])
        self.assertEqual(self.protocol.calls.count("servo-frame"), 1)
        self.assertEqual(len(self.protocol.frames), 1)
        self.assertEqual(payload["state"]["servo_state"]["enabled_mask"], 0)
        self.assertFalse(payload["state"]["servo_state"]["active"])
        self.assertEqual(payload["state"]["servo_state"]["watchdog_ms"], 0)
        self.assertFalse(payload["state"]["safety_off_pending"])
        frame_index = self.protocol.calls.index("servo-frame")
        all_off_index = self.protocol.calls.index("all-off", frame_index + 1)
        state_index = self.protocol.calls.index("state", all_off_index + 1)
        self.assertLess(frame_index, all_off_index)
        self.assertLess(all_off_index, state_index)

    def test_posture_test_never_succeeds_when_final_outputs_remain_enabled(self) -> None:
        install_posture_test_calibration(self.studio)
        self.protocol.calls.clear()
        self.protocol.frames.clear()
        original_servo_frame = self.protocol.servo_frame
        original_safe_off = self.protocol.safe_off
        safe_off_calls = 0

        def unsafe_servo_frame(servos_us, duration_ms, easing="smooth"):
            frame_reply = original_servo_frame(servos_us, duration_ms, easing)
            self.protocol.state_data.update(
                {"active": False, "enabled_mask": 0x0F, "watchdog_ms": 1000}
            )
            return frame_reply

        def ineffective_safe_off():
            nonlocal safe_off_calls
            safe_off_calls += 1
            if safe_off_calls == 1:
                return original_safe_off()
            self.protocol.calls.extend(["all-off", ("watchdog", 0), "rgb-release"])
            return reply("ok")

        with (
            mock.patch.object(
                self.protocol,
                "servo_frame",
                side_effect=unsafe_servo_frame,
            ),
            mock.patch.object(self.protocol, "safe_off", side_effect=ineffective_safe_off),
        ):
            status, payload = self.request(
                "/api/posture-test",
                "POST",
                posture_test_api_payload(self.studio),
            )

        self.assertEqual(status, 400, payload)
        self.assertFalse(payload["ok"])
        self.assertEqual(self.protocol.calls.count("servo-frame"), 1)
        self.assertEqual(len(self.protocol.frames), 1)
        self.assertTrue(payload["state"]["safety_off_pending"])
        self.assertFalse(payload["state"]["servo_state"]["active"])
        self.assertEqual(payload["state"]["servo_state"]["enabled_mask"], 0x0F)
        self.assertEqual(payload["state"]["servo_state"]["watchdog_ms"], 1000)
        frame_index = self.protocol.calls.index("servo-frame")
        all_off_index = self.protocol.calls.index("all-off", frame_index + 1)
        state_index = self.protocol.calls.index("state", all_off_index + 1)
        self.assertLess(frame_index, all_off_index)
        self.assertLess(all_off_index, state_index)

    def test_posture_test_estop_during_preclear_never_restarts_servo_frame(self) -> None:
        install_posture_test_calibration(self.studio)
        self.protocol.calls.clear()
        self.protocol.frames.clear()
        posture_preclear_started = threading.Event()
        release_posture_preclear = threading.Event()
        safe_off_call_count = 0
        safe_off_call_lock = threading.Lock()
        original_safe_off = self.protocol.safe_off
        posture_response: dict[str, Any] = {}
        estop_response: dict[str, Any] = {}
        request_errors: list[BaseException] = []

        def coordinated_safe_off():
            nonlocal safe_off_call_count
            with safe_off_call_lock:
                safe_off_call_count += 1
                call_number = safe_off_call_count
            if call_number == 1:
                posture_preclear_started.set()
                if not release_posture_preclear.wait(2.0):
                    raise motion.StudioError("test posture preclear release timeout")
            return original_safe_off()

        def request_posture() -> None:
            try:
                posture_response["response"] = self.request(
                    "/api/posture-test",
                    "POST",
                    posture_test_api_payload(self.studio),
                )
            except BaseException as exc:  # Preserve worker failures for the main assertion thread.
                request_errors.append(exc)

        def request_estop() -> None:
            try:
                estop_response["response"] = self.request("/api/estop", "POST", {})
            except BaseException as exc:
                request_errors.append(exc)

        def cancellation_requested() -> bool:
            with self.studio.lock:
                return bool(self.studio._servo_test_cancel_requested)

        with mock.patch.object(
            self.protocol,
            "safe_off",
            side_effect=coordinated_safe_off,
        ):
            posture_worker = threading.Thread(target=request_posture, daemon=True)
            estop_worker = threading.Thread(target=request_estop, daemon=True)
            posture_worker.start()
            try:
                self.assertTrue(
                    posture_preclear_started.wait(1.0),
                    "posture request never reached its pre-frame safe_off",
                )
                estop_worker.start()
                self.assertTrue(
                    wait_until(cancellation_requested, timeout=1.0),
                    "emergency stop never signalled posture cancellation",
                )
            finally:
                release_posture_preclear.set()
                posture_worker.join(timeout=2.0)
                if estop_worker.ident is not None:
                    estop_worker.join(timeout=2.0)

        self.assertFalse(
            posture_worker.is_alive(),
            "posture request did not terminate after emergency stop",
        )
        self.assertFalse(estop_worker.is_alive(), "emergency stop did not complete")
        self.assertFalse(request_errors, request_errors)
        self.assertIn("response", posture_response)
        self.assertIn("response", estop_response)
        posture_status, posture_payload = posture_response["response"]
        self.assertEqual(posture_status, 400, posture_payload)
        self.assertIn("取消", posture_payload["error"])
        estop_status, estop_payload = estop_response["response"]
        self.assertEqual(estop_status, 200, estop_payload)
        self.assertFalse(estop_payload["state"]["armed"])
        self.assertEqual(estop_payload["state"]["servo_state"]["enabled_mask"], 0)
        self.assertFalse(estop_payload["state"]["servo_state"]["active"])
        self.assertEqual(estop_payload["state"]["servo_state"]["watchdog_ms"], 0)
        self.assertNotIn("servo-frame", self.protocol.calls)
        self.assertFalse(self.protocol.frames)

        final = self.studio.snapshot()
        self.assertFalse(final["armed"])
        self.assertEqual(final["servo_state"]["enabled_mask"], 0)
        self.assertFalse(final["servo_state"]["active"])
        self.assertEqual(final["servo_state"]["watchdog_ms"], 0)
        self.assertEqual(self.protocol.state_data["enabled_mask"], 0)
        self.assertFalse(self.protocol.state_data["active"])
        self.assertEqual(self.protocol.state_data["watchdog_ms"], 0)

    def test_posture_test_op_lock_serializes_calibration_and_playback_stop(self) -> None:
        install_posture_test_calibration(self.studio)
        self.protocol.calls.clear()
        self.protocol.frames.clear()
        posture_preclear_started = threading.Event()
        release_posture_preclear = threading.Event()
        playback_stop_started = threading.Event()
        original_safe_off = self.protocol.safe_off
        posture_response: dict[str, Any] = {}
        playback_response: dict[str, Any] = {}
        worker_errors: list[BaseException] = []

        calibrations_before = json.loads(
            json.dumps(self.studio.store.project["device_calibrations"])
        )
        led_calibration_before = json.loads(json.dumps(self.protocol.led_calibration))

        def blocking_preclear():
            posture_preclear_started.set()
            if not release_posture_preclear.wait(2.0):
                raise motion.StudioError("test posture preclear release timeout")
            return original_safe_off()

        def request_posture() -> None:
            try:
                posture_response["response"] = self.request(
                    "/api/posture-test",
                    "POST",
                    posture_test_api_payload(self.studio),
                )
            except BaseException as exc:
                worker_errors.append(exc)

        def stop_playback() -> None:
            playback_stop_started.set()
            try:
                playback_response["result"] = self.studio.playback_stop()
            except BaseException as exc:
                worker_errors.append(exc)

        with mock.patch.object(
            self.protocol,
            "safe_off",
            side_effect=blocking_preclear,
        ):
            posture_worker = threading.Thread(target=request_posture, daemon=True)
            playback_worker = threading.Thread(target=stop_playback, daemon=True)
            posture_worker.start()
            try:
                self.assertTrue(
                    posture_preclear_started.wait(1.0),
                    "posture request never acquired the actuation operation lock",
                )

                changed_calibration = calibration_api_payload(
                    self.studio,
                    home_pose_us=[1900, 1100, 1900, 1100],
                )
                with self.assertRaisesRegex(motion.StudioError, "设备输出操作正在执行"):
                    self.studio.save_calibration(changed_calibration)
                with self.assertRaisesRegex(motion.StudioError, "设备输出操作正在执行"):
                    self.studio.save_led_calibration(
                        led_calibration_api_payload(self.studio, color_order="RGB")
                    )

                self.assertEqual(
                    self.studio.store.project["device_calibrations"],
                    calibrations_before,
                )
                self.assertEqual(self.protocol.led_calibration, led_calibration_before)
                self.assertFalse(
                    any(
                        isinstance(item, tuple)
                        and item[0] == "led-calibration-set"
                        for item in self.protocol.calls
                    )
                )

                playback_worker.start()
                self.assertTrue(playback_stop_started.wait(1.0))
                self.assertTrue(
                    playback_worker.is_alive(),
                    "playback_stop did not wait for the posture operation lock",
                )
                self.assertNotIn("stop", self.protocol.calls)
            finally:
                release_posture_preclear.set()
                posture_worker.join(timeout=2.0)
                if playback_worker.ident is not None:
                    playback_worker.join(timeout=2.0)

        self.assertFalse(posture_worker.is_alive(), "posture request did not complete")
        self.assertFalse(playback_worker.is_alive(), "playback_stop did not complete")
        self.assertFalse(worker_errors, worker_errors)
        self.assertIn("response", posture_response)
        posture_status, posture_payload = posture_response["response"]
        self.assertEqual(posture_status, 200, posture_payload)
        self.assertIn("result", playback_response)
        self.assertEqual(self.protocol.calls.count("servo-frame"), 1)
        self.assertEqual(self.protocol.calls.count("stop"), 1)
        self.assertLess(
            self.protocol.calls.index("servo-frame"),
            self.protocol.calls.index("stop"),
        )
        final = self.studio.snapshot()
        self.assertFalse(final["armed"])
        self.assertEqual(final["servo_state"]["enabled_mask"], 0)
        self.assertFalse(final["servo_state"]["active"])
        self.assertEqual(final["servo_state"]["watchdog_ms"], 0)

    def test_servo_test_api_jogs_one_channel_and_confirms_outputs_off(self) -> None:
        self.protocol.calls.clear()
        status, payload = self.request(
            "/api/servo-test",
            "POST",
            servo_test_api_payload(self.studio, channel=2, pulse_us=1420, hold_ms=250),
        )
        self.assertEqual(status, 200, payload)
        result = payload["result"]
        self.assertTrue(result["accepted"])
        self.assertTrue(result["outputs_safe"])
        self.assertEqual(result["channel"], 2)
        self.assertEqual(result["pad"], motion.SERVO_PADS[2])
        self.assertEqual(result["pulse_us"], 1420)
        self.assertEqual(result["servo_state"]["enabled_mask"], 0)
        self.assertFalse(result["servo_state"]["active"])
        self.assertFalse(payload["state"]["armed"])
        self.assertFalse(payload["state"]["safety_off_pending"])
        self.assertEqual(payload["state"]["servo_state"], result["servo_state"])
        self.assertEqual(
            [call for call in self.protocol.calls if call == "power" or call == "state" or (
                isinstance(call, tuple) and call[0] == "servo-test"
            )],
            ["power", ("servo-test", 2, 1420, 250), "state"],
        )

    def test_servo_test_api_rejects_identity_mismatch_and_armed_state(self) -> None:
        self.protocol.calls.clear()
        status, payload = self.request(
            "/api/servo-test",
            "POST",
            servo_test_api_payload(self.studio, connection_epoch=self.studio.connection_epoch + 1),
        )
        self.assertEqual(status, 400, payload)
        self.assertIn("connection_epoch", payload["error"])
        status, payload = self.request(
            "/api/servo-test",
            "POST",
            servo_test_api_payload(self.studio, chip_id="fedcba9876543210"),
        )
        self.assertEqual(status, 400, payload)
        self.assertIn("chip_id", payload["error"])
        self.assertFalse(any(isinstance(item, tuple) and item[0] == "servo-test" for item in self.protocol.calls))

        self.studio.armed = True
        status, payload = self.request(
            "/api/servo-test",
            "POST",
            servo_test_api_payload(self.studio),
        )
        self.assertEqual(status, 400, payload)
        self.assertIn("保持锁定", payload["error"])
        self.assertFalse(any(isinstance(item, tuple) and item[0] == "servo-test" for item in self.protocol.calls))

    def test_servo_test_api_requires_power_ready_before_output(self) -> None:
        self.protocol.power_ready = False
        self.protocol.calls.clear()
        status, payload = self.request(
            "/api/servo-test",
            "POST",
            servo_test_api_payload(self.studio),
        )
        self.assertEqual(status, 400, payload)
        self.assertIn("电源预检未通过", payload["error"])
        self.assertEqual(payload["state"]["power"]["ready"], False)
        self.assertIn("power", self.protocol.calls)
        self.assertFalse(any(isinstance(item, tuple) and item[0] == "servo-test" for item in self.protocol.calls))

    def test_servo_test_api_rejects_playback_and_recording_states(self) -> None:
        self.protocol.calls.clear()
        self.studio.playback["state"] = "playing"
        status, payload = self.request(
            "/api/servo-test",
            "POST",
            servo_test_api_payload(self.studio),
        )
        self.assertEqual(status, 400, payload)
        self.assertIn("播放未停止", payload["error"])

        self.studio.playback["state"] = "stopped"
        self.studio.recording["state"] = "recording"
        status, payload = self.request(
            "/api/servo-test",
            "POST",
            servo_test_api_payload(self.studio),
        )
        self.assertEqual(status, 400, payload)
        self.assertIn("录制未停止", payload["error"])
        self.assertNotIn("power", self.protocol.calls)
        self.assertFalse(any(isinstance(item, tuple) and item[0] == "servo-test" for item in self.protocol.calls))

    def test_servo_test_command_error_runs_and_confirms_safe_off(self) -> None:
        self.protocol.calls.clear()
        with mock.patch.object(
            self.protocol,
            "servo_test",
            side_effect=motion.StudioError("test command failed"),
        ):
            status, payload = self.request(
                "/api/servo-test",
                "POST",
                servo_test_api_payload(self.studio),
            )
        self.assertEqual(status, 400, payload)
        self.assertIn("test command failed", payload["error"])
        self.assertIn("已执行全路安全断开", payload["error"])
        self.assertFalse(payload["state"]["safety_off_pending"])
        self.assertEqual(payload["state"]["servo_state"]["enabled_mask"], 0)
        self.assertFalse(payload["state"]["servo_state"]["active"])
        self.assertEqual(
            [
                item
                for item in self.protocol.calls
                if item in {"all-off", ("watchdog", 0), "rgb-release", "state"}
            ],
            ["all-off", ("watchdog", 0), "rgb-release", "state"],
        )

    def test_servo_test_api_never_reports_success_when_channel_does_not_auto_disable(self) -> None:
        def unsafe_servo_test(channel: int, pulse_us: int, hold_ms: int):
            self.protocol.calls.append(("servo-test", channel, pulse_us, hold_ms))
            self.protocol.state_data.update(
                {
                    "mode": "idle",
                    "active": True,
                    "enabled_mask": 1 << channel,
                    "duration_ms": hold_ms,
                    "elapsed_ms": hold_ms,
                }
            )
            return reply("SERVO_ACK action=test status=0", "ok")

        # Deliberately make cleanup ineffective as well, so the API must keep
        # the real unsafe state and leave the safety confirmation pending.
        with (
            mock.patch.object(self.protocol, "servo_test", side_effect=unsafe_servo_test),
            mock.patch.object(self.protocol, "safe_off", return_value=reply("ok")),
        ):
            status, payload = self.request(
                "/api/servo-test",
                "POST",
                servo_test_api_payload(self.studio),
            )
        self.assertEqual(status, 400, payload)
        self.assertFalse(payload["ok"])
        self.assertIn("未自动关闭", payload["error"])
        self.assertTrue(payload["state"]["safety_off_pending"])
        self.assertTrue(payload["state"]["servo_state"]["active"])
        self.assertEqual(payload["state"]["servo_state"]["enabled_mask"], 1 << 2)

    def test_arm_pose_light_and_estop(self) -> None:
        status, payload = self.request("/api/arm", "POST", {"armed": True})
        self.assertEqual(status, 200)
        self.assertTrue(payload["state"]["armed"])
        status, payload = self.request(
            "/api/pose",
            "POST",
            {"servos_us": [1200, 1300, 1700, 1800], "duration_ms": 180, "easing": "smooth"},
        )
        self.assertEqual(status, 202)
        self.assertTrue(payload["result"]["accepted"])
        status, _ = self.request(
            "/api/light",
            "POST",
            {"enabled": True, "color": "#1133AA", "brightness": 40, "pixel_count": 6},
        )
        self.assertEqual(status, 202)
        stale = {
            **self.protocol.state_data,
            "mode": "frame",
            "active": True,
            "enabled_mask": 0x0F,
            "watchdog_ms": motion.SERVO_LEASE_MS,
            "remaining_ms": 600,
        }
        self.protocol.state_data = dict(stale)
        with self.studio.lock:
            self.studio.servo_state = dict(stale)
        status, payload = self.request("/api/estop", "POST", {})
        self.assertEqual(status, 200)
        self.assertFalse(payload["state"]["armed"])
        self.assertTrue(payload["state"]["estop"])
        self.assertEqual(payload["state"]["servo_state"]["enabled_mask"], 0)
        self.assertFalse(payload["state"]["servo_state"]["active"])
        self.assertEqual(payload["state"]["servo_state"]["watchdog_ms"], 0)
        self.assertFalse(payload["state"]["safety_off_pending"])

    def test_light_endpoint_does_not_require_arm(self) -> None:
        status, payload = self.request(
            "/api/light",
            "POST",
            {"enabled": True, "color": "#1020F0", "brightness": 20, "pixel_count": 3},
        )
        self.assertEqual(status, 202)
        self.assertFalse(payload["state"]["armed"])
        self.assertFalse(self.protocol.frames)
        self.assertTrue(any(isinstance(item, tuple) and item[0] == "light" for item in self.protocol.calls))

    def test_record_stop_updates_selected_action_and_playback_uses_new_frames(self) -> None:
        action = self.studio.create_action(
            {
                "name": "selected",
                "loop": {"enabled": True, "start_ms": 0, "end_ms": 100, "count": 0},
                "default_rate": 1.5,
                "end_behavior": "hold",
                "keyframes": [
                    {
                        "time_ms": 0,
                        "duration_ms": 100,
                        "easing": "linear",
                        "servos_us": [1500] * 4,
                        "led": motion.default_led(),
                    }
                ],
            }
        )
        self.request("/api/arm", "POST", {"armed": True})
        self.request("/api/record/start", "POST", {"action_id": action["id"]})
        status, _ = self.request(
            "/api/pose",
            "POST",
            {
                "servos_us": [1250, 1350, 1650, 1750],
                "duration_ms": 80,
                "easing": "smooth",
                "led": {"enabled": True, "color": "#AA2200", "brightness": 32, "pixel_count": 5},
            },
        )
        self.assertEqual(status, 202)
        self.assertTrue(wait_until(lambda: self.studio.confirmed_pose["servos_us"] == [1250, 1350, 1650, 1750]))
        status, payload = self.request(
            "/api/record/stop",
            "POST",
            {"name": "selected", "action_id": action["id"]},
        )
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["result"]["id"], action["id"])
        self.assertEqual(len(payload["state"]["project"]["actions"]), 1)
        updated = payload["state"]["project"]["actions"][0]
        self.assertTrue(updated["loop"]["enabled"])
        self.assertEqual(updated["default_rate"], 1.5)
        self.assertEqual(updated["keyframes"][-1]["servos_us"], [1250, 1350, 1650, 1750])
        frame_count = len(self.protocol.frames)
        status, payload = self.request(
            "/api/playback",
            "POST",
            {
                "operation": "play",
                "action_id": action["id"],
                "rate": 4,
                "loop": False,
                "loop_count": 1,
            },
        )
        self.assertEqual(status, 200, payload)
        # Playback can emit the initial recorded pose before the final pose.
        # Wait for the requested frame, not merely any frame (Windows race).
        self.assertTrue(wait_until(lambda: any(
            frame["servos_us"] == [1250, 1350, 1650, 1750]
            for frame in self.protocol.frames[frame_count:]
        )))
        self.assertEqual(self.protocol.frames[-1]["servos_us"], [1250, 1350, 1650, 1750])

    def test_invalid_pose_has_uniform_error_envelope(self) -> None:
        self.request("/api/arm", "POST", {"armed": True})
        status, payload = self.request("/api/pose", "POST", {"servos_us": [1500]})
        self.assertEqual(status, 400)
        self.assertFalse(payload["ok"])
        self.assertIsInstance(payload["error"], str)
        self.assertIn("state", payload)

    def test_frontend_adapter_project_put_then_play(self) -> None:
        """Exercise the exact shape emitted by studio.js toBackendProject()."""
        profile_servos = []
        for index, pin in enumerate(motion.SERVO_PADS):
            profile_servos.append(
                {
                    "channel": index,
                    "pin": pin,
                    "name": f"LEG-{index}",
                    "min_us": 1000,
                    "center_us": 1500,
                    "max_us": 2000,
                    "invert": index % 2 == 1,
                    "trim_us": 0,
                }
            )
        converted = {
            "format": "arcs-motion-project",
            "version": 1,
            "id": "ui-project-1",
            "name": "UI converted",
            "created_at": "2026-08-21T10:00:00Z",
            "updated_at": "2026-08-21T10:01:00Z",
            "device_profile": {
                "id": "arcs-mini-v2-1-studio",
                "revision": 1,
                "board": "arcs_mini_v2_1",
                "servo_frequency_hz": 50,
                "servos": profile_servos,
                "led": {
                    "pin": "PA08",
                    "protocol": "ws2812-grb",
                    "pixel_count": 6,
                    "max_pixel_count": 64,
                },
            },
            "actions": [
                {
                    "id": "ui-walk",
                    "name": "UI walk",
                    "description": "converted in browser",
                    "duration_ms": 40,
                    "default_rate": 1,
                    "end_behavior": "hold",
                    "loop": {"enabled": False, "start_ms": 0, "end_ms": 40, "count": 1},
                    "keyframes": [
                        {
                            "id": "ui-frame-1",
                            "time_ms": 0,
                            "duration_ms": 40,
                            "easing": "smooth",
                            "servos_us": [1400, 1600, 1400, 1600],
                            "led": {
                                "enabled": True,
                                "color": "#35A7FF",
                                "brightness": 28,
                                "pixel_count": 6,
                            },
                        }
                    ],
                }
            ],
        }
        status, payload = self.request("/api/project", "PUT", converted)
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["state"]["project"]["id"], "ui-project-1")
        self.assertEqual(payload["state"]["project"]["device_profile"]["servos"][1]["trim_us"], 0)
        self.request("/api/arm", "POST", {"armed": True})
        status, payload = self.request(
            "/api/playback",
            "POST",
            {"operation": "play", "action_id": "ui-walk", "rate": 1, "loop": False, "loop_count": 1},
        )
        self.assertEqual(status, 200, payload)
        self.assertTrue(wait_until(lambda: any(frame.get("source") == "playback" for frame in self.protocol.frames)))
        played = next(frame for frame in self.protocol.frames if frame.get("source") == "playback")
        self.assertEqual(played["servos_us"], [1400, 1600, 1400, 1600])
        self.assertEqual(played["led"]["pixel_count"], 6)


if __name__ == "__main__":
    unittest.main()
