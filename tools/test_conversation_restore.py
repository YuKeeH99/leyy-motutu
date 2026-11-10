"""Static rollback boundaries, not a latency or real-cloud test."""
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = "b544bd4a6c62926623e8a8878b0245ea72d9f2f3"
PLAYER = "apps/arcs-mini/voice_player.c"


def baseline(path):
    return subprocess.check_output(
        ["git", "show", f"{BASE}:{path}"], cwd=ROOT
    ).decode("utf-8").replace("\r\n", "\n").rstrip()


def current(path):
    return (ROOT / path).read_text(encoding="utf-8").rstrip()


def function(text, name):
    # Mask strings/comments so braces in messages do not affect nesting.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: " " * len(m.group()), text, flags=re.S)
    match = re.search(r"\b" + name + r"\([^;{}]*\)\s*\{", masked)
    if not match:
        raise AssertionError(f"Missing function {name}")
    start = masked.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    return text[start:end]


def without_rgb_lifecycle_hooks(text):
    # Only these approved, separately behavior-tested RGB hooks are outside
    # the dialogue rollback boundary. Keep all audio statements comparable.
    for hook in (
        '#ifdef CONFIG_BOARD_ARCS_MINI_V2_1\n'
        '    const char *boot_url = app_tone_get_url(TONE_ID_106);\n'
        '    s_rgb_boot_tone = boot_url != NULL && strcmp(request->url, boot_url) == 0;\n'
        '#endif\n',
        '#ifdef CONFIG_BOARD_ARCS_MINI_V2_1\n'
        '        voice_player_boot_rgb_event(APP_PLAYER_EVENT_ERROR);\n'
        '#endif\n',
        '#ifdef CONFIG_BOARD_ARCS_MINI_V2_1\n'
        '    voice_player_boot_rgb_event(APP_PLAYER_EVENT_STOPPED);\n'
        '#endif\n',
    ):
        text = text.replace(hook, '')
    return text


class ConversationRestoreTest(unittest.TestCase):
    def test_original_dialogue_handlers_are_unchanged(self):
        before, after = baseline(PLAYER), current(PLAYER)
        for name in (
            "async_play_task", "async_play_init", "async_play_url_internal",
            "voice_player_wait_pushup_tts_mp3_ready", "voice_player_download_pushup_tts_mp3",
            "voice_player_tts_track_url", "voice_player_play_msg", "on_tts_event",
            "voice_player_play_wakeup_tone", "voice_player_get_wakeup_tone_url",
            "voice_player_start_tone_request", "voice_player_start_next_queued_tone",
            "voice_player_flush_tone_queue", "voice_player_audio_item",
            "voice_player_play_control", "voice_alarm_ring_play_once",
            "voice_alarm_ring_force_stop", "voice_player_init",
        ):
            with self.subTest(function=name):
                self.assertEqual(function(before, name), without_rgb_lifecycle_hooks(function(after, name)))

    def test_original_intent_and_sdk_playback(self):
        paths = ["apps/arcs-mini/voice_intent/voice_intent_mgr.c"]
        paths += ["arcs-sdk/components/app_player/" + name for name in (
            "app_player.c", "app_player.h", "app_player_core.c", "app_player_core.h",
            "app_player_focus.c", "app_player_internal.h")]
        for path in paths:
            with self.subTest(path=path):
                self.assertEqual(baseline(path), current(path))

    def test_no_global_tone_fences_or_transition_worker(self):
        source = current(PLAYER)
        for token in ("tone.next", "s_tone_operation_lock", "app_player_wait_callbacks_idle",
                      "s_tone_generation", "app_player_set_post_stop_error_dedup"):
            self.assertNotIn(token, source)
        self.assertIn("TONE_ID_106", source)
        self.assertIn("TONE_ID_107", source)
        self.assertIn("voice_player_try_play_prompt_tone_url", source)

    def test_only_action_dispatch_changes_in_cloud_server(self):
        path = "src/server/lingxin_server/lingxin_server.c"
        before, after = baseline(path), current(path)
        name = "lingxin_handle_text_output"
        self.assertEqual(before.replace(function(before, name), "ACTION_DISPATCH"),
                         after.replace(function(after, name), "ACTION_DISPATCH"))
        self.assertIn("VOICE_MSG_CLOUD_ACTION_LIST", function(after, name))

    def test_poweroff_wait_is_local_and_bounded(self):
        source = current(PLAYER)
        shutdown = function(source, "voice_player_play_shutdown_prompt_sync")
        self.assertIn("pdMS_TO_TICKS(4000U)", shutdown)
        self.assertIn("pdMS_TO_TICKS(6000U)", shutdown)
        self.assertIn("boot_watchdog_feed", shutdown)
        self.assertIn("power_is_usb_plugged", shutdown)
        self.assertNotIn("vTaskDelay", function(source, "on_tts_event"))


if __name__ == "__main__":
    unittest.main()
