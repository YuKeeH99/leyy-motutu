import unittest
from tools import test_offline_action as support


class LocalToneUplinkGateTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_mode_control_guard_is_nonblocking_and_covers_speaker_tail(self):
        voice = (support.ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static bool s_tts_active,s_tone_playing,s_local_tone_active,s_local_tone_tail_pending;
static uint32_t now,s_local_tone_tail_until;
static uint32_t s_beat_music_owner;
static uint32_t xTaskGetTickCount(void) {return now;}
''' + support.c_function(voice, 'voice_player_mode_command_blocked') + r'''
int main(void) {
 assert(!voice_player_mode_command_blocked());
 s_beat_music_owner=7;assert(voice_player_mode_command_blocked());s_beat_music_owner=0;
 s_tts_active=true;assert(voice_player_mode_command_blocked());s_tts_active=false;
 s_tone_playing=true;assert(voice_player_mode_command_blocked());s_tone_playing=false;
 s_local_tone_active=true;assert(voice_player_mode_command_blocked());s_local_tone_active=false;
 s_local_tone_tail_pending=true;s_local_tone_tail_until=200;
 now=199;assert(voice_player_mode_command_blocked());
 now=200;assert(!voice_player_mode_command_blocked());
 now=UINT32_MAX-100;s_local_tone_tail_until=now+200;
 assert(voice_player_mode_command_blocked());now+=200;
 assert(!voice_player_mode_command_blocked());
 return 0;
}
''')

    def test_local_tone_hold_survives_wakeup_release_and_covers_tail(self):
        voice = (support.ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        audio = (support.ROOT / 'src/server/lingxin_server/adapter/lingxin_audio_adapter.c').read_text(encoding='utf-8')
        functions = ''.join(support.c_function(voice, name) for name in (
            'voice_player_local_tone_gate_begin', 'voice_player_local_tone_gate_end',
            'voice_player_local_tone_blocks_uplink', 'voice_player_local_tone_event'))
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define pdMS_TO_TICKS(n) (n)
#define WAKEUP_UPLINK_TAIL_GUARD_MS 200U
#define LOCAL_COMMAND_TAIL_GUARD_MS 200U
typedef uint32_t TickType_t;
static TickType_t now,s_local_tone_tail_until;
typedef enum {APP_PLAYER_EVENT_PLAYING,APP_PLAYER_EVENT_COMPLETED,APP_PLAYER_EVENT_STOPPED,APP_PLAYER_EVENT_ERROR} app_player_event_t;
static void voice_player_drain_wakeup_output(void) {}
static void *tone_player=(void*)1;
static void *beat_player=(void*)2;
static uint32_t s_beat_music_owner;
static bool callback_pending;
static bool app_player_callbacks_idle(void *p) {return !callback_pending;}
static bool s_local_tone_active,s_local_tone_tail_pending;
static TickType_t xTaskGetTickCount(void) {return now;}
static bool online=true,s_record_uplink_gate_open=true;
static uint32_t s_record_uplink_gate_generation=3;
static unsigned service_offline_action_generation(void) {return 1;}
static bool service_offline_action_online_current(unsigned g) {return online;}
''' + functions + support.c_function(audio, 'lingxin_audio_uplink_gate_is_open') + r'''
int main(void) {
 assert(lingxin_audio_uplink_gate_is_open(0));
 s_beat_music_owner=1;assert(!lingxin_audio_uplink_gate_is_open(0));s_beat_music_owner=0;
 voice_player_local_tone_gate_begin();
 assert(!lingxin_audio_uplink_gate_is_open(0));
 now=6000; /* A five-second wake gate failsafe must not leak a longer prompt. */
 s_record_uplink_gate_open=true;assert(!lingxin_audio_uplink_gate_is_open(0));
 voice_player_local_tone_gate_end();
 now+=199;assert(!lingxin_audio_uplink_gate_is_open(0));
 now++;assert(lingxin_audio_uplink_gate_is_open(0));
 /* Old STOPPED can arrive after a new request begins. Queue backlog stays
  * blocked, and the new PLAYING event must reassert the hold. */
 voice_player_local_tone_gate_begin();callback_pending=true;
 voice_player_local_tone_event(APP_PLAYER_EVENT_STOPPED);
 now+=1000;assert(!lingxin_audio_uplink_gate_is_open(0));
 voice_player_local_tone_event(APP_PLAYER_EVENT_PLAYING);callback_pending=false;
 now+=1000;assert(!lingxin_audio_uplink_gate_is_open(0));
 voice_player_local_tone_event(APP_PLAYER_EVENT_COMPLETED);
 now+=200;assert(lingxin_audio_uplink_gate_is_open(0));
 voice_player_local_tone_gate_begin();voice_player_local_tone_gate_end();
 now+=50;voice_player_local_tone_gate_begin();now+=300;
 assert(!lingxin_audio_uplink_gate_is_open(0));
 voice_player_local_tone_gate_end();now+=200;
 s_record_uplink_gate_open=false;assert(!lingxin_audio_uplink_gate_is_open(0));
 s_record_uplink_gate_open=true;online=false;assert(!lingxin_audio_uplink_gate_is_open(0));
 online=true;now=UINT32_MAX-100;voice_player_local_tone_gate_begin();voice_player_local_tone_gate_end();
 now+=199;assert(!lingxin_audio_uplink_gate_is_open(0));
 now++;assert(lingxin_audio_uplink_gate_is_open(0));
 return 0;
}
''')

    def test_gate_starts_before_speaker_and_clears_recording_queue(self):
        voice = (support.ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        start = support.c_function(voice, 'voice_player_start_tone_request')
        self.assertLess(start.index('voice_player_local_tone_gate_begin()'), start.index('app_player_play(tone_player'))
        callback = support.c_function(voice, 'on_tone_event')
        self.assertIn('voice_player_local_tone_event(event)', callback)
        audio = (support.ROOT / 'src/server/lingxin_server/adapter/lingxin_audio_adapter.c').read_text(encoding='utf-8')
        capture = support.c_function(audio, 'lingxin_record_audio_callback')
        blocked = capture.split('if (!lingxin_audio_uplink_gate_is_open(NULL))', 1)[1].split('return;', 1)[0]
        self.assertIn('lingxin_record_discard_buffered(recorder)', blocked)
