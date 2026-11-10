#!/usr/bin/env python3
"""Contracts for app-side recovery entry and soft-recovery escape."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RecoveryEntryContractTest(unittest.TestCase):
    def test_shell_recovery_uses_the_public_boot_api(self) -> None:
        source = (ROOT / "src/shell/cmd/sys.c").read_text(encoding="utf-8")
        self.assertIn('#include "uboot_recovery_api.h"', source)
        self.assertIn("uboot_recovery_request(UBOOT_RECOVERY_MODE_SOFT)", source)
        self.assertNotIn("struct boot_info", source)
        self.assertNotIn("REG_SW_RESET_CP0", source)

    def test_adb_reboot_recovery_uses_the_same_public_boot_api(self) -> None:
        for relative in (
            "arcs-sdk/components/cherryusb-appclass/adb/adb_reboot.c",
            "arcs-sdk/components/tinyusb-appclass/adb/adb_reboot.c",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn('#include "uboot_recovery_api.h"', source)
            self.assertIn("uboot_recovery_request(UBOOT_RECOVERY_MODE_SOFT)", source)
            self.assertNotIn("struct boot_info", source)
            self.assertNotIn("REG_SW_RESET_CP0", source)

    def test_product_enables_the_recovery_api(self) -> None:
        config = (ROOT / "apps/arcs-mini/prj.conf").read_text(encoding="utf-8")
        self.assertIn("CONFIG_BOOT_RECOVERY_API=y", config)

    def test_v2_1_boot_enables_adb_sync_for_image_downloads(self) -> None:
        config = (ROOT / "boards/arcs_mini_v2_1/boot.conf").read_text(
            encoding="utf-8"
        )
        self.assertIn("CONFIG_BOOT_ADB_SYNC=y", config)

    def test_both_adb_backends_report_a_real_protocol_connection(self) -> None:
        for relative in (
            "arcs-sdk/components/cherryusb-appclass/adb/adb.c",
            "arcs-sdk/components/tinyusb-appclass/adb/adb.c",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("boot_adb_runtime_notify_host_connected", source)
            self.assertRegex(
                source,
                r"case\s+A_CNXN\s*:[\s\S]*?boot_adb_runtime_notify_host_connected",
            )


if __name__ == "__main__":
    unittest.main()
