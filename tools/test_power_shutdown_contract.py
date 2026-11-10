#!/usr/bin/env python3
"""Contracts for non-blocking and emergency power-button shutdown."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


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


class PowerShutdownContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
        cls.button = (
            ROOT / "apps/arcs-mini/services/service_button.c"
        ).read_text(encoding="utf-8")
        cls.power = (
            ROOT / "src/middleware/power/power_manager.c"
        ).read_text(encoding="utf-8")

    def test_main_button_handler_only_queues_shutdown(self) -> None:
        handler = function_body(self.main, "button_changed")
        long_hold = handler.find("VOICE_MSG_BUTTON_ACTION_LONG_HOLD")
        branch = handler[long_hold:]
        self.assertIn("power_request_shutdown()", branch)
        self.assertNotIn("voice_player_play_shutdown_prompt_sync()", branch)
        self.assertNotRegex(branch, r"(?<!request_)power_shutdown\s*\(")

    def test_shutdown_claims_audio_before_offline_cancellation(self):
        body = function_body(self.main, "app_prepare_shutdown_audio")
        self.assertLess(body.index("voice_player_begin_shutdown_prompt()"),
                        body.index("service_offline_action_shutdown()"))
        source = (ROOT / "apps/arcs-mini/voice_player.c").read_text(encoding="utf-8")
        handler = function_body(source, "voice_player_play_msg")
        self.assertLess(handler.index("if (s_shutdown_prompt_in_progress)"),
                        handler.index("++s_audio_message_inflight"))
        self.assertIn("--s_audio_message_inflight", handler)
        shutdown = function_body(source, "voice_player_play_shutdown_prompt_sync")
        self.assertLess(shutdown.index("s_audio_message_inflight"), shutdown.index("app_player_stop(tone_player)"))
        self.assertLess(shutdown.index("app_player_callbacks_idle"), shutdown.index("app_player_stop(tone_player)"))
        stop = shutdown.index("app_player_stop(tone_player)")
        self.assertLess(shutdown.index("app_player_callbacks_idle", stop), shutdown.index("s_shutdown_tone_playing = true"))

    def test_shutdown_worker_owns_bounded_prompt_then_power_release(self) -> None:
        worker = function_body(self.power, "power_shutdown_worker")
        prepare = worker.find("shutdown_prepare_cb")
        shutdown = worker.find("power_shutdown()")
        self.assertGreaterEqual(prepare, 0)
        self.assertGreater(shutdown, prepare)
        self.assertIn("power_set_shutdown_prepare_callback", self.power)

    def test_eight_second_raw_key_monitor_bypasses_voice_message_bus(self) -> None:
        self.assertRegex(
            self.button,
            r"MINI_POWER_KEY_FORCE_SHUTDOWN_MS\s+8000U",
        )
        monitor = function_body(self.button, "power_key_force_shutdown_task")
        self.assertIn("lisa_gpio_read_pin", monitor)
        self.assertIn("power_request_shutdown()", monitor)
        self.assertNotIn("voice_msg_pub", monitor)


if __name__ == "__main__":
    unittest.main()
