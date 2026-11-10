#!/usr/bin/env python3
"""Contracts for wake-triggered, single-attempt Wi-Fi recovery."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SYS_WIFI_C = (ROOT / "src/system/sys_wifi.c").read_text(encoding="utf-8")
SYS_WIFI_H = (ROOT / "src/system/sys_wifi.h").read_text(encoding="utf-8")
WAKEUP_C = (ROOT / "src/category/comm/msgs/voice_wakeup_msg.c").read_text(encoding="utf-8")
PLAYER_C = (ROOT / "apps/arcs-mini/voice_player.c").read_text(encoding="utf-8")
PLAYER_H = (ROOT / "src/category/comm/voice_player_comm.h").read_text(encoding="utf-8")
NETWORK_MANAGER_C = (ROOT / "src/system/sys_network_manager.c").read_text(encoding="utf-8")
LINGXIN_WS_C = (ROOT / "src/server/lingxin_server/adapter/lingxin_websocket_adapter.c").read_text(encoding="utf-8")


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


class WifiWakeupReconnectContract(unittest.TestCase):
    def test_connected_callback_mirrors_sdk_autoconnect_stop(self) -> None:
        callback = function_body(SYS_WIFI_C, "wifi_mgr_connection_cb")
        connected = callback.split("case WIFI_MGR_STA_CONNECTED:", 1)[1].split("case WIFI_MGR_STA_DISCONNECTED:", 1)[0]
        self.assertIn("wifi_mgr_auto_connect_stop()", connected)
        self.assertLess(
            connected.find("wifi_mgr_auto_connect_stop()"),
            connected.find("s_wifi.autoconnect_enabled = false"),
        )
        self.assertIn("s_wifi.autoconnect_enabled = false", connected)

    def test_single_attempt_api_restarts_saved_ap_autoconnect(self) -> None:
        self.assertIn("int sys_wifi_reconnect_saved_once_start(void);", SYS_WIFI_H)
        self.assertIn("void sys_wifi_reconnect_saved_once_stop(void);", SYS_WIFI_H)

        start = function_body(SYS_WIFI_C, "sys_wifi_reconnect_saved_once_start")
        self.assertIn("sys_wifi_reconnect_decide", start)
        self.assertIn("s_wifi.ready", start)
        self.assertIn("s_wifi.started", start)
        self.assertIn("s_wifi.connected", start)
        self.assertIn("sys_wifi_has_ap()", start)
        self.assertLess(start.find("sys_wifi_stop_autoconnect()"), start.find("sys_wifi_start_autoconnect()"))
        self.assertIn("SYS_WIFI_RECONNECT_FORCE_REASSOCIATE", start)
        self.assertLess(start.find("wifi_sta_disconnect()"), start.find("wifi_mgr_sta_connect(&stale_ap, true)"))

        stop = function_body(SYS_WIFI_C, "sys_wifi_reconnect_saved_once_stop")
        self.assertIn("sys_wifi_stop_autoconnect()", stop)

    def test_failed_websocket_requests_verification_without_fabricating_offline(self) -> None:
        websocket_event = function_body(LINGXIN_WS_C, "lingxin_ws_on_event")
        self.assertIn("sys_network_report_transport_failure()", websocket_event)

        report = function_body(NETWORK_MANAGER_C, "sys_network_report_transport_failure")
        self.assertIn("network_probe_refresh()", report)
        self.assertNotIn("s_mgr.network_connected = false", report)
        self.assertNotIn("VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL", report)

    def test_offline_wakeup_attempt_is_bounded_and_does_not_start_chat(self) -> None:
        thread = function_body(WAKEUP_C, "lingxin_start_thread")
        self.assertIn("sys_wifi_reconnect_saved_once_start()", thread)
        self.assertIn("LINGXIN_WIFI_RECONNECT_TIMEOUT_MS", thread)
        self.assertIn("sys_wifi_reconnect_saved_once_stop()", thread)
        self.assertIn("lingxin_wakeup_request_snapshot()", thread)
        self.assertIn("wifi_reconnect_retriggered", thread)
        self.assertIn("voice_player_play_network_recovery_tone()", thread)
        recovery = thread.find("voice_player_play_network_recovery_tone()")
        chat = thread.find("start_lingxin_chat(app_datas, owner_generation)")
        self.assertGreater(recovery, 0)
        self.assertTrue(chat < 0 or "network_was_ready_at_entry" in thread[:chat])

    def test_offline_wakeup_hands_active_portal_to_sta_before_reconnect(self) -> None:
        thread = function_body(WAKEUP_C, "lingxin_start_thread")
        portal_check = thread.find("app_netcfg_portal_is_active()")
        portal_stop = thread.find("app_netcfg_portal_stop()")
        reconnect = thread.find("sys_wifi_reconnect_saved_once_start()")
        portal_restore = thread.find("app_netcfg_portal_start(")

        self.assertGreaterEqual(portal_check, 0)
        self.assertGreater(portal_stop, portal_check)
        self.assertGreater(reconnect, portal_stop)
        self.assertGreater(portal_restore, reconnect)

    def test_busy_offline_worker_records_each_wakeup_instead_of_dropping_it(self) -> None:
        schedule = function_body(WAKEUP_C, "start_lingxin_chat_async_for_owner")
        self.assertIn("lingxin_wakeup_request_register", schedule)
        self.assertIn("retrigger queued", schedule)
        self.assertNotIn("start ignored: already running", schedule)

        register = function_body(WAKEUP_C, "lingxin_wakeup_request_register")
        self.assertIn("taskENTER_CRITICAL()", register)
        self.assertIn("s_lingxin_wakeup_request_seq++", register)
        self.assertIn("s_lingxin_wifi_recovering", register)
        self.assertIn("wifi_recovery_needed", register)

    def test_physical_wifi_link_is_required_before_cloud_wakeup_tone(self) -> None:
        wake_case = PLAYER_C.split("case VOICE_MSG_WAKEUP_KEYWORD:", 1)[1].split(
            "case VOICE_MSG_CLOUD_CLOUD_AUTH_SUCCESS:", 1
        )[0]
        unavailable = wake_case.find("voice_player_network_is_unavailable")
        cloud_ready = wake_case.find("voice_player_cloud_is_ready")
        self.assertGreaterEqual(unavailable, 0)
        self.assertGreater(cloud_ready, unavailable)

        helper = function_body(PLAYER_C, "voice_player_network_is_unavailable")
        self.assertIn("voice_player_has_network_link", helper)
        self.assertIn("!network_status->connected", helper)

    def test_network_unavailable_tone_is_deduplicated_and_prioritized(self) -> None:
        helper = function_body(PLAYER_C, "voice_player_play_network_tone")
        self.assertIn("tone_id", helper)
        self.assertIn("voice_player_tone_url_is_active_or_queued", helper)
        self.assertIn("voice_player_queue_priority_tone", helper)
        self.assertIn("network_unavailable_tone_already_pending", helper)

        priority = function_body(PLAYER_C, "voice_player_queue_priority_tone")
        self.assertIn("dropped_wakeup_generation", priority)
        self.assertIn("voice_player_schedule_wakeup_gate_release", priority)

    def test_wakeup_uses_existing_failure_and_success_tones(self) -> None:
        wake_case = PLAYER_C.split("case VOICE_MSG_WAKEUP_KEYWORD:", 1)[1].split(
            "case VOICE_MSG_CLOUD_CLOUD_AUTH_SUCCESS:", 1
        )[0]
        self.assertIn("voice_player_play_network_unavailable_tone", wake_case)
        unavailable = function_body(PLAYER_C, "voice_player_play_network_unavailable_tone")
        self.assertIn("TONE_ID_64", unavailable)
        self.assertIn("void voice_player_play_network_recovery_tone(void);", PLAYER_H)
        recovery = function_body(PLAYER_C, "voice_player_play_network_recovery_tone")
        self.assertIn("s_disconnect_tone_played = false", recovery)
        self.assertIn("voice_player_play_network_success_tone()", recovery)


if __name__ == "__main__":
    unittest.main()
