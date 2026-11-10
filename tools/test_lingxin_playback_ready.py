"""Recording must not reuse a player with unfinished or failed cleanup."""
import unittest
from test_lingxin_round_races import COMMON, body, run_c

ADAPTER = "src/server/lingxin_server/adapter/lingxin_audio_adapter.c"


class PlaybackReadyTest(unittest.TestCase):
    def test_failed_reset_and_pending_executor_do_not_look_ready(self):
        run_c(COMMON + r'''
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static bool s_stream_playing, s_stream_writer_exited=true;
static bool s_player_cleanup_started, s_player_cleanup_running, s_playback_input_blocked;
static uint32_t s_stream_enqueue_active, s_pending_termination_generation, s_playback_generation=7;
bool lingxin_audio_playback_input_ready(void)
''' + body(ADAPTER, "lingxin_audio_playback_input_ready") + r'''
static void lingxin_playback_release_input(uint32_t generation)
''' + body(ADAPTER, "lingxin_playback_release_input") + r'''
int main(void) {
 assert(lingxin_audio_playback_input_ready());
 s_pending_termination_generation=7;assert(!lingxin_audio_playback_input_ready());
 s_pending_termination_generation=0;s_player_cleanup_started=true;
 assert(!lingxin_audio_playback_input_ready());
 s_player_cleanup_started=false;s_player_cleanup_running=true;
 assert(!lingxin_audio_playback_input_ready());
 s_player_cleanup_running=false;s_playback_input_blocked=true;
 assert(!lingxin_audio_playback_input_ready()); /* reset returned an error */
 lingxin_playback_release_input(6);assert(!lingxin_audio_playback_input_ready());
 lingxin_playback_release_input(7);assert(lingxin_audio_playback_input_ready());
 s_stream_writer_exited=false;assert(!lingxin_audio_playback_input_ready());
 s_stream_writer_exited=true;s_stream_enqueue_active=1;assert(!lingxin_audio_playback_input_ready());
 s_stream_enqueue_active=0;s_stream_playing=true;assert(!lingxin_audio_playback_input_ready());
}
''')


if __name__ == "__main__":
    unittest.main()
