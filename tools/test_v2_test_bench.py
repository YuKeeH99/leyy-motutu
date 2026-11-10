#!/usr/bin/env python3
"""Host-only tests for the V2.1 unified test bench dashboard."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import unittest
import unittest.mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("v2_test_bench.py")
SPEC = importlib.util.spec_from_file_location("v2_test_bench", MODULE_PATH)
assert SPEC and SPEC.loader
bench = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bench
SPEC.loader.exec_module(bench)


TDOA_LEFT = (
    '\x1b[0m TDOA_JSON {"s":168,"v":1,"e":0,"x":1,"d":333,"c":533,"a":14,'
    '"m0":269,"m1":157,"p0":3394,"p1":1401,"b":262,"f":1}\r'
)
TDOA_SILENT = (
    'TDOA_JSON {"s":92,"v":0,"e":0,"x":0,"d":0,"c":767,"a":1,'
    '"m0":63,"m1":59,"p0":1394,"p1":801,"b":0,"f":0}'
)
TOUCH_LOW = "touch PA09: status=0 (ok) level=0"
TOUCH_HIGH = "touch PA09: status=0 (ok) level=1"
TOUCH_PA13_LOW = "touch PA13: status=0 (ok) level=0"
TOUCH_PA13_HIGH = "touch PA13: status=0 (ok) level=1"
MPU_LINE = (
    "mpu6050 i2c0/0x68: status=0 (ok) who=0x68 accel=-2074,-1616,15982 "
    "temp=-3050 gyro=-468,227,-164"
)
PN532_LINE = "pn532 i2c0/0x24: status=0 (ok) ic=0x32 fw=1.6 support=0x07"
POWER_EMPTY = "VBAT/servo pin2=0mV level=n/a battery=no-battery"
POWER_LIVE = "VBAT/servo pin2=3874mV level=82% battery=present"
BOARD_LINE = "board=arcs_mini_v2_1 actuators=ready servos=disabled-by-default"


class ParserTest(unittest.TestCase):
    def test_tdoa_strips_ansi_and_carriage_return(self) -> None:
        payload = bench.parse_tdoa(bench.clean_line(TDOA_LEFT))
        assert payload is not None
        self.assertEqual(payload["s"], 168)
        self.assertEqual(payload["x"], bench.SIDE_MIC0)

    def test_tdoa_rejects_unrelated_and_malformed(self) -> None:
        self.assertIsNone(bench.parse_tdoa("tdoa: no coherent speech"))
        self.assertIsNone(bench.parse_tdoa('TDOA_JSON {"s":1,'))
        self.assertIsNone(bench.parse_tdoa('TDOA_JSON {"v":1}'))

    def test_touch(self) -> None:
        record = bench.parse_touch(TOUCH_HIGH)
        assert record is not None
        self.assertEqual(record["pad"], "PA09")
        self.assertEqual(record["status"], 0)
        self.assertTrue(record["touched"])
        low = bench.parse_touch(TOUCH_LOW)
        assert low is not None
        self.assertFalse(low["touched"])

        pa13 = bench.parse_touch(TOUCH_PA13_HIGH)
        assert pa13 is not None
        self.assertEqual(pa13["pad"], "PA13")
        self.assertEqual(pa13["level"], 1)

    def test_mpu_computes_magnitude_in_g(self) -> None:
        record = bench.parse_mpu(MPU_LINE)
        assert record is not None
        self.assertEqual(record["who"], "0x68")
        self.assertEqual(record["accel"], [-2074, -1616, 15982])
        self.assertEqual(record["gyro"], [-468, 227, -164])
        self.assertEqual(record["temp_raw"], -3050)
        self.assertAlmostEqual(record["accel_g"], 0.99, delta=0.05)

    def test_mpu_error_line_without_samples(self) -> None:
        record = bench.parse_mpu("mpu6050 i2c0/0x68: status=-5 (io error)")
        assert record is not None
        self.assertEqual(record["status"], -5)
        self.assertNotIn("accel", record)

    def test_pn532(self) -> None:
        record = bench.parse_pn532(PN532_LINE)
        assert record is not None
        self.assertEqual(record["ic"], "0x32")
        self.assertEqual(record["firmware"], "1.6")

    def test_power_presence(self) -> None:
        empty = bench.parse_power(POWER_EMPTY)
        assert empty is not None
        self.assertFalse(empty["present"])
        self.assertEqual(empty["millivolt"], 0)
        self.assertIsNone(empty["percent"])
        self.assertEqual(empty["battery_label"], "未接电池")
        live = bench.parse_power(POWER_LIVE)
        assert live is not None
        self.assertTrue(live["present"])
        self.assertAlmostEqual(live["volt"], 3.874)
        self.assertEqual(live["percent"], 82)

    def test_power_charging_states(self) -> None:
        charging = bench.parse_power("VBAT/servo pin2=3932mV level=38% battery=charging")
        assert charging is not None
        self.assertTrue(charging["charging"])
        self.assertFalse(charging["charge_done"])
        self.assertEqual(charging["percent"], 38)
        self.assertEqual(charging["battery_label"], "充电中")

        done = bench.parse_power("VBAT/servo pin2=4180mV level=100% battery=charged")
        assert done is not None
        self.assertFalse(done["charging"])
        self.assertTrue(done["charge_done"])
        self.assertEqual(done["battery_label"], "已充满")

        discharging = bench.parse_power("VBAT/servo pin2=3700mV level=55% battery=not-charging")
        assert discharging is not None
        self.assertFalse(discharging["charging"])
        self.assertTrue(discharging["present"])
        self.assertEqual(discharging["battery_label"], "未充电")

        unknown = bench.parse_power("VBAT/servo pin2=0mV level=n/a battery=unknown")
        assert unknown is not None
        self.assertFalse(unknown["present"])

    def test_board(self) -> None:
        record = bench.parse_board(BOARD_LINE)
        assert record is not None
        self.assertEqual(record["board"], "arcs_mini_v2_1")
        self.assertEqual(record["actuators"], "ready")

    def test_card_uid_is_normalized(self) -> None:
        self.assertEqual(
            bench.parse_card_uid("card found: uid=08 a1 00 01 type=mifare"),
            "08 A1 00 01",
        )
        self.assertIsNone(bench.parse_card_uid("no card"))

    def test_delay_conversion(self) -> None:
        self.assertAlmostEqual(bench.delay_microseconds(1000, 16000), 62.5)
        self.assertLess(bench.delay_microseconds(-770, 16000), 0.0)
        self.assertEqual(bench.delay_microseconds(1000, 0), 0.0)

    def test_snapshot_keeps_independent_session_peak_for_each_physical_mic(self) -> None:
        monitor = bench.TestBench("adb", None, 16000)
        monitor.ingest(TDOA_LEFT)
        monitor.ingest(TDOA_SILENT)

        self.assertEqual(
            {"mic1": 3394, "mic2": 1401},
            monitor.snapshot()["mic_peaks"],
        )

    def test_windows_subprocesses_request_no_console_window(self) -> None:
        self.assertTrue(hasattr(bench, "hidden_subprocess_kwargs"))
        with unittest.mock.patch.object(bench.os, "name", "nt"):
            kwargs = bench.hidden_subprocess_kwargs()
        self.assertEqual(subprocess.CREATE_NO_WINDOW, kwargs["creationflags"])


class CommandBuilderTest(unittest.TestCase):
    def test_rgb_set_clamps_channels(self) -> None:
        self.assertEqual(
            bench.build_command("rgb_set", {"red": 999, "green": -4, "blue": "60"}),
            ["v2hw", "rgb", "set", "255", "0", "60"],
        )

    def test_servo_clamps_channel_and_pulse(self) -> None:
        self.assertEqual(
            bench.build_command("servo_test", {"channel": 9, "pulse_us": 9000, "hold_ms": 400}),
            ["v2hw", "servo", "test", "3", "2500", "400"],
        )
        self.assertEqual(
            bench.build_command("servo_test", {"channel": -2, "pulse_us": 10})[3:5],
            ["0", "500"],
        )

    def test_servo_electrical_boundary_contract(self) -> None:
        self.assertEqual(
            (bench.SERVO_MIN_PULSE_US, bench.SERVO_MAX_PULSE_US),
            (500, 2500),
        )
        self.assertEqual(bench.clamp_pulse(500), 500)
        self.assertEqual(bench.clamp_pulse(2500), 2500)
        self.assertEqual(bench.clamp_pulse(499), 500)
        self.assertEqual(bench.clamp_pulse(2501), 2500)

        limits = bench.TestBench("adb", None, 16000).snapshot()["servo_limits"]
        self.assertEqual(limits["min_pulse_us"], 500)
        self.assertEqual(limits["max_pulse_us"], 2500)

    def test_non_numeric_values_fall_back_to_defaults(self) -> None:
        argv = bench.build_command("servo_test", {"channel": "x", "pulse_us": None})
        self.assertEqual(argv, ["v2hw", "servo", "test", "0", "1500", "500"])

    def test_injection_attempt_cannot_add_shell_words(self) -> None:
        argv = bench.build_command("rgb_set", {"red": "0; reboot", "green": 0, "blue": 0})
        self.assertEqual(argv, ["v2hw", "rgb", "set", "0", "0", "0"])

    def test_peer_subcommands(self) -> None:
        self.assertEqual(
            bench.build_command("nfc_peer_start", {}), ["v2hw", "nfc", "peer", "start"]
        )
        self.assertEqual(
            bench.build_command("nfc_peer_stop", {}), ["v2hw", "nfc", "peer", "stop"]
        )

    def test_durations_are_bounded(self) -> None:
        self.assertEqual(
            bench.build_command("touch_watch", {"duration_ms": 999999})[-1], "20000"
        )
        self.assertEqual(
            bench.build_command("nfc_card_read", {"timeout_ms": 1})[-1], "500"
        )

    def test_touch_watch_targets_both_channels(self) -> None:
        self.assertEqual(
            bench.build_command("touch_watch", {"duration_ms": 5000}),
            ["v2hw", "touch", "watch", "all", "5000"],
        )

    def test_unknown_action(self) -> None:
        self.assertIsNone(bench.build_command("format_flash", {}))


class CloudClassifyTest(unittest.TestCase):
    def test_stages_are_recognized(self) -> None:
        self.assertEqual(bench.classify_cloud_event("voice_cloud_tts_txt, start")[0], "tts")
        self.assertEqual(bench.classify_cloud_event("stream-text thread start")[0], "asr")
        self.assertEqual(bench.classify_cloud_event("voice_wakeup_msg hit")[0], "wakeup")

    def test_plain_line_is_ignored(self) -> None:
        self.assertIsNone(bench.classify_cloud_event("heap: free 12345"))


class IngestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.bench = bench.TestBench("adb", 9, 16000)

    def test_each_line_lands_in_its_own_panel(self) -> None:
        for line in (
            BOARD_LINE, TOUCH_LOW, TOUCH_PA13_HIGH, MPU_LINE, PN532_LINE, POWER_EMPTY, TDOA_LEFT
        ):
            self.bench.ingest(line)
        state = self.bench.snapshot()
        self.assertEqual(state["board"]["board"], "arcs_mini_v2_1")
        self.assertEqual(state["touch"]["level"], 0)
        self.assertEqual(state["touches"]["PA09"]["level"], 0)
        self.assertEqual(state["touches"]["PA13"]["level"], 1)
        self.assertEqual(state["mpu"]["who"], "0x68")
        self.assertEqual(state["nfc"]["ic"], "0x32")
        self.assertFalse(state["power"]["present"])
        self.assertEqual(state["tdoa"]["side_label"], "左")

    def test_battery_changes_are_logged_and_aged(self) -> None:
        self.bench.ingest("VBAT/servo pin2=3932mV level=38% battery=charging")
        self.bench.ingest("VBAT/servo pin2=3940mV level=39% battery=charging")
        self.bench.ingest("VBAT/servo pin2=4180mV level=100% battery=charged")
        state = self.bench.snapshot()
        texts = [item["text"] for item in state["events"] if item["kind"] == "power"]
        self.assertEqual(len(texts), 2)
        self.assertIn("38% -> 39%", texts[0])
        self.assertIn("充电中 -> 已充满", texts[1])
        self.assertIsNotNone(state["power_age"])
        self.assertLess(state["power_age"], 1.0)

    def test_repeated_identical_battery_line_logs_nothing(self) -> None:
        for _ in range(3):
            self.bench.ingest("VBAT/servo pin2=3932mV level=38% battery=charging")
        self.assertEqual(
            [item for item in self.bench.snapshot()["events"] if item["kind"] == "power"], []
        )

    def test_capture_collects_reply_and_stops_at_ok(self) -> None:
        self.bench.capture = []
        self.bench.capture_done = False
        self.bench.ingest("ListenAI:/$ v2hw touch")
        self.bench.ingest("touch PA09: status=0 (ok) level=0")
        self.assertFalse(self.bench.capture_done)
        self.bench.ingest("ok")
        self.assertTrue(self.bench.capture_done)
        self.assertEqual(len(self.bench.capture), 3)

    def test_capture_stops_on_failure_line(self) -> None:
        self.bench.capture = []
        self.bench.capture_done = False
        self.bench.ingest("v2hw command failed: -5")
        self.assertTrue(self.bench.capture_done)

    def test_ingest_still_parses_while_capturing(self) -> None:
        self.bench.capture = []
        self.bench.capture_done = False
        self.bench.ingest("touch PA09: status=0 (ok) level=0")
        self.assertEqual(self.bench.snapshot()["touch"]["level"], 0)

    def test_touch_transition_is_counted_once(self) -> None:
        for line in (TOUCH_LOW, TOUCH_LOW, TOUCH_HIGH, TOUCH_HIGH, TOUCH_LOW):
            self.bench.ingest(line)
        self.assertEqual(self.bench.snapshot()["touch_transitions"], 2)

    def test_touch_channels_have_independent_baselines_and_counts(self) -> None:
        for line in (
            TOUCH_LOW,
            TOUCH_PA13_HIGH,
            TOUCH_LOW,
            TOUCH_PA13_HIGH,
            TOUCH_HIGH,
            TOUCH_PA13_LOW,
        ):
            self.bench.ingest(line)
        state = self.bench.snapshot()
        self.assertEqual(state["touch_transition_counts"], {"PA09": 1, "PA13": 1})

    def test_failed_touch_read_never_creates_a_transition(self) -> None:
        self.bench.ingest(TOUCH_LOW)
        self.bench.ingest("touch PA09: status=-5 (io error) level=1")
        self.assertEqual(self.bench.snapshot()["touch_transition_counts"]["PA09"], 0)
        self.bench.ingest(TOUCH_HIGH)
        self.assertEqual(self.bench.snapshot()["touch_transition_counts"]["PA09"], 1)

    def test_repeated_tdoa_sequence_is_ignored(self) -> None:
        self.bench.ingest(TDOA_LEFT)
        self.bench.ingest(TDOA_LEFT)
        self.assertEqual(self.bench.snapshot()["counts"]["left"], 1)

    def test_silent_report_keeps_last_valid_direction(self) -> None:
        self.bench.ingest(TDOA_LEFT)
        self.bench.ingest(TDOA_SILENT)
        state = self.bench.snapshot()
        self.assertFalse(state["tdoa"]["valid"])
        self.assertEqual(state["tdoa_last_valid"]["side"], bench.SIDE_MIC0)

    def test_history_is_bounded(self) -> None:
        for index in range(bench.MAX_HISTORY + 30):
            self.bench._on_tdoa({"s": index, "v": 1, "x": bench.SIDE_MIC0, "d": 100})
        self.assertEqual(len(self.bench.history), bench.MAX_HISTORY)

    def test_events_are_bounded(self) -> None:
        for index in range(bench.MAX_EVENTS + 20):
            self.bench.ingest(f"voice_cloud_tts_txt, start {index}")
        self.assertEqual(len(self.bench.snapshot()["events"]), bench.MAX_EVENTS)

    def test_card_uid_is_deduplicated(self) -> None:
        self.bench.ingest("uid=08 a1 00 01")
        self.bench.ingest("uid=08 A1 00 01")
        self.assertEqual(self.bench.snapshot()["card_uids"], ["08 A1 00 01"])

    def test_subscriber_receives_initial_and_updated_state(self) -> None:
        channel = self.bench.subscribe()
        self.assertIsNotNone(channel.get_nowait())
        self.bench.ingest(TDOA_LEFT)
        self.assertEqual(channel.get_nowait()["tdoa"]["side_label"], "左")
        self.bench.unsubscribe(channel)
        self.assertEqual(self.bench.channels, [])


class ServoArmingTest(unittest.TestCase):
    def setUp(self) -> None:
        self.bench = bench.TestBench("adb", 9, 16000)

    def test_servo_is_locked_by_default(self) -> None:
        self.assertFalse(self.bench.snapshot()["servo_armed"])
        result = self.bench.run_command("servo_test", {"channel": 0})
        self.assertFalse(result["ok"])
        self.assertIn("锁定", result["error"])

    def test_arming_allows_the_command_through(self) -> None:
        self.bench.set_servo_armed(True)
        self.bench.transport_id = None
        result = self.bench.run_command("servo_test", {"channel": 0})
        self.assertFalse(result["ok"])
        self.assertIn("transport_id", result["error"])

    def test_other_actions_do_not_need_arming(self) -> None:
        self.assertNotIn("rgb_set", bench.ACTIONS_NEEDING_ARM)

    def test_starting_a_sweep_needs_arming_but_stopping_never_does(self) -> None:
        self.assertIn("servo_sweep_start", bench.ACTIONS_NEEDING_ARM)
        # Locking the servo must never trap a running sweep.
        self.assertNotIn("servo_sweep_stop", bench.ACTIONS_NEEDING_ARM)
        result = self.bench.run_command("servo_sweep_start", {"channel": 1})
        self.assertFalse(result["ok"])
        self.assertIn("锁定", result["error"])


class SweepTest(unittest.TestCase):
    def setUp(self) -> None:
        self.bench = bench.TestBench("adb", 9, 16000)

    def test_period_is_clamped_into_the_firmware_range(self) -> None:
        low = bench.build_command("servo_sweep_start", {"channel": 0, "period_ms": 1})
        high = bench.build_command("servo_sweep_start", {"channel": 0, "period_ms": 999999})
        self.assertEqual(low[-1], str(bench.SERVO_SWEEP_MIN_PERIOD_MS))
        self.assertEqual(high[-1], str(bench.SERVO_SWEEP_MAX_PERIOD_MS))

    def test_missing_period_uses_the_default(self) -> None:
        argv = bench.build_command("servo_sweep_start", {"channel": 2})
        self.assertEqual(
            argv, ["v2hw", "servo", "sweep", "2", str(bench.SERVO_SWEEP_DEF_PERIOD_MS)]
        )

    def test_channel_is_clamped(self) -> None:
        argv = bench.build_command("servo_sweep_start", {"channel": 99})
        self.assertEqual(argv[3], str(bench.SERVO_COUNT - 1))

    def test_stop_and_status_take_no_arguments(self) -> None:
        self.assertEqual(
            bench.build_command("servo_sweep_stop", {}), ["v2hw", "servo", "sweep-stop"]
        )
        self.assertEqual(
            bench.build_command("servo_sweep_status", {}),
            ["v2hw", "servo", "sweep-status"],
        )

    def test_dedicated_line_carries_the_channel(self) -> None:
        running = bench.parse_sweep("servo sweep: running channel=3")
        self.assertEqual(running, {"running": True, "channel": 3})
        self.assertEqual(
            bench.parse_sweep("servo sweep: stopped"), {"running": False, "channel": None}
        )
        self.assertIsNone(bench.parse_sweep("board=arcs_mini_v2_1"))

    def test_status_field_flips_state_without_losing_the_channel(self) -> None:
        self.bench.ingest("servo sweep: running channel=2")
        self.assertEqual(self.bench.snapshot()["sweep"], {"running": True, "channel": 2})
        # ``v2hw status`` reports only running/stopped, so a bare stopped field
        # must clear the channel rather than leave a stale one on screen.
        self.bench.ingest(
            "board=arcs_mini_v2_1 actuators=ready "
            "servos=disabled-by-default sweep=stopped"
        )
        self.assertEqual(
            self.bench.snapshot()["sweep"], {"running": False, "channel": None}
        )

    def test_board_line_still_parses_with_the_extra_field(self) -> None:
        record = bench.parse_board(
            "board=arcs_mini_v2_1 actuators=ready "
            "servos=disabled-by-default sweep=running"
        )
        assert record is not None
        self.assertEqual(record["actuators"], "ready")
        self.assertEqual(record["sweep"], "running")


class VolumeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.bench = bench.TestBench("adb", 9, 16000)

    def test_level_is_clamped_to_the_device_range(self) -> None:
        self.assertEqual(bench.build_command("vol_set", {"volume": -20})[-1], "0")
        self.assertEqual(bench.build_command("vol_set", {"volume": 500})[-1], "100")

    def test_non_numeric_level_falls_back_instead_of_reaching_the_shell(self) -> None:
        argv = bench.build_command("vol_set", {"volume": "80; reboot"})
        self.assertEqual(len(argv), 4)
        self.assertTrue(argv[-1].isdigit())

    def test_mute_and_unmute_are_distinct_commands(self) -> None:
        self.assertEqual(bench.build_command("vol_mute", {}), ["v2hw", "vol", "mute"])
        self.assertEqual(bench.build_command("vol_unmute", {}), ["v2hw", "vol", "unmute"])
        self.assertEqual(bench.build_command("vol_get", {}), ["v2hw", "vol"])

    def test_readback_line_populates_the_panel(self) -> None:
        self.bench.ingest("volume=64 min=10 muted=no")
        vol = self.bench.snapshot()["volume"]
        self.assertEqual(vol["volume"], 64)
        self.assertEqual(vol["min"], 10)
        self.assertFalse(vol["muted"])

    def test_mute_reports_zero_below_the_configured_floor(self) -> None:
        # The floor exists to stop a too-high "minimum"; an explicit mute is a
        # different intent and the firmware deliberately bypasses it.
        self.bench.ingest("volume=0 min=10 muted=yes")
        vol = self.bench.snapshot()["volume"]
        self.assertEqual(vol["volume"], 0)
        self.assertTrue(vol["muted"])

    def test_volume_starts_unknown_until_the_device_answers(self) -> None:
        self.assertIsNone(self.bench.snapshot()["volume"])


class NoAckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.bench = bench.TestBench("adb", 9, 16000)
        self.bench.transport_id = 9

    def test_missing_ok_is_not_a_failure_for_interact(self) -> None:
        self.bench._exchange = lambda command, budget: (["voice pcm send enable"], True)
        result = self.bench.run_command("interact_start", {})
        self.assertTrue(result["ok"])
        self.assertNotIn("error", result)

    def test_missing_ok_is_still_a_failure_elsewhere(self) -> None:
        self.bench._exchange = lambda command, budget: (["touch PA09: status=0 (ok) level=0"], True)
        result = self.bench.run_command("power", {})
        self.assertIsNotNone(bench.build_command("power", {}))
        self.assertFalse(result["ok"])
        self.assertIn("超时", result["error"])

    def test_explicit_failure_line_still_fails(self) -> None:
        self.bench._exchange = lambda command, budget: (["v2hw command failed: -5"], False)
        self.assertFalse(self.bench.run_command("interact_start", {})["ok"])


class ArgumentTest(unittest.TestCase):
    def test_defaults(self) -> None:
        args = bench.parse_args([])
        self.assertIsNone(args.transport)
        self.assertEqual(args.sample_rate, 16000)
        self.assertEqual(args.port, 8770)
        self.assertEqual(args.host, "127.0.0.1")

    def test_explicit_transport(self) -> None:
        args = bench.parse_args(["--transport", "15", "--no-browser"])
        self.assertEqual(args.transport, 15)
        self.assertTrue(args.no_browser)


class PageTest(unittest.TestCase):
    def test_every_panel_id_the_script_touches_exists(self) -> None:
        for element_id in (
            "state", "meta", "rgbState", "arm", "servoGo", "servoState",
            "dLeft", "dCenter", "dFront", "tSide", "tDelay", "tCorr", "tActive",
            "tFused", "tTally", "m0", "m1", "bar0", "bar1",
            "mStatus", "mWho", "mAccel", "mG", "mGyro",
            "cube", "yawOn", "mRP", "mYaw",
            "k09Big", "k09Status", "k09Level", "k09Flips", "k09Result",
            "k13Big", "k13Status", "k13Level", "k13Flips", "k13Result",
            "nStatus", "nIc", "nUid",
            "pVolt", "pLevel", "pBattery", "pHint", "pBig", "pBar", "pAge",
            "cStage", "events", "out",
        ):
            self.assertIn(f'id="{element_id}"', bench.HTML, element_id)

    def test_servo_input_and_endpoint_buttons_match_host_limits(self) -> None:
        self.assertIn('id="pulse" min="500" max="2500"', bench.HTML)
        self.assertIn('setPulse(500)">最小 500', bench.HTML)
        self.assertIn('setPulse(2500)">最大 2500', bench.HTML)


class FastRotationTest(unittest.TestCase):
    def test_imu_gets_the_most_slots(self) -> None:
        counts = {cmd: bench.FAST_ROTATION.count(cmd) for cmd in bench.FAST_ROTATION}
        self.assertEqual(counts["v2hw mpu"], 2)
        self.assertEqual(counts["tdoa status"], 1)
        self.assertEqual(counts["v2hw touch all"], 1)

    def test_every_fast_command_is_a_read(self) -> None:
        for cmd in bench.FAST_ROTATION:
            self.assertNotIn("set", cmd)
            self.assertNotIn("test", cmd)

    def test_imu_refresh_stays_under_one_second(self) -> None:
        slots = len(bench.FAST_ROTATION) / bench.FAST_ROTATION.count("v2hw mpu")
        self.assertLess(slots * bench.FAST_INTERVAL, 1.0)


class CommandBudgetTest(unittest.TestCase):
    def test_blocking_commands_get_their_duration_added(self) -> None:
        base = bench.COMMAND_TIMEOUT
        self.assertEqual(bench.command_budget("status", {}), bench.NO_ACK_COLLECT)
        self.assertEqual(bench.command_budget("power", {}), base)
        self.assertEqual(
            bench.command_budget("touch_watch", {"duration_ms": 10000}), base + 10.0
        )
        self.assertEqual(
            bench.command_budget("nfc_card_read", {"timeout_ms": 3000}), base + 3.0
        )
        self.assertEqual(
            bench.command_budget("servo_test", {"hold_ms": 800}), base + 0.8
        )

    def test_interact_gets_a_short_collect_window_not_the_ok_timeout(self) -> None:
        for action in ("interact_start", "interact_stop"):
            self.assertEqual(bench.command_budget(action, {}), bench.NO_ACK_COLLECT)
            self.assertLess(bench.command_budget(action, {}), bench.COMMAND_TIMEOUT)

    def test_out_of_range_duration_is_clamped_not_trusted(self) -> None:
        self.assertEqual(
            bench.command_budget("touch_watch", {"duration_ms": 999999}),
            bench.COMMAND_TIMEOUT + 20.0,
        )
        self.assertEqual(
            bench.command_budget("touch_watch", {"duration_ms": "nope"}),
            bench.COMMAND_TIMEOUT + 5.0,
        )


class AdbEnvironmentTest(unittest.TestCase):
    def test_native_backend_is_the_default(self) -> None:
        with unittest.mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(bench.adb_environment()["ADB_LIBUSB"], "0")

    def test_explicit_choice_is_not_overridden(self) -> None:
        with unittest.mock.patch.dict(os.environ, {"ADB_LIBUSB": "1"}, clear=True):
            self.assertEqual(bench.adb_environment()["ADB_LIBUSB"], "1")


class EchoTest(unittest.TestCase):
    def test_prompt_echo_is_recognised(self) -> None:
        self.assertTrue(bench.is_echo("ListenAI:/$ v2hw touch", "v2hw touch"))
        self.assertTrue(bench.is_echo("v2hw touch", "v2hw touch"))

    def test_reply_is_not_an_echo(self) -> None:
        self.assertFalse(
            bench.is_echo("touch PA09: status=0 (ok) level=1", "v2hw touch")
        )
        self.assertFalse(bench.is_echo("ok", "v2hw touch"))


if __name__ == "__main__":
    unittest.main()
