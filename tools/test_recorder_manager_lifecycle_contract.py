#!/usr/bin/env python3
"""Lifecycle contracts for the Lingxin recorder sender task."""

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


class RecorderManagerLifecycleContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manager = (
            ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/recorder_manager.c"
        ).read_text(encoding="utf-8")
        cls.adapter = (
            ROOT / "src/server/lingxin_server/adapter/lingxin_thread_adapter.c"
        ).read_text(encoding="utf-8")

    def test_sender_never_becomes_a_sleeping_zombie_or_uses_forced_cancel(self) -> None:
        start = function_body(self.manager, "start_send_thread")
        sender = function_body(self.manager, "send_record")

        self.assertNotIn("send_thread_pid_ptr", self.manager)
        self.assertIn("lingxin_thread_create(&thread_id", start)
        self.assertNotIn("lingxin_thread_sleep(60000)", sender)
        self.assertIn("goto send_record_thread_exit", sender)
        self.assertNotIn("LINGXIN_THREAD_DESTROY_CANCEL", self.manager)

    def test_immediate_stop_does_not_sleep_while_holding_lifecycle_mutex(self) -> None:
        stop = function_body(self.manager, "stop_send_thread")
        stop_signal = stop.find("send_thread_stop_flag = 1")
        unlock = stop.find("lingxin_mutex_unlock(send_thread_mutex)", stop_signal)
        wait = stop.find("lingxin_thread_sleep", stop_signal)

        self.assertGreaterEqual(stop_signal, 0)
        self.assertGreater(unlock, stop_signal)
        self.assertGreater(wait, unlock, "sender wait must happen after releasing lifecycle mutex")
        self.assertIn("recorder_thread_stop_timeout", stop)

    def test_sender_publishes_stopped_only_after_owned_sync_cleanup(self) -> None:
        sender = function_body(self.manager, "send_record")
        lock = sender.find("lingxin_mutex_lock(send_thread_mutex)", sender.index("send_record_thread_exit:"))
        cleanup = sender.find("cleanup_send_thread_sync_objects", lock)
        stopped = sender.find("send_thread_running = 0", lock)
        unlock = sender.find("lingxin_mutex_unlock(send_thread_mutex)", lock)

        self.assertGreaterEqual(lock, 0)
        self.assertGreater(cleanup, lock)
        self.assertGreater(stopped, cleanup)
        self.assertGreater(unlock, stopped)

    def test_audio_producer_uses_the_sender_lifecycle_lock(self) -> None:
        producer = function_body(self.manager, "lingxin_process_record_data")
        self.assertIn("lingxin_mutex_lock(send_thread_mutex)", producer)
        self.assertIn("lingxin_mutex_unlock(send_thread_mutex)", producer)

    def test_send_flag_never_posts_sender_semaphore_without_lifecycle_lock(self) -> None:
        setter = function_body(self.manager, "set_send_flag")
        lock = setter.find("lingxin_mutex_lock(send_thread_mutex)")
        post = setter.find("lingxin_semaphore_post")
        unlock = setter.find("lingxin_mutex_unlock(send_thread_mutex)")

        self.assertGreaterEqual(lock, 0)
        self.assertGreater(post, lock)
        self.assertGreater(unlock, post)

    def test_audio_adapter_waits_for_sender_exit_before_deleting_queue(self) -> None:
        audio = (
            ROOT / "src/server/lingxin_server/adapter/lingxin_audio_adapter.c"
        ).read_text(encoding="utf-8")
        stop = function_body(audio, "lingxin_record_sender_stop")
        sender = function_body(audio, "lingxin_record_sender_thread")

        self.assertIn("sender_exited", sender)
        self.assertIn("sender_exited", stop)
        wait = stop.find("lisa_thread_mdelay")
        delete = stop.find("lisa_queue_delete")
        self.assertGreaterEqual(wait, 0)
        self.assertGreater(delete, wait)

    def test_thread_trampoline_deletes_current_task_after_entry_returns(self) -> None:
        trampoline = function_body(self.adapter, "lingxin_thread_trampoline")
        free = trampoline.find("lisa_mem_free(ctx)")
        delete = trampoline.find("vTaskDelete(NULL)")

        self.assertGreaterEqual(free, 0)
        self.assertGreater(delete, free)


if __name__ == "__main__":
    unittest.main()
