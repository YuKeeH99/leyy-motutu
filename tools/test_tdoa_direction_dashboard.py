#!/usr/bin/env python3
"""Host-only tests for the dual-microphone direction dashboard."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("tdoa_direction_dashboard.py")
SPEC = importlib.util.spec_from_file_location("tdoa_direction_dashboard", MODULE_PATH)
assert SPEC and SPEC.loader
dash = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dash
SPEC.loader.exec_module(dash)


LEFT_LINE = (
    '\x1b[0m TDOA_JSON {"s":168,"v":1,"e":0,"x":1,"d":333,"c":533,"a":14,'
    '"m0":269,"m1":157,"p0":3394,"p1":1401,"b":262,"f":1}\r'
)
FRONT_LINE = (
    'TDOA_JSON {"s":220,"v":1,"e":0,"x":3,"d":-770,"c":464,"a":7,'
    '"m0":250,"m1":462,"p0":1558,"p1":3804,"b":-297,"f":1}'
)
SILENT_LINE = (
    'TDOA_JSON {"s":92,"v":0,"e":0,"x":0,"d":0,"c":767,"a":1,'
    '"m0":63,"m1":59,"p0":1394,"p1":801,"b":0,"f":0}'
)


class ParseSnapshotTest(unittest.TestCase):
    def test_strips_ansi_and_carriage_return(self) -> None:
        payload = dash.parse_snapshot(LEFT_LINE)
        self.assertIsNotNone(payload)
        assert payload is not None
        self.assertEqual(payload["s"], 168)
        self.assertEqual(payload["x"], dash.SIDE_MIC0)

    def test_rejects_unrelated_lines(self) -> None:
        self.assertIsNone(dash.parse_snapshot("tdoa: no coherent speech"))
        self.assertIsNone(dash.parse_snapshot(""))

    def test_rejects_malformed_json(self) -> None:
        self.assertIsNone(dash.parse_snapshot('TDOA_JSON {"s":1,'))

    def test_rejects_payload_without_sequence(self) -> None:
        self.assertIsNone(dash.parse_snapshot('TDOA_JSON {"v":1}'))


class DelayConversionTest(unittest.TestCase):
    def test_one_sample_at_16k_is_62_5_microseconds(self) -> None:
        self.assertAlmostEqual(dash.delay_microseconds(1000, 16000), 62.5)

    def test_sign_is_preserved(self) -> None:
        self.assertLess(dash.delay_microseconds(-770, 16000), 0.0)

    def test_zero_sample_rate_does_not_divide(self) -> None:
        self.assertEqual(dash.delay_microseconds(1000, 0), 0.0)


class DecorateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.monitor = dash.DirectionMonitor("adb", 9, 16000)

    def test_left_snapshot_maps_to_mic0(self) -> None:
        payload = dash.parse_snapshot(LEFT_LINE)
        assert payload is not None
        record = self.monitor._decorate(payload)
        self.assertTrue(record["valid"])
        self.assertEqual(record["side"], dash.SIDE_MIC0)
        self.assertEqual(record["side_label"], "左")
        self.assertGreater(record["delay_milli"], 0)
        self.assertGreater(record["level_mic0"], record["level_mic1"])
        self.assertTrue(record["fused"])

    def test_front_snapshot_maps_to_mic1(self) -> None:
        payload = dash.parse_snapshot(FRONT_LINE)
        assert payload is not None
        record = self.monitor._decorate(payload)
        self.assertEqual(record["side"], dash.SIDE_MIC1)
        self.assertEqual(record["side_label"], "前")
        self.assertLess(record["delay_milli"], 0)
        self.assertLess(record["balance_milli"], 0)

    def test_silent_snapshot_is_not_valid(self) -> None:
        payload = dash.parse_snapshot(SILENT_LINE)
        assert payload is not None
        record = self.monitor._decorate(payload)
        self.assertFalse(record["valid"])
        self.assertEqual(record["side"], dash.SIDE_UNKNOWN)


class SnapshotFanoutTest(unittest.TestCase):
    def setUp(self) -> None:
        self.monitor = dash.DirectionMonitor("adb", 9, 16000)

    def test_counts_only_valid_reports(self) -> None:
        for line in (LEFT_LINE, FRONT_LINE, SILENT_LINE):
            payload = dash.parse_snapshot(line)
            assert payload is not None
            self.monitor._on_snapshot(payload)
        state = self.monitor.snapshot()
        self.assertEqual(state["counts"], {"left": 1, "center": 0, "front": 1})
        self.assertEqual(len(state["history"]), 2)

    def test_repeated_sequence_is_ignored(self) -> None:
        payload = dash.parse_snapshot(LEFT_LINE)
        assert payload is not None
        self.monitor._on_snapshot(payload)
        self.monitor._on_snapshot(dict(payload))
        self.assertEqual(self.monitor.snapshot()["counts"]["left"], 1)

    def test_silent_report_keeps_last_valid_direction(self) -> None:
        for line in (FRONT_LINE, SILENT_LINE):
            payload = dash.parse_snapshot(line)
            assert payload is not None
            self.monitor._on_snapshot(payload)
        state = self.monitor.snapshot()
        self.assertFalse(state["latest"]["valid"])
        self.assertEqual(state["last_valid"]["side"], dash.SIDE_MIC1)

    def test_subscriber_receives_initial_and_updated_state(self) -> None:
        channel = self.monitor.subscribe()
        self.assertIsNotNone(channel.get_nowait())
        payload = dash.parse_snapshot(LEFT_LINE)
        assert payload is not None
        self.monitor._on_snapshot(payload)
        update = channel.get_nowait()
        self.assertEqual(update["latest"]["side_label"], "左")
        self.monitor.unsubscribe(channel)
        self.assertEqual(self.monitor.channels, [])

    def test_history_is_bounded(self) -> None:
        for index in range(dash.MAX_HISTORY + 40):
            self.monitor._on_snapshot(
                {"s": index, "v": 1, "x": dash.SIDE_MIC0, "d": 100}
            )
        self.assertEqual(len(self.monitor.history), dash.MAX_HISTORY)


class ArgumentTest(unittest.TestCase):
    def test_defaults(self) -> None:
        args = dash.parse_args([])
        self.assertIsNone(args.transport)
        self.assertEqual(args.sample_rate, 16000)
        self.assertEqual(args.port, 8767)

    def test_explicit_transport(self) -> None:
        args = dash.parse_args(["--transport", "9", "--no-browser"])
        self.assertEqual(args.transport, 9)
        self.assertTrue(args.no_browser)


if __name__ == "__main__":
    unittest.main()
