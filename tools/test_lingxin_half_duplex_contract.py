#!/usr/bin/env python3
"""Contracts for the Cloud-VAD half-duplex protocol lifecycle."""

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


class LingxinHalfDuplexContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.protocol = (
            ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c"
        ).read_text(encoding="utf-8")
        cls.state = (
            ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/chat_state_machine.c"
        ).read_text(encoding="utf-8")

    def test_each_start_task_resets_round_one_shots_before_send(self) -> None:
        start = function_body(self.protocol, "sendStartTask")
        send = start.find("hook_websocket_send_text")
        for reset in (
            "isTaskStarted = false",
            "s_end_audio_sent = false",
            "s_end_task_sent = false",
            "s_cloud_vad_upload_allowed = false",
            "s_cloud_vad_close_requested = false",
        ):
            self.assertGreaterEqual(start.find(reset), 0)
            self.assertLess(start.find(reset), send)

    def test_server_events_are_filtered_by_active_task_id(self) -> None:
        deal = function_body(self.protocol, "dealEventFromServer")
        stale = deal.find("chat_manager_discard_stale_task_event")
        task_started = deal.find('strcmp(event, "task_started")')
        self.assertGreaterEqual(stale, 0)
        self.assertGreater(task_started, stale)

    def test_first_audio_packet_not_start_notice_enters_speaking(self) -> None:
        receive = function_body(self.state, "state_machine_receive_mp3_data")
        feed = receive.find("state_download_feed_data")
        speaking = receive.find("State_Download_Play")
        self.assertGreaterEqual(feed, 0)
        self.assertGreater(speaking, feed)

    def test_task_stage_end_is_the_only_normal_end_task_trigger(self) -> None:
        deal = function_body(self.protocol, "dealEventFromServer")
        stage = deal.find('strcmp(event, "task_stage_end")')
        send = deal.find("sendEndTask(handler)", stage)
        response_end = deal.find('strcmp(event, "audio_response_end")')
        self.assertGreaterEqual(stage, 0)
        self.assertGreater(send, stage)
        self.assertGreater(response_end, send)
        audio_ended = deal.find('strcmp(event, "audio_ended")', response_end)
        asr_ended = deal.find('strcmp(event, "asr_ended")', audio_ended)
        self.assertNotIn("sendEndTask", deal[response_end:audio_ended])
        self.assertIn("s_task_stage_end_pending", deal[audio_ended:asr_ended])
        self.assertIn("sendEndTask", deal[audio_ended:asr_ended])

    def test_terminate_wait_has_a_generation_scoped_timeout(self) -> None:
        terminate = function_body(self.protocol, "voiceChatTerminateCheck")
        watchdog = function_body(self.protocol, "protocol_watchdog_thread")
        self.assertIn("arm_terminate_task_watchdog", terminate)
        self.assertIn("s_terminate_watchdog_seq", watchdog)
        self.assertIn("s_waiting_task_terminated", watchdog)
        self.assertIn("on_voice_chat_init_fail", watchdog)

    def test_end_audio_wait_has_a_generation_scoped_first_tts_timeout(self) -> None:
        end_audio = function_body(self.protocol, "voiceChatStopSendAudio")
        watchdog = function_body(self.protocol, "protocol_watchdog_thread")
        binary = function_body(self.protocol, "onWebSocketEvent")
        self.assertIn("arm_first_tts_watchdog", end_audio)
        self.assertIn("s_first_tts_watchdog_seq", watchdog)
        self.assertIn("s_waiting_first_tts", watchdog)
        self.assertIn("on_voice_chat_init_fail", watchdog)
        self.assertIn("s_waiting_first_tts = false", binary)

    def test_deferred_failure_is_scoped_to_the_originating_dialog_round(self) -> None:
        start_new = function_body(self.protocol, "voice_chat_start_new")
        failure = function_body(self.protocol, "voice_chat_connection_failure_thread")
        schedule = function_body(self.protocol, "on_voice_chat_init_fail_for_epoch")

        self.assertIn("s_recovery_epoch", start_new)
        self.assertIn("uint32_t epoch", failure)
        self.assertIn("s_recovery_epoch", failure)
        self.assertIn("__atomic_compare_exchange_n", schedule)
        self.assertIn("module_voiceChat_exit", failure)
        self.assertNotIn("isWebsocketConnected = false", schedule)

    def test_start_task_watchdog_creation_failure_recovers_current_round(self) -> None:
        arm = function_body(self.protocol, "arm_start_task_watchdog")
        self.assertIn("if (!ensure_protocol_watchdog())", arm)
        self.assertIn("on_voice_chat_init_fail", arm)

    def test_termination_cancels_the_first_tts_timeout(self) -> None:
        deal = function_body(self.protocol, "dealEventFromServer")
        terminated_pos = deal.find('else if (strcmp(event, "task_terminated")')
        ended_pos = deal.find('else if (strcmp(event, "task_ended")', terminated_pos)
        terminated = deal[terminated_pos:ended_pos]

        self.assertIn("s_waiting_first_tts = false", terminated)
        self.assertIn("s_first_tts_deadline_ms", terminated)

    def test_all_protocol_deadlines_share_one_supervisor_task(self) -> None:
        supervisor = function_body(self.protocol, "protocol_watchdog_thread")
        self.assertIn("s_start_task_deadline_ms", supervisor)
        self.assertIn("s_terminate_deadline_ms", supervisor)
        self.assertIn("s_first_tts_deadline_ms", supervisor)

        for arm in (
            "arm_start_task_watchdog",
            "arm_terminate_task_watchdog",
            "arm_first_tts_watchdog",
        ):
            self.assertNotIn(
                "lingxin_thread_create",
                function_body(self.protocol, arm),
                f"{arm} must not allocate a sleeping task per round",
            )

    def test_local_wakeup_during_upload_starts_a_new_task(self) -> None:
        server = (ROOT / "src/server/lingxin_server/lingxin_server.c").read_text(encoding="utf-8")
        start = function_body(server, "lingxin_server_chat_start_internal")

        self.assertNotIn("wakeup_keep_current_task", start)
        self.assertNotIn("lingxin_should_keep_current_task_on_wakeup", start)
        self.assertIn("INT01_local_wakeup_interrupt", start)

    def test_cloud_idle_exit_default_is_thirty_seconds(self) -> None:
        config = (ROOT / "apps/arcs-mini/prj.conf").read_text(encoding="utf-8")
        cloud = (ROOT / "src/category/comm/msgs/voice_cloud_msg.c").read_text(encoding="utf-8")

        self.assertIn("CONFIG_CLOUD_IDLE_EXIT_TIMEOUT_MS=30000", config)
        self.assertIn("VOICE_IDLE_EXIT_TIMEOUT_MS_MIN", cloud)


if __name__ == "__main__":
    unittest.main()
