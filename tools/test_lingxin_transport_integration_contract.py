#!/usr/bin/env python3
"""Production integration contracts for Lingxin websocket transport guards."""

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


class LingxinTransportIntegrationContractTest(unittest.TestCase):
    def test_websocket_worker_uses_pending_policy_and_propagates_stalls(self) -> None:
        source = (
            ROOT / "arcs-sdk/components/lisa_websocket/lisa_websocket.c"
        ).read_text(encoding="utf-8")
        normal_send = function_body(source, "lisa_websocket_msg_normal_send")
        send_proc = function_body(source, "lisa_websocket_msg_send_proc")
        worker = function_body(source, "lisa_websocket_nopoll_send_recv_proc")

        self.assertIn('#include "lisa_websocket_pending_policy.h"', source)
        self.assertIn("lisa_ws_classify_send_result", normal_send)
        self.assertNotRegex(
            normal_send,
            r"do\s*\{[\s\S]*?\}\s*while\s*\(\s*err\s*==\s*-2\s*\)",
            "EAGAIN means noPoll owns the frame and must not resubmit it",
        )
        self.assertIn("lisa_websocket_pending_write_proc", send_proc)
        self.assertRegex(worker, r"err\s*=\s*lisa_websocket_msg_send_proc")
        self.assertRegex(worker, r"if\s*\(\s*err\s*!=\s*0\s*\)")
        self.assertRegex(
            worker,
            r"nopoll_conn_pending_write_bytes\([^)]*\)\s*==\s*0[\s\S]*?nopoll_conn_send_ping",
        )

    def test_close_retains_handler_until_worker_ack_and_drains_api_operations(self) -> None:
        source = (
            ROOT / "src/server/lingxin_server/adapter/lingxin_websocket_adapter.c"
        ).read_text(encoding="utf-8")
        close = function_body(source, "closeWebsocket")
        acquire = function_body(source, "lingxin_ws_acquire")
        release = function_body(source, "lingxin_ws_release")
        destroy = function_body(source, "destroyWebsocketClient")

        self.assertIn("active_operations", source)
        self.assertIn("__atomic_add_fetch", acquire)
        self.assertIn("__atomic_sub_fetch", release)
        closing = close.find("__atomic_store_n(&client->closing, true")
        drain = close.find("active_operations")
        cleanup = close.find("lisa_ws_cleanup")
        detach = close.find("__atomic_store_n(&client->clientHandler, NULL")
        self.assertGreaterEqual(closing, 0)
        self.assertGreater(drain, closing)
        self.assertGreater(cleanup, drain)
        self.assertGreater(detach, cleanup)
        self.assertRegex(close[cleanup:detach], r"!=\s*LISA_WS_OK[\s\S]*?return false;")
        self.assertGreater(close.find("lisa_mem_free(ctx)"), detach)
        self.assertIn("LINGXIN_WS_CLOSE_DRAIN_MS", close)
        self.assertIn("close_active", close)
        self.assertNotIn("lisa_mem_free(client)", close)
        self.assertIn("lisa_mem_free(client)", destroy)
        self.assertIn("clientHandler", destroy)
        self.assertIn("close_active", destroy)

        sdk = (ROOT / "arcs-sdk/components/lisa_websocket/lisa_websocket.c").read_text(encoding="utf-8")
        sdk_cleanup = function_body(sdk, "lisa_ws_cleanup")
        self.assertLess(sdk_cleanup.find("return result"), sdk_cleanup.find("lisa_thread_delete"))
        sdk_worker = function_body(sdk, "lisa_websocket_thread")
        ack = sdk_worker.rfind("lisa_semaphore_give(ins->exited_sem)")
        self.assertGreater(ack, sdk_worker.find("lisa_websocket_nopoll_send_recv_proc"))
        self.assertGreater(sdk_worker.find("__atomic_store_n(&ins->worker_busy, false"), ack)

        event = function_body(source, "lingxin_ws_on_event")
        self.assertLess(event.find("ctx->closing"), event.find("switch (event->what)"))
        disconnected = event[event.find("case LISA_WS_ON_DISCONNECTED") :]
        callback = disconnected.find("listener(ON_WEBSOCKET_DESTROY")
        self.assertGreaterEqual(callback, 0)
        self.assertLess(disconnected.find("ctx->config->listener = NULL"), callback)
        self.assertLess(disconnected.find("ctx->config->userContext = NULL"), callback)

        protocol = (
            ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c"
        ).read_text(encoding="utf-8")
        self.assertIn("destroyWebsocketClient", function_body(protocol, "freeVoiceChat"))

    def test_protocol_owns_disconnect_cleanup_outside_websocket_callback(self) -> None:
        protocol = (
            ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c"
        ).read_text(encoding="utf-8")
        callback = function_body(protocol, "onWebSocketEvent")
        exit_body = function_body(protocol, "module_voiceChat_exit")
        completion = function_body(protocol, "voice_chat_close_completed")

        destroy_case = callback[callback.find("case ON_WEBSOCKET_DESTROY") :]
        self.assertNotIn("freeVoiceChat", destroy_case)
        self.assertIn("on_voice_chat_init_fail_for_epoch", destroy_case)
        self.assertIn("hook_websocket_close_async", exit_body)
        self.assertNotIn("freeVoiceChat(", exit_body)
        self.assertNotIn("state_machine_run_event(", exit_body)
        self.assertLess(completion.find("freeVoiceChat("), completion.find("State_Event_VoiceChat_ExitEnd"))

        hook = (ROOT / "third_party/AI2T_LingXinEngine/src/websocket/lingxin_hook_websocket.c").read_text(encoding="utf-8")
        worker = function_body(hook, "websocket_close_worker")
        self.assertRegex(worker, r"while\s*\(\s*!closeWebsocket\(job->client\)\s*\)")
        self.assertLess(worker.find("closeWebsocket"), worker.find("job->completed(job->user)"))
        self.assertNotIn("job->client", worker[worker.find("job->completed(job->user)"):])

    def test_audio_sender_holds_handler_lease_until_send_and_logging_finish(self) -> None:
        protocol = (
            ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c"
        ).read_text(encoding="utf-8")
        send = function_body(protocol, "voiceChatSendAudio")
        exit_body = function_body(protocol, "module_voiceChat_exit")
        completion = function_body(protocol, "voice_chat_close_completed")

        self.assertIn("voice_chat_handler_acquire", send)
        self.assertIn("voice_chat_handler_release", send)
        self.assertNotIn("globalHandler->", send)
        self.assertIn("voice_chat_handler_begin_close", exit_body)
        self.assertIn("active_operations", completion)
        self.assertLess(completion.find("active_operations"), completion.find("freeVoiceChat("))
        begin_close = function_body(protocol, "voice_chat_handler_begin_close")
        self.assertIn("close_requested", begin_close)
        self.assertIn("__atomic_store_n(&handler->closing, true", begin_close)


if __name__ == "__main__":
    unittest.main()
