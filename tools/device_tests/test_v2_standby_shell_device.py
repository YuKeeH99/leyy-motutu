#!/usr/bin/env python3
"""On-device contract test for the V2 voice-standby ADB commands."""

from __future__ import annotations

import subprocess
import time
import unittest


def adb_shell(command: str, timeout: int = 15) -> str:
    completed = subprocess.run(
        ["adb", "shell", command],
        check=True,
        capture_output=True,
        timeout=timeout,
    )
    return (completed.stdout + completed.stderr).decode("utf-8", errors="replace")


class V2StandbyShellDeviceTest(unittest.TestCase):
    def test_enter_reports_asleep_with_all_servo_pwm_disabled(self) -> None:
        enter_output = adb_shell("v2hw standby enter", timeout=30)

        self.assertRegex(enter_output, r"standby request=(accepted|already-asleep)")
        status_output = ""
        for _ in range(10):
            status_output = adb_shell("v2hw standby status")
            if ("standby state=voice-standby" in status_output and
                    "servo_enabled=0x00" in status_output):
                break
            time.sleep(0.2)
        self.assertIn("standby state=voice-standby", status_output)
        self.assertIn("servo_enabled=0x00", status_output)


if __name__ == "__main__":
    unittest.main()
