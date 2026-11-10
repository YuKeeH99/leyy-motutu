#!/usr/bin/env python3
"""Contracts for debounced, final-count-only power-button gestures."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUTTON_H = (ROOT / "src/middleware/button/lisa_btn.h").read_text(encoding="utf-8")
BUTTON_C = (ROOT / "src/middleware/button/lisa_btn.c").read_text(encoding="utf-8")
SERVICE_C = (ROOT / "apps/arcs-mini/services/service_button.c").read_text(encoding="utf-8")
WAKEUP_C = (ROOT / "src/category/comm/msgs/voice_wakeup_msg.c").read_text(encoding="utf-8")


class ButtonClickContract(unittest.TestCase):
    def test_power_button_uses_a_500ms_final_count_window(self) -> None:
        self.assertIn("multiple_click_interval_ms", BUTTON_H)
        self.assertRegex(SERVICE_C, r"\.multiple_click_interval_ms\s*=\s*500U")
        self.assertRegex(
            BUTTON_C,
            r"max_multiple_clicks_interval\s*=\s*FLEX_MS_TO_SCAN_CNT\("
            r"\s*config->time_config\.multiple_click_interval_ms\)",
        )

    def test_gpio_level_is_debounced_before_flexible_button_consumes_it(self) -> None:
        self.assertIn("GPIO_BUTTON_DEBOUNCE_MS", BUTTON_C)
        self.assertIn("stable_level", BUTTON_C)
        self.assertIn("candidate_level", BUTTON_C)
        self.assertIn("candidate_since", BUTTON_C)

    def test_lingxin_does_not_consume_intermediate_release_events(self) -> None:
        match = re.search(
            r"static void voice_wakeup_button_event\([^)]*\)\s*\{([\s\S]*?)\n\}",
            WAKEUP_C,
        )
        self.assertIsNotNone(match)
        body = match.group(1)
        lingxin_guard = body.split("#ifdef CONFIG_VOICE_LINK_LINGXIN", 1)[1].split("#endif", 1)[0]
        self.assertIn("return;", lingxin_guard)


if __name__ == "__main__":
    unittest.main()
