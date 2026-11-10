#!/usr/bin/env python3
"""Source-level regressions for V2.1 button, wake-audio gate and RGB output."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b(?:static\s+)?[\w\s*]+\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
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


class ButtonWakeupRgbContractTest(unittest.TestCase):
    def test_v21_double_click_preempts_cloud_redirect_and_enters_assembly_pose(self) -> None:
        source = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
        handler = function_body(source, "button_changed")
        double_click = handler.find("evt->action == VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK")
        redirect = handler.find("app_should_open_info_by_cloud_state")

        self.assertGreaterEqual(double_click, 0, "V2.1 double click must have an early dedicated handler")
        self.assertGreater(redirect, double_click, "cloud status redirect must not consume servo reset")
        early_handler = handler[double_click:redirect]
        self.assertIn("service_v2_voice_standby_wake_pose(true)", early_handler)
        standby = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby.c").read_text(encoding="utf-8")
        self.assertIn("s_requested_pose = assembly ? 2U : 1U", function_body(standby, "service_v2_voice_standby_wake_pose"))
        self.assertIn("pose == 2U ? SERVICE_V2_GAIT_ACTION_ASSEMBLY : SERVICE_V2_GAIT_ACTION_STAND", standby)
        self.assertNotIn("service_v2_gait_submit(SERVICE_V2_GAIT_ACTION_STAND, 0U)", early_handler)
        self.assertRegex(early_handler, r"\breturn\s*;")

    def test_wakeup_gate_drops_inflight_frames_and_clears_capture_state(self) -> None:
        source = (
            ROOT / "src/server/lingxin_server/adapter/lingxin_audio_adapter.c"
        ).read_text(encoding="utf-8")
        sender = function_body(source, "lingxin_record_sender_thread")
        gate_check = sender.find("!lingxin_audio_uplink_gate_is_open(NULL)")
        gate = function_body(source, "lingxin_audio_uplink_gate_is_open")
        self.assertIn("s_record_uplink_gate_open", gate)
        self.assertIn("service_offline_action_online_current", gate)
        send = sender.find("lingxin_process_record_data")
        self.assertGreaterEqual(gate_check, 0, "sender must enforce the gate after popping a queued frame")
        self.assertGreater(send, gate_check, "sender gate check must happen before cloud upload")

        close_gate = function_body(source, "lingxin_audio_uplink_gate_close")
        open_gate = function_body(source, "lingxin_audio_uplink_gate_open")
        discard = "lingxin_record_discard_buffered(s_active_recorder)"
        self.assertIn(discard, close_gate)
        self.assertIn(discard, open_gate)
        self.assertLess(open_gate.find(discard), open_gate.find("s_record_uplink_gate_open = true"))

    def test_wakeup_tone_drains_audio_hardware_before_tail_guard(self) -> None:
        source = (ROOT / "apps/arcs-mini/voice_player.c").read_text(encoding="utf-8")
        drain = function_body(source, "voice_player_drain_wakeup_output")
        tone_event = function_body(source, "on_tone_event")
        self.assertIn("lisa_audio_play_flush", drain)
        drain_call = tone_event.find("voice_player_drain_wakeup_output()")
        release = tone_event.find("voice_player_schedule_wakeup_gate_release(completed_wakeup_generation")
        next_tone = tone_event.find("voice_player_start_next_queued_tone()")
        self.assertGreaterEqual(drain_call, 0)
        self.assertGreater(release, drain_call)
        self.assertGreater(next_tone, release, "do not accidentally drain a subsequent queued tone")

    def test_rgb_effect_reports_the_first_pa08_result_and_write_errors(self) -> None:
        source = (
            ROOT / "apps/arcs-mini/services/service_v2_rgb_effect.c"
        ).read_text(encoding="utf-8")
        apply = function_body(source, "rgb_effect_apply")
        self.assertRegex(apply, r"\bret\s*=\s*service_v2_actuators_rgb_set")
        self.assertIn("PA08", apply)
        self.assertIn("LISA_LOGE", apply)

    def test_boot_rgb_is_driven_by_actual_tone_playback(self) -> None:
        source = (ROOT / "apps/arcs-mini/voice_player.c").read_text(encoding="utf-8")
        event = function_body(source, "voice_player_boot_rgb_event")
        self.assertIn("APP_PLAYER_EVENT_PLAYING", event)
        self.assertIn("service_v2_rgb_effect_show_start(SERVICE_V2_RGB_SHOW_BOOT)", event)
        for terminal in ("APP_PLAYER_EVENT_COMPLETED", "APP_PLAYER_EVENT_STOPPED", "APP_PLAYER_EVENT_ERROR"):
            self.assertIn(terminal, event)
        self.assertIn("service_v2_rgb_effect_show_stop(SERVICE_V2_RGB_SHOW_BOOT)", event)

    def test_touch_worker_monitors_both_pads_and_explains_usb_gate(self) -> None:
        source = (
            ROOT / "apps/arcs-mini/services/service_v2_touch_action.c"
        ).read_text(encoding="utf-8")
        task = function_body(source, "touch_action_task")
        transition = function_body(source, "touch_action_handle_transition")

        self.assertIn("SERVICE_V2_TOUCH_CHANNEL_PA09", task)
        self.assertIn("SERVICE_V2_TOUCH_CHANNEL_PA13", task)
        self.assertIn("touch_action_accept_sample(&s_pa09", task)
        self.assertIn("touch_action_accept_sample(&s_pa13", task)
        self.assertIn("power_is_usb_plugged()", transition)
        self.assertIn("Type-C", transition)

    def test_pa09_and_pa13_have_separate_product_actions(self) -> None:
        source = (
            ROOT / "apps/arcs-mini/services/service_v2_touch_action.c"
        ).read_text(encoding="utf-8")
        transition = function_body(source, "touch_action_handle_transition")
        self.assertRegex(source, r's_pa09\s*=\s*\{[\s\S]*?LEFT_FRONT_HANDSHAKE')
        self.assertRegex(source, r's_pa13\s*=\s*\{[\s\S]*?RIGHT_FRONT_KICK')
        self.assertIn("service_v2_gait_submit_if_idle(state->action, 0U)", transition)


if __name__ == "__main__":
    unittest.main()
