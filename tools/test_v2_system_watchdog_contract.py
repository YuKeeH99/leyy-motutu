#!/usr/bin/env python3
"""Source contract for the V2.1 runtime hardware watchdog."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
MAIN_PATH = REPO_ROOT / "apps/arcs-mini/main.c"
CONFIG_PATH = REPO_ROOT / "apps/arcs-mini/prj.conf"


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\bstatic\s+(?:int|void)\s+{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing function: {name}")

    opening_brace = source.find("{", match.start())
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace + 1 : index]
    raise AssertionError(f"unterminated function: {name}")


class V2SystemWatchdogContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = MAIN_PATH.read_text(encoding="utf-8")
        cls.config = CONFIG_PATH.read_text(encoding="utf-8")

    def test_lisa_watchdog_is_enabled_and_legacy_recovery_watchdog_is_disabled(self) -> None:
        self.assertRegex(self.config, r"(?m)^CONFIG_BOOT_WITH_WATCHDOG=y$")
        self.assertRegex(self.config, r"(?m)^CONFIG_WATCHDOG_ENABLE=n$")
        self.assertRegex(self.config, r"(?m)^CONFIG_LISA_WDT=y$")

    def test_runtime_timeout_contract_is_10_seconds_plus_reset_stage(self) -> None:
        self.assertRegex(
            self.main,
            r"#define\s+V2_SYSTEM_WATCHDOG_INT_TIMEOUT_MS\s+10000U",
        )
        self.assertRegex(
            self.main,
            r"#define\s+V2_SYSTEM_WATCHDOG_RST_TIMEOUT_MS\s+500U",
        )

    def test_start_path_uses_wdt0_setup_and_hardware_start(self) -> None:
        body = function_body(self.main, "app_v2_system_watchdog_start")
        self.assertIn("lisa_device_get(V2_SYSTEM_WATCHDOG_DEVICE_NAME)", body)
        self.assertIn("lisa_device_ready(s_v2_system_watchdog)", body)
        self.assertIn("lisa_wdt_setup(s_v2_system_watchdog, &config)", body)
        self.assertIn("lisa_wdt_start(s_v2_system_watchdog)", body)
        self.assertIn("lisa_wdt_feed(s_v2_system_watchdog)", body)

    def test_existing_main_loop_is_the_feed_owner(self) -> None:
        body = function_body(self.main, "app_v2_system_watchdog_feed")
        self.assertIn("lisa_wdt_feed(s_v2_system_watchdog)", body)
        self.assertNotIn("lisa_thread_create", body)
        self.assertNotIn("xTaskCreate", body)
        self.assertRegex(
            self.main,
            r"while\s*\(\s*1\s*\)\s*\{\s*"
            r"#if[^\n]*CONFIG_LISA_WDT[^\n]*\n\s*"
            r"app_v2_system_watchdog_feed\(\);",
        )


if __name__ == "__main__":
    unittest.main()
