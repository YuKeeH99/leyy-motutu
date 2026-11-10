"""A wake request waits for physical playback cleanup before opening a task."""
import unittest

from test_lingxin_round_races import COMMON, ROOT, body, run_c

SERVER = 'src/server/lingxin_server/lingxin_server.c'


class PlaybackStartGateTest(unittest.TestCase):
    def test_explicit_wake_closes_old_transport_then_resumes_same_request(self):
        source = (ROOT / SERVER).read_text(encoding='utf-8')
        self.assertIn('static int lingxin_close_previous_voice_session(', source,
                      'active wake must retire the old socket without waiting for terminate_task ACK')
        code = COMMON + r'''
#define CONFIG_VOICE_LINK_LINGXIN 1
#define CONFIG_LINGXIN_ENGINE_ENABLE 1
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define LINGXIN_PLAYBACK_READY_WAIT_MS 5000U
#define LINGXIN_PLAYBACK_READY_POLL_MS 20U
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
enum {State_Idle, State_Download_Play, State_Exit};
static unsigned elapsed, settle_at, exit_at, change_at, epoch=17, owner=9;
static uint32_t s_lingxin_exit_sequence;
static bool s_lingxin_exit_callback_complete;
static int state=State_Download_Play, stop_ret, stops;
static int direct_closes;
static bool close_in_progress;
static bool inited=true, ready=true, cancel_in_stop, change_epoch;
static unsigned voice_chat_current_epoch(void){return epoch;}
static int get_current_chat_state(void){return state;}
static bool isVoiceChatInited(void){return inited;}
static bool lingxin_audio_playback_input_ready(void){return ready;}
static bool lingxin_start_request_current(uint32_t o,uint32_t e){return o==owner && e==epoch;}
static uint32_t service_offline_action_generation(void){return owner;}
static bool service_offline_action_online_current(uint32_t value){return value==owner;}
static unsigned suppresses;
static void lingxin_audio_suppress_current_tts(const char *why){suppresses++;}
static int lingxin_wait_playback_input_ready(uint32_t o,uint32_t e){return 0;}
static bool module_voiceChat_exit(void){direct_closes++;if(close_in_progress)return false;inited=false;return true;}
static int lingxin_server_chat_stop(void){
 stops++;state=State_Exit;
 /* Intentional stop may retire the old protocol epoch; the wake owner and
  * clean capture generation belong to the new wake, so they survive. */
 epoch++;
 if(cancel_in_stop)owner++;
 return stop_ret;
}
static void lisa_thread_mdelay(unsigned ms){
 elapsed+=ms;
 if(settle_at && elapsed>=settle_at){state=State_Idle;inited=false;ready=true;}
 if(exit_at && elapsed>=exit_at){s_lingxin_exit_sequence++;s_lingxin_exit_callback_complete=true;exit_at=0;}
 if(change_at && elapsed>=change_at){if(change_epoch)epoch++;else owner++;change_at=0;}
}
static int lingxin_close_previous_voice_session(uint32_t owner, uint32_t *epoch)
''' + body(SERVER, 'lingxin_close_previous_voice_session') + r'''
static int start_preflight_owned(const char *text, bool continue_after_text,uint32_t requested_owner)
''' + body(SERVER, 'lingxin_server_chat_start_internal').split('    lingxin_goodbye_reset("new_chat");')[0] + r'''
 return 0;
#endif
}
static int start_preflight(const char *text,bool query){return start_preflight_owned(text,query,0);}
static void reset(void){
 elapsed=0;settle_at=120;exit_at=200;change_at=0;epoch=17;owner=9;state=State_Download_Play;
 inited=true;ready=false;stop_ret=0;stops=0;suppresses=0;cancel_in_stop=false;change_epoch=false;
 s_lingxin_exit_callback_complete=false;
 direct_closes=0;close_in_progress=false;
}
int main(void){
 uint32_t request_epoch=17;
 reset();owner=10;
 assert(start_preflight_owned(NULL,false,9)==-2 && !suppresses && !stops && !direct_closes);
 reset();assert(lingxin_close_previous_voice_session(9,&request_epoch)==0);
 /* State becomes Idle before the EXIT application callback finishes; the
  * old callback must not mark the new session finished after it starts. */
 assert(stops==1 && elapsed==200 && request_epoch==18 && !inited && state==State_Idle);
 reset();request_epoch=17;stop_ret=-1;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==-1 && stops==1 && !elapsed);
 reset();request_epoch=17;settle_at=0;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==-1 && elapsed==5000);
 reset();request_epoch=17;cancel_in_stop=true;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==-2 && !elapsed);
 reset();request_epoch=17;change_at=40;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==-2 && elapsed==40);
 reset();request_epoch=17;change_at=40;change_epoch=true;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==-2 && elapsed==40);
 reset();request_epoch=16;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==-2 && stops==0);
 /* Old session can finish naturally between the caller's active-state check
  * and entry here. Do not wait for another EXIT that will never occur. */
 reset();request_epoch=17;state=State_Idle;inited=false;ready=true;s_lingxin_exit_callback_complete=true;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==0 && stops==0 && !elapsed);
 reset();request_epoch=17;state=State_Idle;inited=false;ready=true;
 assert(lingxin_close_previous_voice_session(9,&request_epoch)==0 && stops==0 && elapsed==200);
 /* Exercise the real caller branch: observing Idle must not skip a pending
  * EXIT callback and let it finish a newly opened task. */
 reset();state=State_Idle;inited=false;ready=true;
 assert(start_preflight(NULL,false)==0 && !stops && elapsed==200);
 reset();state=State_Idle;inited=false;ready=true;s_lingxin_exit_callback_complete=true;
 assert(start_preflight(NULL,false)==0 && !stops && !direct_closes && !elapsed);
 /* An old idle handler still gets the existing direct-close recovery. */
 reset();state=State_Idle;ready=true;s_lingxin_exit_callback_complete=true;
 assert(start_preflight(NULL,false)==0 && !stops && direct_closes==1 && !elapsed && !inited);
 reset();state=State_Idle;ready=true;s_lingxin_exit_callback_complete=true;close_in_progress=true;
 assert(start_preflight(NULL,false)==0 && !stops && direct_closes==1 && elapsed==120 && !inited);
 reset();ready=true;
 assert(start_preflight("generic shell text",false)==0 && !stops && !direct_closes && !elapsed);
 /* A query worker's own stop can observe Idle before EXIT finishes too. */
 reset();state=State_Idle;inited=false;ready=true;
 assert(start_preflight("local device result",true)==0 && !stops && elapsed==200);
 reset();state=State_Idle;ready=true;s_lingxin_exit_callback_complete=true;
 assert(start_preflight("local device result",true)==0 && !stops && direct_closes==1 && !elapsed);
}
'''
        run_c(code)

    def test_transport_retirement_is_for_wake_or_device_query_only(self):
        start = body(SERVER, 'lingxin_server_chat_start_internal')
        self.assertIn('lingxin_close_previous_voice_session(requested_owner, &start_epoch)', start)
        close = start.index('lingxin_close_previous_voice_session(')
        self.assertLess(start.index('lingxin_wait_playback_input_ready('), close)
        self.assertLess(close, start.index('lingxin_goodbye_reset('))
        self.assertLess(close, start.index('get_start_new_chat_default_props()'))
        guarded = start[start.rfind('if (', 0, close):close]
        self.assertIn('text == NULL', guarded)
        self.assertIn('continue_after_text', guarded)
        self.assertNotIn('get_current_chat_state() != State_Idle', guarded)
        self.assertNotIn('module_voiceChat_exit()', start)
        listener = body(SERVER, 'lingxin_chat_life_cycle_listener')
        self.assertLess(listener.index('lingxin_publish_chat_finished()'),
                        listener.index('__atomic_add_fetch(&s_lingxin_exit_sequence'))
        self.assertIn('__atomic_store_n(&s_lingxin_exit_callback_complete, false',
                      body(SERVER, 'lingxin_publish_chat_started'))

    def test_cleanup_completion_resumes_same_request_and_wait_is_cancellable(self):
        code = COMMON + r'''
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define LINGXIN_PLAYBACK_READY_WAIT_MS 5000U
#define LINGXIN_PLAYBACK_READY_POLL_MS 20U
#define LOGW(...) ((void)0)
static unsigned elapsed, ready_at, epoch=17, owner=9, change_at;
static bool change_owner;
static bool service_offline_action_online_current(uint32_t requested){return requested==owner;}
static unsigned voice_chat_current_epoch(void){return epoch;}
static bool lingxin_audio_playback_input_ready(void){return elapsed>=ready_at;}
static void lisa_thread_mdelay(unsigned ms){
 elapsed+=ms;
 if(change_at && elapsed>=change_at){if(change_owner)owner++;else epoch++;change_at=0;}
}
static bool lingxin_start_request_current(uint32_t owner, uint32_t epoch)
''' + body(SERVER, 'lingxin_start_request_current') + r'''
static int lingxin_wait_playback_input_ready(uint32_t owner, uint32_t epoch)
''' + body(SERVER, 'lingxin_wait_playback_input_ready') + r'''
int main(void){
 ready_at=2400;assert(lingxin_wait_playback_input_ready(9,17)==0);assert(elapsed==2400);
 /* No new wake request or protocol round was created while waiting. */
 assert(owner==9 && epoch==17);
 elapsed=0;ready_at=0;assert(lingxin_wait_playback_input_ready(9,17)==0 && !elapsed);
 elapsed=0;ready_at=10000;assert(lingxin_wait_playback_input_ready(9,17)==-1 && elapsed==5000);
 elapsed=0;change_at=100;assert(lingxin_wait_playback_input_ready(9,17)==-2 && elapsed==100);
 epoch=17;elapsed=0;change_at=80;change_owner=true;
 assert(lingxin_wait_playback_input_ready(9,17)==-2 && elapsed==80);
 /* Readiness never revives a request cancelled by another owner. */
 elapsed=0;ready_at=0;assert(lingxin_wait_playback_input_ready(9,17)==-2 && !elapsed);
}
'''
        run_c(code)

    def test_gate_precedes_forced_idle_and_actual_task_start(self):
        source = (ROOT / SERVER).read_text(encoding='utf-8')
        start = body(SERVER, 'lingxin_server_chat_start_internal')
        wait = start.index('lingxin_wait_playback_input_ready(')
        self.assertLess(wait, start.index('state_machine_force_reset_to_idle()'))
        self.assertLess(wait, start.index('lingxin_goodbye_reset('))
        final_gate = start.rindex('lingxin_audio_playback_input_ready()')
        self.assertLess(final_gate, start.index('ret = start_new_chat(&props)'))
        self.assertGreater(final_gate, start.index('sys_wifi_set_realtime_mode(true)'))
        self.assertIn('lingxin_start_request_current(requested_owner, start_epoch)', start)
        self.assertIn('if (playback_ready != 0) return playback_ready;', start)
        wake_worker = (ROOT / 'src/category/comm/msgs/voice_wakeup_msg.c').read_text(encoding='utf-8')
        self.assertIn('if (start_ret != -2)', wake_worker)


if __name__ == '__main__':
    unittest.main()
