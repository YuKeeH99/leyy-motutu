#!/usr/bin/env python3
"""Contracts for the state-aware single-click dialog control."""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path

from tools import romfs


ROOT = Path(__file__).resolve().parents[1]
MAIN_C = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
SERVER_H = (ROOT / "src/server/lingxin_server/lingxin_server.h").read_text(encoding="utf-8")
SERVER_C = (ROOT / "src/server/lingxin_server/lingxin_server.c").read_text(encoding="utf-8")
BUTTON_C = (ROOT / "apps/arcs-mini/services/service_button.c").read_text(encoding="utf-8")
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


class ButtonDialogStateContract(unittest.TestCase):
    def test_server_exposes_stable_dialog_phases(self) -> None:
        for phase in (
            "LINGXIN_DIALOG_PHASE_IDLE",
            "LINGXIN_DIALOG_PHASE_LISTENING",
            "LINGXIN_DIALOG_PHASE_SPEAKING",
            "LINGXIN_DIALOG_PHASE_TRANSITION",
            "LINGXIN_DIALOG_PHASE_UNAVAILABLE",
        ):
            self.assertIn(phase, SERVER_H)
        self.assertIn("lingxin_server_dialog_phase(void);", SERVER_H)

    def test_sdk_states_are_mapped_without_leaking_raw_numbers_to_main(self) -> None:
        body = function_body(SERVER_C, "lingxin_server_dialog_phase")
        self.assertRegex(body, r"State_Idle[\s\S]*LINGXIN_DIALOG_PHASE_IDLE")
        self.assertRegex(
            body,
            r"State_Upload_Init[\s\S]*State_Upload_Transfer[\s\S]*LINGXIN_DIALOG_PHASE_LISTENING",
        )
        self.assertRegex(
            body,
            r"State_Welcome[\s\S]*State_Download_Init[\s\S]*State_Download_Play"
            r"[\s\S]*State_Download_End[\s\S]*LINGXIN_DIALOG_PHASE_SPEAKING",
        )
        self.assertIn("LINGXIN_DIALOG_PHASE_TRANSITION", body)

    def test_single_click_has_distinct_idle_speaking_and_listening_actions(self) -> None:
        body = function_body(MAIN_C, "button_changed")
        click = body.split("case VOICE_MSG_BUTTON_ACTION_CLICK:", 1)[1].split(
            "case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:", 1
        )[0]
        execute = function_body(MAIN_C, "app_lingxin_single_click_execute")

        snapshot = function_body(BUTTON_C, "service_button_snapshot_event")
        self.assertIn("VOICE_MSG_BUTTON_ACTION_PRESS_DOWN", snapshot)
        self.assertIn("lingxin_server_dialog_phase()", snapshot)
        self.assertIn("LINGXIN_DIALOG_PHASE_LISTENING", snapshot)
        self.assertNotIn("lingxin_server_dialog_phase()", click)
        self.assertIn("SERVICE_BUTTON_DIALOG_WAKE", execute)
        self.assertIn("SERVICE_BUTTON_DIALOG_EXIT", execute)
        self.assertNotIn("lingxin_server_suppress_current_tts", execute)
        self.assertIn("service_button_wakeup_request(event)", execute)
        self.assertIn("VOICE_MSG_WAKEUP_BUTTON_START", function_body(BUTTON_C, "service_button_wakeup_request"))
        self.assertIn("lingxin_server_chat_end_record()", execute)
        self.assertIn("lingxin_server_chat_stop()", execute)
        self.assertIn("app_play_dialog_exit_prompt", execute)
        self.assertIn("TONE_ID_114", function_body(MAIN_C, "app_play_dialog_exit_prompt"))

    def test_transition_click_uses_existing_asynchronous_wake_without_reinterpreting_intent(self) -> None:
        body = function_body(MAIN_C, "button_changed")
        click = body.split("case VOICE_MSG_BUTTON_ACTION_CLICK:", 1)[1].split(
            "case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:", 1
        )[0]
        self.assertIn("app_lingxin_single_click_execute(data)", click)
        self.assertNotIn("app_lingxin_single_click_defer", MAIN_C)
        wake = (ROOT / "src/category/comm/msgs/voice_wakeup_msg.c").read_text(encoding="utf-8")
        self.assertIn('start_lingxin_chat_async_for_owner("button", owner_generation)', wake)
        self.assertNotIn("Single click ignored during dialog transition", click)

    def test_local_exit_prompt_is_packaged(self) -> None:
        self.assertIn("TONE_ID_114 = 114", TONE_H)
        asset = TONE_DIR / "114_exit_dialog.mp3"
        self.assertTrue(asset.is_file())
        with tempfile.TemporaryDirectory() as temp:
            romfs.extract_image(str(ROOT / "res/arcs-mini/tone.bin"), temp)
            self.assertEqual((Path(temp) / asset.name).read_bytes(), asset.read_bytes())


if __name__ == "__main__":
    unittest.main()
