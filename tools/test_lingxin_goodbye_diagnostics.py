"""Exercise goodbye observability without logging conversation content."""
import unittest

from test_lingxin_round_races import COMMON, ROOT, body, run_c

SERVER = 'src/server/lingxin_server/lingxin_server.c'


class GoodbyeDiagnosticsTest(unittest.TestCase):
    def test_received_dispatched_handled_and_actual_exit_are_distinct(self):
        code = COMMON + '\n#undef lingxin_server_diag_timing_mark\n#include "' + (ROOT / 'src/server/lingxin_server/lingxin_server.h').as_posix() + '"\n' + r'''
#define CONFIG_LINGXIN_ENGINE_ENABLE 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
#define LINGXIN_GOODBYE_EXIT_TIMEOUT_MS 8000U
#define VOICE_MSG_CLOUD_MCP_CHAT_EXIT 42
#define pdMS_TO_TICKS(ms) (ms)
#define pdFALSE 0
#define pdPASS 1
typedef void *TimerHandle_t;
static TimerHandle_t s_lingxin_goodbye_timer;
static uint32_t s_lingxin_chat_generation=7,s_lingxin_goodbye_generation;
static volatile bool s_lingxin_goodbye_pending;
static lingxin_goodbye_stats_t s_lingxin_goodbye_stats;
static unsigned published;
static int publish_ret;
static unsigned voice_chat_current_epoch(void){return 19;}
static unsigned lisa_os_get_tick_ms(void){return 1000;}
static int xTimerIsTimerActive(void*p){return 1;}
static int xTimerStop(void*p,int n){return 1;}
static void *xTimerCreate(const char*n,unsigned p,int a,void*d,void(*cb)(void*)){return (void*)1;}
static int xTimerChangePeriod(void*p,unsigned m,int n){return 1;}
static int voice_msg_pub_timeout(int id,const void*p,unsigned n,unsigned t){published++;return publish_ret;}
static void lingxin_goodbye_timeout_cb(TimerHandle_t timer);
''' + '\n'.join(signature + body(SERVER, name) for name, signature in [
            ('lingxin_goodbye_reset', 'static void lingxin_goodbye_reset(const char *reason)'),
            ('lingxin_goodbye_dispatch_exit', 'static void lingxin_goodbye_dispatch_exit(const char *reason)'),
            ('lingxin_goodbye_timeout_cb', 'static void lingxin_goodbye_timeout_cb(TimerHandle_t timer)'),
            ('lingxin_goodbye_arm', 'static void lingxin_goodbye_arm(void)'),
            ('lingxin_server_note_chat_exit_handled', 'void lingxin_server_note_chat_exit_handled(int result)'),
            ('lingxin_server_get_goodbye_stats', 'void lingxin_server_get_goodbye_stats(lingxin_goodbye_stats_t *stats)'),
        ]) + r'''
int main(void){
 lingxin_goodbye_stats_t s={0};
 lingxin_goodbye_arm();lingxin_server_get_goodbye_stats(&s);
 assert(s.received==1 && s.last_received_generation==7 && s.last_received_epoch==19);
 assert(s.pending && s.pending_generation==7 && s.dispatched==0 && s.handled==0);
 lingxin_goodbye_dispatch_exit("playback_complete");lingxin_server_get_goodbye_stats(&s);
 assert(!s.pending && s.dispatched==1 && s.last_dispatched_generation==7 && published==1);
 assert(s.handled==0 && s.session_exits==0 && s.publish_failures==0);
 lingxin_server_note_chat_exit_handled(-3);lingxin_server_get_goodbye_stats(&s);
 assert(s.handled==1 && s.last_stop_result==-3 && s.session_exits==0);
 lingxin_goodbye_dispatch_exit("duplicate");assert(published==1);
 s_lingxin_chat_generation=8;lingxin_goodbye_arm();publish_ret=-1;
 lingxin_goodbye_timeout_cb(NULL);lingxin_server_get_goodbye_stats(&s);
 assert(s.received==2 && s.dispatched==2 && s.publish_failures==1 && s.timeouts==1);
 s_lingxin_chat_generation=9;lingxin_goodbye_arm();s_lingxin_chat_generation=10;
 lingxin_goodbye_dispatch_exit("stale");assert(published==2);
 lingxin_goodbye_reset("new_chat");lingxin_server_get_goodbye_stats(&s);
 assert(s.received==3 && s.last_received_generation==9 && !s.pending);
 lingxin_server_get_goodbye_stats(NULL);
}
'''
        run_c(code)

    def test_actual_session_exit_and_consumer_are_instrumented(self):
        listener = body(SERVER, 'lingxin_chat_life_cycle_listener')
        self.assertIn('s_lingxin_goodbye_stats.session_exits++', listener)
        self.assertIn('s_lingxin_goodbye_stats.last_exit_code = exit_code', listener)
        consumer = body('src/category/comm/msgs/voice_cloud_msg.c', 'voice_cloud_mcp_chat_exit')
        self.assertIn('lingxin_server_note_chat_exit_handled', consumer)
        shell = (ROOT / 'src/shell/cmd/lingxin.c').read_text(encoding='utf-8')
        self.assertIn('{"goodbye", lingxin_cmd_goodbye,', shell)


if __name__ == '__main__':
    unittest.main()
