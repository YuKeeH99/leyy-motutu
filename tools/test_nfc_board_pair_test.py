import unittest
from unittest import mock

from tools import nfc_board_pair_test as pair


class NfcBoardPairTest(unittest.TestCase):
    def test_duplicate_serial_devices_keep_transport_ids(self) -> None:
        output = """List of devices attached
FFBBCCDDEE001122 device usb:1-2 product:mido transport_id:7
FFBBCCDDEE001122 device usb:1-3 product:mido transport_id:6
"""
        devices = pair.parse_adb_devices(output)
        self.assertEqual([item["transport_id"] for item in devices], [7, 6])

    def test_chip_id_uses_last_match_after_boot_log(self) -> None:
        output = "did: old\nChip ID: 6b3bc01b1642ccbd\nListenAI:/$"
        self.assertEqual(pair.extract_chip_id(output), "6b3bc01b1642ccbd")

    def test_scan_result(self) -> None:
        output = (
            "nfc target=1 uid=08123456 sens_res=0x0004 "
            "sel_res=0x20 ats=0675778102\nok"
        )
        self.assertEqual(
            pair.parse_scan_result(output),
            {
                "target": "1",
                "uid": "08123456",
                "sens_res": "0x0004",
                "sel_res": "0x20",
            },
        )

    def test_no_target_is_not_success(self) -> None:
        self.assertIsNone(
            pair.parse_scan_result("status=-7 (no NFC target)")
        )

    def test_stop_keeps_run_active_until_target_exits(self) -> None:
        controller = pair.PairController("adb")
        controller.running = True

        result = controller.stop()

        self.assertTrue(result["ok"])
        self.assertTrue(controller.stop_requested.is_set())
        self.assertTrue(controller.running)
        self.assertEqual(controller.phase, "stopping")

    def test_worker_rearms_target_until_stop(self) -> None:
        reader = pair.Device(7, "same", "1-2", pair.DEFAULT_READER_ID)
        target = pair.Device(6, "same", "1-3", pair.DEFAULT_TARGET_ID)

        class FakeTargetProcess:
            def __init__(self) -> None:
                self.exited = False

            def poll(self):
                return 0 if self.exited else None

            def communicate(self, timeout=None):
                self.exited = True
                return ("nfc target activated: mode=0x05\nok\n", None)

        class FakeController(pair.PairController):
            def __init__(self) -> None:
                super().__init__("adb")
                self.running = True
                self.spawned: list[FakeTargetProcess] = []

            def _peer_enabled(self, device):
                return False

            def _spawn_target(self, device):
                process = FakeTargetProcess()
                self.spawned.append(process)
                return process

            def _shell(self, device, command, timeout):
                if command == "v2hw nfc scan 5000":
                    self.spawned[-1].exited = True
                    if len(self.spawned) == 2:
                        self.stop_requested.set()
                    return (
                        "nfc target=1 uid=08123456 sens_res=0x0004 "
                        "sel_res=0x20 ats=0575339203\nok\n"
                    )
                if command == "v2hw nfc info":
                    return "status=0 ic=0x32 fw=1.6\nok\n"
                return "ok\n"

        controller = FakeController()
        with mock.patch.object(pair, "TARGET_ARM_DELAY_S", 0.0), mock.patch.object(
            pair, "TARGET_REARM_DELAY_S", 0.0
        ):
            controller._run_test(reader, target)

        self.assertEqual(len(controller.spawned), 2)
        self.assertEqual(controller.success_count, 2)
        self.assertEqual(controller.phase, "stopped")
        self.assertFalse(controller.running)


if __name__ == "__main__":
    unittest.main()
