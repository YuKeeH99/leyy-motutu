#!/usr/bin/env python3
"""Contracts for non-destructive provisioning and six-click Wi-Fi clearing."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_C = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
TONE_H = (ROOT / "apps/arcs-mini/tone/tone.h").read_text(encoding="utf-8")
TONE_DIR = ROOT / "res/arcs-mini/tone"


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b(?:static\s+)?[\w\s*]+\b{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing function: {name}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function: {name}")


class WifiMultiApContract(unittest.TestCase):
    def test_three_click_and_ble_provisioning_preserve_saved_wifi(self) -> None:
        triple = function_body(MAIN_C, "app_enter_web_netcfg_by_button")
        ble = function_body(MAIN_C, "app_ble_netcfg_enter_task")
        self.assertNotIn("app_clear_saved_wifi", triple)
        self.assertNotIn("sys_wifi_clear_saved_aps", triple)
        self.assertNotIn("app_clear_saved_wifi", ble)
        self.assertNotIn("sys_wifi_clear_saved_aps", ble)

    def test_five_click_standby_is_preserved_and_six_click_clears_wifi(self) -> None:
        button = function_body(MAIN_C, "button_changed")
        five = button.split("VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK", 1)[1].split(
            "service_v2_voice_standby_note_activity", 1
        )[0]
        self.assertIn("service_v2_voice_standby_request", five)

        self.assertIn("VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK", button)
        self.assertIn("app_clear_all_wifi_and_enter_netcfg_async", button)
        self.assertLess(
            button.find("VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK"),
            button.find("app_should_open_info_by_cloud_state"),
        )

        clear = function_body(MAIN_C, "app_clear_all_wifi_and_enter_netcfg_task")
        self.assertIn("app_clear_all_saved_wifi", clear)
        clear_storage = function_body(MAIN_C, "app_clear_all_saved_wifi")
        self.assertIn("sys_wifi_clear_saved_aps", clear_storage)
        self.assertIn("TONE_ID_113", clear)
        self.assertIn("app_netcfg_portal_start", clear)
        self.assertIn("app_ble_netcfg_adv_start_delayed", clear)

    def test_clear_prompt_is_packaged_as_local_tone(self) -> None:
        self.assertIn("TONE_ID_113", TONE_H)
        self.assertTrue((TONE_DIR / "113_wifi_aps_cleared.mp3").is_file())


if __name__ == "__main__":
    unittest.main()
