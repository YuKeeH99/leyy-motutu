#!/usr/bin/env python3
"""Pure host-side tests for the minimal NFC identity monitor."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("nfc_identity_monitor.py")
SPEC = importlib.util.spec_from_file_location("nfc_identity_monitor", MODULE_PATH)
assert SPEC and SPEC.loader
monitor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = monitor
SPEC.loader.exec_module(monitor)


def _status(local_id: str, seen: int, leave: int = 0):
    return monitor.PeerStatus(local_id=local_id, seen=seen, leave=leave)


class _FakePopen:
    def __init__(self, output: str = "", returncode: int = 0,
                 timeout_once: bool = False) -> None:
        self.output = output
        self.returncode = returncode
        self.pid = 12345
        self.timeout_once = timeout_once
        self.killed = False

    def kill(self):
        self.killed = True

    def communicate(self, timeout=None):
        if self.timeout_once and timeout is not None:
            self.timeout_once = False
            raise subprocess.TimeoutExpired(["adb"], timeout)
        return self.output, None


class ParsingTests(unittest.TestCase):
    def test_duplicate_serial_devices_keep_distinct_transports_and_usb(self) -> None:
        devices = monitor.parse_adb_devices(
            "List of devices attached\n"
            "ARCS device usb:1-2 product:x transport_id:8\n"
            "ARCS device usb:1-1 product:x transport_id:7\n"
        )

        self.assertEqual(
            devices,
            [
                monitor.AdbDevice(7, "ARCS", "1-1"),
                monitor.AdbDevice(8, "ARCS", "1-2"),
            ],
        )

    def test_peer_status_parser_ignores_noise_and_ansi(self) -> None:
        output = (
            "shell> \x1b[32mnfc peer: enabled=1 role=reader "
            "local=6B3BC01B1634CFBD present=1 "
            "peer=6b3bc01b1d41cfbd age=14 seen=23 enter=2 leave=1 "
            "advertised=0 last_error=-3\x1b[0m\r\n"
        )

        self.assertEqual(
            monitor.parse_peer_status(output),
            _status(monitor.READER_ID, seen=23, leave=1),
        )

    def test_interrupted_status_is_retried_immediately(self) -> None:
        valid = (
            "nfc peer: enabled=1 role=reader "
            f"local={monitor.READER_ID} present=0 peer=none age=n/a "
            "seen=9 enter=1 leave=1 advertised=0 last_error=0\n"
        )
        processes = [
            _FakePopen("split log only\n"),
            _FakePopen(valid),
        ]

        with mock.patch.object(
            monitor.subprocess, "Popen", side_effect=processes
        ) as popen:
            status = monitor.AdbClient().query_peer(
                monitor.AdbDevice(2, "duplicate", "1-2")
            )

        self.assertEqual(status, _status(monitor.READER_ID, 9, 1))
        self.assertEqual(popen.call_count, 2)

    def test_one_timeout_is_retried_immediately(self) -> None:
        valid = (
            "nfc peer: enabled=1 role=reader "
            f"local={monitor.READER_ID} present=0 peer=none age=n/a "
            "seen=12 enter=2 leave=2 advertised=0 last_error=0\n"
        )

        timed_out = _FakePopen(timeout_once=True)
        with mock.patch.object(
            monitor.subprocess, "Popen",
            side_effect=[timed_out, _FakePopen(valid)],
        ) as popen, mock.patch.object(monitor.os, "killpg", create=True) as killpg:
            status = monitor.AdbClient().query_peer(
                monitor.AdbDevice(2, "duplicate", "1-2")
            )

        self.assertEqual(status, _status(monitor.READER_ID, 12, 2))
        self.assertEqual(popen.call_count, 2)
        if monitor.os.name == "nt":
            self.assertTrue(timed_out.killed)
            killpg.assert_not_called()
        else:
            killpg.assert_called_once()


class RecognitionGateTests(unittest.TestCase):
    def test_first_seen_rise_notifies_immediately(self) -> None:
        gate = monitor.RecognitionGate(rearm_seconds=1.0)
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 10), 0.0))
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 11), 0.05))

    def test_continuous_seen_rises_only_notify_once(self) -> None:
        gate = monitor.RecognitionGate(rearm_seconds=1.0)
        gate.observe(_status(monitor.READER_ID, 0), 0.0)
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 1), 0.1))
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 2), 0.2))
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 8), 0.9))

    def test_quiet_reader_rearms_for_next_encounter(self) -> None:
        gate = monitor.RecognitionGate(rearm_seconds=1.0)
        gate.observe(_status(monitor.READER_ID, 0), 0.0)
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 1), 0.1))
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 1), 0.2))
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 1), 1.2))
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 2), 1.25))

    def test_explicit_leave_rearms_without_waiting(self) -> None:
        gate = monitor.RecognitionGate(rearm_seconds=5.0)
        gate.observe(_status(monitor.READER_ID, 3), 0.0)
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 4), 0.1))
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 4, 1), 0.2))
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 5, 1), 0.25))

    def test_adb_gap_does_not_count_as_quiet_rearm(self) -> None:
        gate = monitor.RecognitionGate(rearm_seconds=1.0)
        gate.observe(_status(monitor.READER_ID, 0), 0.0)
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 1), 0.1))
        gate.mark_unavailable()
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 7), 5.0))

    def test_counter_reset_establishes_new_baseline(self) -> None:
        gate = monitor.RecognitionGate()
        gate.observe(_status(monitor.READER_ID, 10, 3), 0.0)
        self.assertFalse(gate.observe(_status(monitor.READER_ID, 0, 0), 0.1))
        self.assertTrue(gate.observe(_status(monitor.READER_ID, 1, 0), 0.2))


class _FakeClient:
    def __init__(self, devices, statuses) -> None:
        self.devices = devices
        self.statuses = statuses
        self.queries: list[int] = []

    def discover(self):
        return list(self.devices)

    def query_peer(self, device):
        self.queries.append(device.transport_id)
        response = self.statuses[device.transport_id]
        if isinstance(response, list):
            return response.pop(0)
        return response


class MonitorWorkerTests(unittest.TestCase):
    def test_high_frequency_poll_only_queries_reader_after_discovery(self) -> None:
        target = monitor.AdbDevice(1, "duplicate", "1-1")
        reader = monitor.AdbDevice(2, "duplicate", "1-2")
        client = _FakeClient(
            [target, reader],
            {
                1: _status("6b3bc01b1d41cfbd", 4),
                2: [
                    _status(monitor.READER_ID, 10),
                    _status(monitor.READER_ID, 11),
                    _status(monitor.READER_ID, 12),
                ],
            },
        )
        notifications: list[str] = []
        worker = monitor.MonitorWorker(
            client, monitor.RecognitionGate(),
            lambda: notifications.append(monitor.SUCCESS_TEXT),
        )

        worker.poll_once(0.0)
        client.queries.clear()
        worker.poll_once(0.1)
        worker.poll_once(0.2)

        self.assertEqual(client.queries, [2, 2])
        self.assertEqual(notifications, [monitor.SUCCESS_TEXT])

    def test_replug_rediscovers_changed_transport_by_reader_usb(self) -> None:
        target = monitor.AdbDevice(1, "duplicate", "1-1")
        old_reader = monitor.AdbDevice(2, "duplicate", "1-2")
        client = _FakeClient(
            [target, old_reader],
            {
                1: _status("6b3bc01b1d41cfbd", 0),
                2: [_status(monitor.READER_ID, 5), None],
            },
        )
        worker = monitor.MonitorWorker(
            client, monitor.RecognitionGate(), lambda: None
        )
        worker.poll_once(0.0)
        worker.poll_once(0.1)
        self.assertIsNone(worker.reader)

        new_reader = monitor.AdbDevice(9, "duplicate", "1-2")
        client.devices = [target, new_reader]
        client.statuses[9] = _status(monitor.READER_ID, 5)
        client.queries.clear()
        worker.poll_once(0.2)

        self.assertEqual(client.queries, [9])
        self.assertEqual(worker.reader, new_reader)


if __name__ == "__main__":
    unittest.main()
