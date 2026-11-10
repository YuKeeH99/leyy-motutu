#!/usr/bin/env python3
"""Host-only tests for the NFC peer speed monitor."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("nfc_speed_test.py")
SPEC = importlib.util.spec_from_file_location("nfc_speed_test", MODULE_PATH)
assert SPEC and SPEC.loader
speed = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = speed
SPEC.loader.exec_module(speed)


def _device(transport: int, usb: str) -> dict[str, object]:
    return {"transport_id": transport, "serial": "duplicate", "usb": usb}


class ParsingTests(unittest.TestCase):
    def test_real_firmware_field_order_and_ansi_are_parsed(self) -> None:
        line = (
            "\x1b[36mI/v2.nfc.peer [00:00:03.010] "
            "NFC_PEER_RECOGNIZED "
            f"local={speed.READER_ID} peer={speed.CURRENT_TARGET_ID} "
            "count=7 latency_ms=43\x1b[0m\r\n"
        )
        with mock.patch.object(speed.time, "time", return_value=123.5):
            event = speed.parse_recognized_event(line)

        self.assertEqual(
            event,
            {
                "local": speed.READER_ID,
                "peer": speed.CURRENT_TARGET_ID,
                "latency_ms": 43,
                "count": 7,
                "received_at": 123.5,
            },
        )

    def test_parser_is_field_order_independent(self) -> None:
        event = speed.parse_recognized_event(
            "NFC_PEER_RECOGNIZED latency_ms=9 count=2 extra=ok "
            f"peer={speed.LEGACY_TARGET_ID} local={speed.READER_ID}"
        )
        self.assertIsNotNone(event)
        assert event is not None
        self.assertEqual(event["latency_ms"], 9)
        self.assertEqual(event["count"], 2)

    def test_parser_accepts_continuous_exchange_events(self) -> None:
        event = speed.parse_recognized_event(
            "NFC_PEER_EXCHANGE "
            f"local={speed.READER_ID} peer={speed.CURRENT_TARGET_ID} "
            "count=8 latency_ms=17"
        )
        self.assertIsNotNone(event)
        assert event is not None
        self.assertEqual(event["count"], 8)
        self.assertEqual(event["latency_ms"], 17)

    def test_unrelated_or_incomplete_line_is_ignored(self) -> None:
        self.assertIsNone(speed.parse_recognized_event("PN532 peer INDATA_OK"))
        self.assertIsNone(
            speed.parse_recognized_event(
                f"NFC_PEER_RECOGNIZED local={speed.READER_ID} latency_ms=5"
            )
        )

    def test_duplicate_serial_transports_remain_distinct(self) -> None:
        completed = mock.Mock(
            stdout=(
                "List of devices attached\n"
                "ARCS device usb:1-2 product:x transport_id:8\n"
                "ARCS device usb:1-1 product:x transport_id:7\n"
            )
        )
        with mock.patch.object(speed.subprocess, "run", return_value=completed):
            devices = speed.discover_transports("adb")
        self.assertEqual([d["transport_id"] for d in devices], [7, 8])
        self.assertEqual(
            speed.adb_environment()["ADB_LIBUSB"],
            "0",
        )


class ControllerStatsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.controller = speed.TestController(
            "adb", speed.READER_ID, speed.CURRENT_TARGET_ID
        )
        self.controller.state["phase"] = "running"

    def _event(self, latency: int, count: int = 1) -> str:
        return (
            "I/v2.nfc.peer NFC_PEER_RECOGNIZED "
            f"local={speed.READER_ID} peer={speed.CURRENT_TARGET_ID} "
            f"count={count} latency_ms={latency}"
        )

    def test_reader_events_update_all_statistics_and_time(self) -> None:
        with mock.patch.object(
            speed.time, "time", side_effect=[100.0, 101.0, 102.0, 103.0]
        ):
            for index, latency in enumerate((42, 18, 60, 20), start=1):
                self.controller._on_line("reader", self._event(latency, index))

        state = self.controller.snapshot()
        self.assertEqual(state["count"], 4)
        self.assertEqual(state["latest_ms"], 20)
        self.assertEqual(state["min_ms"], 18)
        self.assertEqual(state["max_ms"], 60)
        self.assertEqual(state["avg_ms"], 35)
        self.assertEqual(state["median_ms"], 31)
        self.assertEqual(state["last_event_time"], 103.0)
        self.assertEqual(state["events"][-1]["board_count"], 4)
        self.assertTrue(state["reader_connected"])
        self.assertEqual(state["measurement_source"], "board_event_latency_ms")

    def test_target_stream_and_wrong_peer_do_not_create_samples(self) -> None:
        self.controller._on_line("target", self._event(5))
        self.controller._on_line(
            "reader",
            "NFC_PEER_RECOGNIZED "
            f"local={speed.READER_ID} peer=0123456789abcdef "
            "count=1 latency_ms=6",
        )
        self.assertEqual(self.controller.snapshot()["count"], 0)

    def test_long_run_keeps_total_count_with_bounded_sample_history(self) -> None:
        for index in range(speed.MAX_SAMPLE_HISTORY + 2):
            self.controller._on_line(
                "reader", self._event(1 if index == 0 else 2, index + 1)
            )

        state = self.controller.snapshot()
        self.assertEqual(state["count"], speed.MAX_SAMPLE_HISTORY + 2)
        self.assertEqual(len(state["samples"]), speed.MAX_SAMPLE_HISTORY)
        self.assertEqual(state["avg_ms"], 2.0)
        self.assertEqual(state["min_ms"], 1)
        self.assertEqual(state["max_ms"], 2)

    def test_device_error_and_stream_close_are_visible(self) -> None:
        self.controller._on_line(
            "reader", "W/v2.nfc.peer role=reader failed: -3 (I/O error)"
        )
        self.controller._on_line("reader", "__stream_closed__")
        state = self.controller.snapshot()
        self.assertFalse(state["reader_connected"])
        self.assertEqual(len(state["errors"]), 2)
        self.assertIn("事件流已断开", state["last_error"])
        self.assertEqual(state["phase"], "error")

    def test_expected_stream_close_during_stop_is_not_reported_as_error(self) -> None:
        self.controller._stopping = True
        self.controller._on_line("reader", "__stream_closed__")
        state = self.controller.snapshot()
        self.assertEqual(state["phase"], "running")
        self.assertEqual(state["errors"], [])


class PairSelectionTests(unittest.TestCase):
    def _find(
        self,
        controller: speed.TestController,
        identities: dict[int, str],
    ) -> tuple[dict[str, object], dict[str, object]]:
        devices = [_device(1, "1-1"), _device(2, "1-2"), _device(3, "1-3")]
        with (
            mock.patch.object(speed, "discover_transports", return_value=devices),
            mock.patch.object(
                speed,
                "identify_transport",
                side_effect=lambda _adb, device: identities[int(device["transport_id"])],
            ),
        ):
            return controller._find_pair()

    def test_default_prefers_current_target(self) -> None:
        controller = speed.TestController("adb")
        reader, target = self._find(
            controller,
            {
                1: speed.LEGACY_TARGET_ID,
                2: speed.READER_ID,
                3: speed.CURRENT_TARGET_ID,
            },
        )
        self.assertEqual(reader["transport_id"], 2)
        self.assertEqual(target["transport_id"], 3)
        self.assertEqual(controller.snapshot()["target_id"], speed.CURRENT_TARGET_ID)

    def test_default_falls_back_to_legacy_target(self) -> None:
        controller = speed.TestController("adb")
        _, target = self._find(
            controller,
            {1: speed.LEGACY_TARGET_ID, 2: speed.READER_ID, 3: "0" * 16},
        )
        self.assertEqual(target["transport_id"], 1)
        self.assertEqual(controller.snapshot()["target_id"], speed.LEGACY_TARGET_ID)

    def test_explicit_target_transport_is_verified_and_selected(self) -> None:
        controller = speed.TestController(
            "adb", target_id=speed.LEGACY_TARGET_ID, target_transport=1
        )
        _, target = self._find(
            controller,
            {1: speed.LEGACY_TARGET_ID, 2: speed.READER_ID, 3: speed.CURRENT_TARGET_ID},
        )
        self.assertEqual(target["transport_id"], 1)

    def test_explicit_transport_id_mismatch_fails(self) -> None:
        controller = speed.TestController(
            "adb", target_id=speed.LEGACY_TARGET_ID, target_transport=3
        )
        with self.assertRaisesRegex(RuntimeError, "不一致"):
            self._find(
                controller,
                {1: speed.LEGACY_TARGET_ID, 2: speed.READER_ID, 3: speed.CURRENT_TARGET_ID},
            )


class _FakeStream:
    calls: list[tuple[str, str]] = []
    fail_role: str | None = None

    def __init__(self, _adb, _device, role, _on_line) -> None:
        self.role = role

    def start(self) -> None:
        self.calls.append((self.role, "stream-start"))
        if self.role == self.fail_role:
            raise RuntimeError(f"{self.role} stream failed")

    def send_and_wait(self, command: str, _success: str, _timeout: float) -> str:
        self.calls.append((self.role, command))
        if command == "v2hw nfc peer status":
            return "nfc peer: enabled=0 role=idle local=0000000000000000"
        return "nfc peer discovery started"

    def stop(self) -> None:
        self.calls.append((self.role, "stream-stop"))


class StartupTests(unittest.TestCase):
    def test_target_is_started_before_reader(self) -> None:
        controller = speed.TestController("adb")
        reader, target = _device(7, "1-1"), _device(8, "1-2")
        _FakeStream.calls = []
        with (
            mock.patch.object(controller, "_find_pair", return_value=(reader, target)),
            mock.patch.object(speed, "EventStream", _FakeStream),
            mock.patch.object(speed.time, "sleep"),
        ):
            state = controller.start()

        target_start = _FakeStream.calls.index(("target", "v2hw nfc peer start"))
        reader_start = _FakeStream.calls.index(("reader", "v2hw nfc peer start"))
        self.assertLess(target_start, reader_start)
        self.assertEqual(state["phase"], "running")
        self.assertEqual(state["reader_transport"], 7)
        self.assertEqual(state["target_transport"], 8)

    def test_partial_start_is_cleaned_up(self) -> None:
        controller = speed.TestController("adb")
        reader, target = _device(7, "1-1"), _device(8, "1-2")
        _FakeStream.calls = []
        _FakeStream.fail_role = "target"
        try:
            with (
                mock.patch.object(controller, "_find_pair", return_value=(reader, target)),
                mock.patch.object(speed, "EventStream", _FakeStream),
                mock.patch.object(speed.time, "sleep"),
            ):
                state = controller.start()
        finally:
            _FakeStream.fail_role = None

        self.assertEqual(state["phase"], "error")
        self.assertIn(("reader", "stream-stop"), _FakeStream.calls)
        self.assertIn(("target", "stream-stop"), _FakeStream.calls)

    def test_event_stream_stop_waits_for_firmware_marker(self) -> None:
        stream = speed.EventStream("adb", _device(7, "1-1"), "reader", lambda *_: None)
        process = mock.Mock()
        process.poll.return_value = None
        stream.process = process
        with (
            mock.patch.object(stream, "send_and_wait", return_value="nfc peer discovery stopped") as wait,
            mock.patch.object(stream, "send") as send,
        ):
            stream.stop()

        wait.assert_called_once_with(
            "v2hw nfc peer stop", "nfc peer discovery stopped", 8.0
        )
        send.assert_called_once_with("exit")

    def test_required_cli_flags_are_supported(self) -> None:
        args = speed.parse_args(
            [
                "--adb", "./adb",
                "--port", "8877",
                "--no-browser",
                "--target-id", speed.LEGACY_TARGET_ID,
                "--target-transport", "9",
            ]
        )
        self.assertEqual(args.adb, "./adb")
        self.assertEqual(args.port, 8877)
        self.assertTrue(args.no_browser)
        self.assertEqual(args.target_transport, 9)


if __name__ == "__main__":
    unittest.main()
