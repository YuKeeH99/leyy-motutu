import unittest
from tools.test_offline_action import OfflineActionTest, ROOT, c_function


class WakeNetworkFeedbackTest(unittest.TestCase):
    run_c = OfflineActionTest.run_c

    def test_confirmed_probe_failure_does_not_require_another_failure_event(self):
        source = (ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        branch = source.split('case VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL:', 1)[1].split('case VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS:', 1)[0]
        self.assertIn('s_disconnect_tone_played = voice_player_play_network_unavailable_tone()', branch)

    def test_reminder_only_after_current_wake_ack_in_online_mode(self):
        source = (ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {bool connected;} sys_network_status_t;
static bool current=true,allowed=true,connected=false,wake_allowed=true;
static int reminds;
static bool service_offline_action_wake_network_allowed(uint32_t g) {return wake_allowed;}
static bool service_offline_action_prompt_current(uint32_t g) {return current;}
static bool service_offline_action_network_prompt_allowed(void) {return allowed;}
static int sys_network_get_status(sys_network_status_t *s) {s->connected=connected;return 0;}
static bool voice_player_play_network_unavailable_tone(void) {reminds++;return true;}
''' + c_function(source, 'voice_player_offline_wake_network_feedback') + r'''
int main(void) {
 voice_player_offline_wake_network_feedback(7);assert(reminds==1);
 wake_allowed=false;voice_player_offline_wake_network_feedback(7);assert(reminds==1);
 wake_allowed=true;connected=true;voice_player_offline_wake_network_feedback(7);assert(reminds==1);
 connected=false;allowed=false;voice_player_offline_wake_network_feedback(7);assert(reminds==1);
 allowed=true;current=false;voice_player_offline_wake_network_feedback(7);assert(reminds==1);
 return 0;
}
''')
