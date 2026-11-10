"""Exercise actual prompt callbacks at the speaker/capture boundary."""
import unittest
from tools import test_offline_action as support


class PromptCaptureBoundaryTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_completion_drains_once_before_releasing_capture(self):
        source = (support.ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        functions = ''.join(support.c_function(source, name) for name in (
            'voice_player_local_tone_event', 'on_tone_event'))
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define WAKEUP_UPLINK_TAIL_GUARD_MS 0U
typedef void app_player_t;
typedef enum { APP_PLAYER_EVENT_PLAYING, APP_PLAYER_EVENT_COMPLETED,
 APP_PLAYER_EVENT_STOPPED, APP_PLAYER_EVENT_ERROR } app_player_event_t;
static bool s_local_tone_active, s_shutdown_prompt_in_progress, s_shutdown_tone_playing;
static bool s_current_tone_prompt, released;
static const char s_upload_cue_tag[]="upload_complete";
static const char *s_current_tone_url;
static uint32_t s_upload_cue_epoch;
static void voice_player_upload_cue_event(app_player_event_t event) {
 if(event==APP_PLAYER_EVENT_COMPLETED || event==APP_PLAYER_EVENT_STOPPED || event==APP_PLAYER_EVENT_ERROR)
  s_upload_cue_epoch=0;
}
static int s_shutdown_tone_result;
static uint32_t s_current_offline_generation, s_current_tone_wakeup_generation;
static uint32_t s_latest_wakeup_gate_generation;
static struct { uint32_t local_generation, token_index, token_count; } s_current_local_request;
static unsigned drains, elapsed;
static void voice_player_drain_wakeup_output(void) {
 assert(!released); ++drains;
 /* RUN-state FLUSH waits buffer_ms + 10 even when the queue is empty. */
 elapsed += 26;
}
static void voice_player_local_tone_gate_begin(void) {s_local_tone_active=true;}
static void voice_player_local_tone_gate_end(void) {s_local_tone_active=false;}
static void voice_player_boot_rgb_event(app_player_event_t e) {}
static bool voice_player_advance_local_sequence(void) {return false;}
static void voice_player_start_next_queued_tone(void) {}
static void voice_player_offline_wake_network_feedback(uint32_t g) {}
static void service_offline_action_prompt_finished(uint32_t g, bool ok) {assert(drains==1);}
static void voice_player_prompt_tone_exit(void) {}
static bool alarm_ring_is_active(void) {return false;}
static void alarm_ring_notify_playback_complete(void) {}
static void voice_player_flush_tone_queue(void) {}
static void voice_player_schedule_wakeup_gate_release(uint32_t g,uint32_t ms,const char *why) {
 assert(drains==1 && !s_local_tone_active); released=true;
}
''' + functions + r'''
static void reset(void) {
 s_local_tone_active=true; s_shutdown_prompt_in_progress=s_shutdown_tone_playing=false;
 s_current_offline_generation=s_current_tone_wakeup_generation=0;
 s_current_local_request.local_generation=0;
 drains=elapsed=0; released=false;
}
int main(void) {
 reset(); s_current_tone_wakeup_generation=s_latest_wakeup_gate_generation=7;
 on_tone_event(NULL,APP_PLAYER_EVENT_COMPLETED,NULL);
 assert(drains==1 && elapsed==26 && released);
 reset(); s_current_offline_generation=8;
 on_tone_event(NULL,APP_PLAYER_EVENT_COMPLETED,NULL); assert(drains==1);
 reset(); s_current_tone_url=s_upload_cue_tag; s_upload_cue_epoch=8;
 on_tone_event(NULL,APP_PLAYER_EVENT_COMPLETED,NULL);
 assert(drains==1 && s_upload_cue_epoch==0);
 s_current_tone_url=NULL;
 reset(); s_current_local_request.local_generation=9;
 s_current_local_request.token_count=1;
 on_tone_event(NULL,APP_PLAYER_EVENT_COMPLETED,NULL); assert(drains==1);
 reset(); s_shutdown_prompt_in_progress=s_shutdown_tone_playing=true;
 on_tone_event(NULL,APP_PLAYER_EVENT_COMPLETED,NULL);
 assert(drains==1 && s_shutdown_tone_result==0);
 /* Missing PLAYING callback still requires one physical drain. */
 reset(); s_local_tone_active=false;
 s_current_tone_wakeup_generation=s_latest_wakeup_gate_generation=10;
 on_tone_event(NULL,APP_PLAYER_EVENT_COMPLETED,NULL); assert(drains==1 && released);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
