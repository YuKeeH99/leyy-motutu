import unittest
from tools import test_offline_action as support

class WakeupGateGenerationTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_old_completion_cannot_replace_new_wake_release(self):
        source = (support.ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define CONFIG_VOICE_LINK_LINGXIN 1
static int critical_depth;
#define taskENTER_CRITICAL() (++critical_depth)
#define taskEXIT_CRITICAL() (--critical_depth)
#define pdMS_TO_TICKS(n) (n)
#define pdPASS 1
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define WAKEUP_UPLINK_GATE_FAILSAFE_MS 5000U
typedef uint32_t TickType_t;
typedef void *TimerHandle_t;
static TimerHandle_t s_wakeup_gate_timer=(void*)1;
static uint32_t s_latest_wakeup_gate_generation,s_wakeup_gate_timer_generation;
static TickType_t now,s_wakeup_gate_not_before_tick;
static uint32_t current;
static bool gate_open;
static unsigned timer_changes;
static TickType_t xTaskGetTickCount(void) {return now;}
static int xTimerChangePeriod(void *t,unsigned period,unsigned wait) {assert(critical_depth>0);++timer_changes;return 1;}
static uint32_t lingxin_server_audio_uplink_gate_hold(const char *r) {gate_open=false;return ++current;}
static void lingxin_server_audio_uplink_gate_open(uint32_t generation,const char *r) {if(generation==current)gate_open=true;}
'''
        code += ''.join(support.c_function(source, f) for f in (
            'voice_player_schedule_wakeup_gate_release','voice_player_begin_wakeup_gate',
            'voice_player_wakeup_gate_timer_cb'))
        code += r'''
int main(void) {
 for(unsigned i=0;i<1000;i++) {
  uint32_t old=voice_player_begin_wakeup_gate("old");
  /* Old completion passed the caller's generation check before a new wake. */
  uint32_t fresh=voice_player_begin_wakeup_gate("interrupt_tts");
  unsigned changes=timer_changes;
  voice_player_schedule_wakeup_gate_release(old,200,"late_old_completion");
  assert(s_wakeup_gate_timer_generation==fresh && timer_changes==changes);
  now+=200;voice_player_wakeup_gate_timer_cb(NULL);assert(!gate_open);
  now+=4800;voice_player_wakeup_gate_timer_cb(NULL);assert(gate_open);
  fresh=voice_player_begin_wakeup_gate("next");
  voice_player_schedule_wakeup_gate_release(fresh,200,"current_complete");
  now+=199;voice_player_wakeup_gate_timer_cb(NULL);assert(!gate_open);
  now++;voice_player_wakeup_gate_timer_cb(NULL);assert(gate_open);
  fresh=voice_player_begin_wakeup_gate("aec_wake");
  changes=timer_changes;
  voice_player_schedule_wakeup_gate_release(fresh,0,"dac_drained");
  assert(gate_open && timer_changes==changes); /* No 200 ms blind interval. */
  old=fresh;fresh=voice_player_begin_wakeup_gate("new_wake");
  voice_player_schedule_wakeup_gate_release(old,0,"stale_drain");assert(!gate_open);
 }
 return 0;
}
'''
        self.run_c(code)
