#!/usr/bin/env python3
"""Mock regressions for the NFC peer dashboard host-side state machine."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("nfc_peer_dashboard.py")
SPEC = importlib.util.spec_from_file_location("nfc_peer_dashboard", MODULE_PATH)
assert SPEC and SPEC.loader
dashboard = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = dashboard
SPEC.loader.exec_module(dashboard)


class _CardTestStub:
    def stop(self, wait_seconds: float = 0.0) -> bool:
        return True


class _StoreStub:
    def get(self) -> dict[str, object]:
        return {"devices": []}


def _device(
    local_id: str,
    *,
    seen: int = 0,
    enter: int = 0,
    leave: int = 0,
    age_ms: int | None = None,
    peer_id: str | None = None,
) -> dict[str, object]:
    return {
        "online": True,
        "stale": False,
        "transport_id": 1 if local_id == dashboard.PEER_READER_ID else 2,
        "serial": "duplicate",
        "usb": "1-1" if local_id == dashboard.PEER_READER_ID else "1-2",
        "local_id": local_id,
        "enabled": True,
        "present": age_ms is not None,
        "peer_id": peer_id,
        "age_ms": age_ms,
        "seen": seen,
        "enter": enter,
        "leave": leave,
        "last_error": 0,
    }


class PeerTestControllerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.controller = dashboard.PeerTestController(
            "adb", _StoreStub(), _CardTestStub()
        )
        self.controller.state.update(
            phase="waiting",
            setup_active=False,
            baseline_reader_enter=0,
            baseline_reader_seen=0,
            baseline_reader_leave=0,
            baseline_target_enter=0,
            baseline_target_seen=0,
            baseline_target_leave=0,
        )

    def test_reader_seen_immediately_recognizes_before_target(self) -> None:
        reader = _device(
            dashboard.PEER_READER_ID,
            seen=1,
            age_ms=42,
            peer_id=dashboard.PEER_TARGET_ID,
        )
        target = _device(dashboard.PEER_TARGET_ID)

        with mock.patch.object(dashboard.time, "time", return_value=100.0):
            self.controller._observe([reader, target])

        state = self.controller.get()
        self.assertEqual(state["phase"], "recognized")
        self.assertEqual(state["success_sequence"], 1)
        self.assertEqual(state["latency_ms"], 42)
        self.assertAlmostEqual(state["firmware_seen_at"], 99.958, places=3)
        self.assertFalse(state["target_confirmed"])

    def test_target_can_confirm_after_reader_popup(self) -> None:
        reader = _device(
            dashboard.PEER_READER_ID,
            seen=1,
            age_ms=20,
            peer_id=dashboard.PEER_TARGET_ID,
        )
        target = _device(dashboard.PEER_TARGET_ID)
        with mock.patch.object(dashboard.time, "time", return_value=10.0):
            self.controller._observe([reader, target])

        confirmed_target = _device(
            dashboard.PEER_TARGET_ID,
            seen=1,
            age_ms=35,
            peer_id=dashboard.PEER_READER_ID,
        )
        with mock.patch.object(dashboard.time, "time", return_value=10.2):
            self.controller._observe([reader, confirmed_target])

        state = self.controller.get()
        self.assertTrue(state["target_confirmed"])
        self.assertEqual(state["target_confirmed_at"], 10.2)
        self.assertEqual(state["success_sequence"], 1)

    def test_age_gap_rearms_seen_without_waiting_for_firmware_leave(self) -> None:
        target = _device(dashboard.PEER_TARGET_ID)
        first = _device(
            dashboard.PEER_READER_ID,
            seen=1,
            age_ms=10,
            peer_id=dashboard.PEER_TARGET_ID,
        )
        self.controller._observe([first, target])

        # Repeated exchanges from one continuous touch stay suppressed.
        held = _device(
            dashboard.PEER_READER_ID,
            seen=5,
            age_ms=100,
            peer_id=dashboard.PEER_TARGET_ID,
        )
        self.controller._observe([held, target])
        self.assertEqual(self.controller.get()["success_sequence"], 1)

        # Reader age grows while absent even though firmware `present` remains
        # latched for 6 seconds. The UI rearms at its shorter age threshold.
        away = _device(
            dashboard.PEER_READER_ID,
            seen=5,
            age_ms=dashboard.PEER_UI_REARM_AGE_MS,
            peer_id=dashboard.PEER_TARGET_ID,
        )
        self.controller._observe([away, target])
        self.assertEqual(self.controller.get()["phase"], "left")

        retap = _device(
            dashboard.PEER_READER_ID,
            seen=6,
            age_ms=15,
            peer_id=dashboard.PEER_TARGET_ID,
        )
        self.controller._observe([retap, target])
        state = self.controller.get()
        self.assertEqual(state["phase"], "recognized")
        self.assertEqual(state["success_sequence"], 2)

    def test_explicit_wrong_reader_identity_never_recognizes(self) -> None:
        reader = _device(
            dashboard.PEER_READER_ID,
            seen=1,
            age_ms=5,
            peer_id="0123456789abcdef",
        )
        self.controller._observe([reader, _device(dashboard.PEER_TARGET_ID)])
        self.assertEqual(self.controller.get()["phase"], "waiting")


class _RecordingStore:
    def __init__(self, ordered: list[dashboard.AdbDevice]) -> None:
        self.ordered = ordered
        self.updates: list[tuple[list[dict[str, object]], str | None]] = []

    def order_devices_for_poll(
        self, devices: list[dashboard.AdbDevice]
    ) -> list[dashboard.AdbDevice]:
        return self.ordered

    def update(
        self,
        devices: list[dict[str, object]],
        poll_error: str | None = None,
    ) -> None:
        self.updates.append((devices, poll_error))


class PollerTests(unittest.TestCase):
    def test_reader_result_is_published_before_slow_target_failure(self) -> None:
        reader_device = dashboard.AdbDevice(1, "duplicate", "1-1")
        target_device = dashboard.AdbDevice(2, "duplicate", "1-2")
        store = _RecordingStore([reader_device, target_device])
        poller = dashboard.Poller("adb", 0.25, store)
        reader_status = _device(
            dashboard.PEER_READER_ID, seen=1, age_ms=12
        )

        with (
            mock.patch.object(
                dashboard,
                "discover_devices",
                return_value=[target_device, reader_device],
            ),
            mock.patch.object(
                dashboard,
                "query_device",
                side_effect=[
                    reader_status,
                    subprocess.TimeoutExpired(["adb"], 0.65),
                ],
            ),
        ):
            poller.poll_once()

        self.assertEqual(len(store.updates), 2)
        first_devices, _ = store.updates[0]
        self.assertEqual(len(first_devices), 1)
        self.assertEqual(first_devices[0]["local_id"], dashboard.PEER_READER_ID)
        final_devices, _ = store.updates[1]
        self.assertEqual(len(final_devices), 2)
        self.assertFalse(final_devices[1]["online"])

    def test_known_reader_usb_is_polled_first(self) -> None:
        store = dashboard.StateStore()
        store.local_by_usb = {
            "1-1": dashboard.PEER_TARGET_ID,
            "1-2": dashboard.PEER_READER_ID,
        }
        ordered = store.order_devices_for_poll([
            dashboard.AdbDevice(1, "duplicate", "1-1"),
            dashboard.AdbDevice(2, "duplicate", "1-2"),
        ])
        self.assertEqual(ordered[0].usb, "1-2")


if __name__ == "__main__":
    unittest.main()
