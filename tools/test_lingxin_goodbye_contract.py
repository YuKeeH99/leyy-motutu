#!/usr/bin/env python3
"""Contracts for SAY_GOODBYE exiting only after its TTS playback."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER_C = (ROOT / "src/server/lingxin_server/lingxin_server.c").read_text(encoding="utf-8")
SERVER_H = (ROOT / "src/server/lingxin_server/lingxin_server.h").read_text(encoding="utf-8")
AUDIO_ADAPTER_C = (
    ROOT / "src/server/lingxin_server/adapter/lingxin_audio_adapter.c"
).read_text(encoding="utf-8")


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


class LingxinGoodbyeContract(unittest.TestCase):
    def test_goodbye_is_consumed_as_session_control_not_motion(self) -> None:
        body = function_body(SERVER_C, "lingxin_handle_text_output")
        self.assertIn('"SAY_GOODBYE"', body)
        self.assertIn("lingxin_goodbye_arm", body)
        self.assertLess(body.find("lingxin_goodbye_arm"), body.find("VOICE_MSG_CLOUD_ACTION_LIST"))

    def test_goodbye_exits_on_matching_playback_end(self) -> None:
        lifecycle = function_body(SERVER_C, "lingxin_chat_life_cycle_listener")
        self.assertIn("CHAT_LIFE_CYCLE_EVENT_PLAY_END", lifecycle)
        self.assertIn("lingxin_goodbye_complete_after_playback", lifecycle)
        completion = function_body(SERVER_C, "lingxin_goodbye_complete_after_playback")
        self.assertIn("s_lingxin_goodbye_generation", completion)
        self.assertIn("s_lingxin_chat_generation", completion)
        self.assertIn("lingxin_goodbye_dispatch_exit", completion)
        self.assertIn(
            "VOICE_MSG_CLOUD_MCP_CHAT_EXIT",
            function_body(SERVER_C, "lingxin_goodbye_dispatch_exit"),
        )

    def test_real_cloud_player_completion_reaches_goodbye_gate(self) -> None:
        """Catch SDK lifecycle PLAY_END being absent while app_player completes."""
        playback_thread = function_body(AUDIO_ADAPTER_C, "lingxin_playback_event_thread")
        self.assertIn("lingxin_server_tts_playback_complete", SERVER_H)
        self.assertIn("Lingxin_Download_Audio_PlayEnd", playback_thread)
        self.assertIn("lingxin_server_tts_playback_complete", playback_thread)

    def test_goodbye_has_timeout_fallback_and_is_reset_at_boundaries(self) -> None:
        self.assertIn("LINGXIN_GOODBYE_EXIT_TIMEOUT_MS", SERVER_C)
        self.assertIn("lingxin_goodbye_timeout_cb", SERVER_C)
        start = function_body(SERVER_C, "lingxin_server_chat_start_internal")
        finished = function_body(SERVER_C, "lingxin_publish_chat_finished")
        self.assertIn("lingxin_goodbye_reset", start)
        self.assertIn("lingxin_goodbye_reset", finished)


if __name__ == "__main__":
    unittest.main()
