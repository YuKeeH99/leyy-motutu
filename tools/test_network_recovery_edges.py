"""Execute real Wi-Fi recovery functions with deterministic failure injection."""
import re
import unittest
from tools import test_offline_action as support


class NetworkRecoveryEdgesTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_transient_ap_blacklist_expires_but_explicit_disconnect_does_not(self):
        source = (support.ROOT/'arcs-sdk/modules/wifi_manager/src/wifi_manager.c').read_text(encoding='utf-8')
        record = re.search(r'typedef struct \{\s*sys_dnode_t node;\s*char ssid\[32\];.*?} wifi_mgr_ap_failure_record_t;', source, re.S).group()
        dlist = (support.ROOT/'arcs-sdk/modules/wifi_manager/include/wifi_manager/dlist.h').as_posix()
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
''' + f'#include "{dlist}"\n' + record + r'''
typedef struct {sys_dlist_t failure_records_list;} wifi_mgr_auto_conn_obj_t;
#define MAX_AP_FAILURE_COUNT 3
#define AP_FAILURE_COOLDOWN_MS 60000U
#define WIFI_MGR_CALLOC calloc
static int locked;
#define WIFI_MGR_MUTEX_LOCK() do {assert(!locked);locked=1;} while(0)
#define WIFI_MGR_MUTEX_UNLOCK() do {assert(locked);locked=0;} while(0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static uint32_t now;
static uint64_t lisa_os_get_tick_ms(void) {return now;}
''' + ''.join(support.c_function(source, n) for n in ('is_ap_blacklisted', 'increment_ap_failure_count', 'set_ap_failure_count_to_max', 'reset_ap_failure_count')) + r'''
int main(void) {
 wifi_mgr_auto_conn_obj_t obj={0};sys_dlist_init(&obj.failure_records_list);
 now=UINT32_MAX-30000U;
 for(int i=0;i<3;++i) increment_ap_failure_count(&obj,"hotspot","01");
 assert(is_ap_blacklisted(&obj,"hotspot","01"));
 now+=59999;assert(is_ap_blacklisted(&obj,"hotspot","01"));
 now++;assert(!is_ap_blacklisted(&obj,"hotspot","01"));
 increment_ap_failure_count(&obj,"hotspot","01");
 assert(!is_ap_blacklisted(&obj,"hotspot","01"));
 set_ap_failure_count_to_max(&obj,"hotspot","01");
 now+=120000;assert(is_ap_blacklisted(&obj,"hotspot","02"));
 reset_ap_failure_count(&obj,"hotspot","02");
 assert(!is_ap_blacklisted(&obj,"hotspot","02"));
 return 0;
}
''')

    def test_repeated_disconnect_does_not_restart_grace(self):
        source = (support.ROOT/'src/system/sys_network_manager.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define SYS_NETWORK_BEARER_WIFI 1
#define SYS_NETWORK_BEARER_NONE 0
#define VOICE_MSG_SYSTEM_NETWORK_DISCONNECTED 2
#define WIFI_DISCONNECT_GRACE_MS 8000U
#define sys_network_diag_mark(...) ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
typedef int sys_network_bearer_t;
static struct {int active_bearer;bool switching,network_connected,wifi_disconnect_pending;unsigned wifi_disconnect_seq,generation;void *wifi_disconnect_sem;} s_mgr;
static unsigned signals,events;
static bool suspended;
static bool sys_wifi_radio_suspended(void) {return suspended;}
static bool sys_wifi_is_connected(void) {return false;}
static void sys_network_sync_app_data(void) {}
static void lisa_semaphore_give(void *s) {signals++;}
static uint32_t sys_network_commit_bearer_locked(int bearer) {s_mgr.active_bearer=bearer;return ++s_mgr.generation;}
static void sys_network_publish_bearer(int bearer,int event,uint32_t generation) {if(event)events++;}
''' + support.c_function(source, 'sys_network_wifi_disconnected') + r'''
int main(void) {
 s_mgr.active_bearer=1;s_mgr.network_connected=true;
 sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 assert(!s_mgr.network_connected && s_mgr.wifi_disconnect_pending && signals==1);
 unsigned first=s_mgr.wifi_disconnect_seq;
 for(int i=0;i<20;++i) sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 assert(s_mgr.wifi_disconnect_seq==first && signals==1 && !events);
 s_mgr.switching=true;sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 assert(!s_mgr.wifi_disconnect_pending && s_mgr.active_bearer==0 && !events);
 return 0;
}
''')

    def test_l2_without_ip_forces_real_reassociation(self):
        source = (support.ROOT/'src/system/sys_wifi.c').read_text(encoding='utf-8')
        policy = (support.ROOT/'src/system/sys_wifi_reconnect_policy.h').as_posix()
        policy_c = (support.ROOT/'src/system/sys_wifi_reconnect_policy.c').read_text(encoding='utf-8').replace('#include "sys_wifi_reconnect_policy.h"', '')
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
''' + f'#include "{policy}"\n' + policy_c + r'''
#define WIFI_MGR_STA_CONNECTED 2
#define SYS_WIFI_STALE_LINK_RELEASE_TIMEOUT_MS 1000U
#define SYS_WIFI_STALE_LINK_POLL_MS 20U
#define sys_network_diag_mark(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
typedef struct {char ssid[32];} wifi_mgr_sta_config_t;
static struct {bool ready,started,connected,saved_scan_absent;} s_wifi;
static bool suspended,saved=true;
static int status,disconnects,connects,scans;
static bool sys_wifi_radio_suspended(void) {return suspended;}
static bool sys_wifi_has_ap(void) {return saved;}
static int wifi_mgr_sta_get_status(void) {return status;}
static void sys_wifi_stop_autoconnect(void) {}
static int wifi_mgr_sta_get_connected_info(wifi_mgr_sta_config_t *ap) {strcpy(ap->ssid,"test");return 0;}
static int wifi_sta_disconnect(void) {disconnects++;status=0;return 0;}
static void lisa_thread_mdelay(unsigned ms) {}
static void sys_wifi_clear_ip_addr(void) {}
static void sys_wifi_reset_connect_result(void) {}
static int wifi_mgr_sta_connect(wifi_mgr_sta_config_t *ap,bool async) {connects++;return 0;}
static int sys_wifi_start_autoconnect(void) {scans++;return 0;}
''' + support.c_function(source, 'sys_wifi_reconnect_saved_once_start') + r'''
int main(void) {
 s_wifi.ready=s_wifi.started=true;status=2; /* L2 yes, DHCP never completed. */
 assert(sys_wifi_reconnect_saved_once_start()==0);
 assert(disconnects==1 && connects==1 && scans==0);
 assert(sys_wifi_reconnect_saved_once_start()==0 && scans==1);
 suspended=true;assert(sys_wifi_reconnect_saved_once_start()==-EAGAIN);
 assert(connects==1 && scans==1);
 return 0;
}
''')

    def test_dhcp_deadline_and_recovery_ownership(self):
        source = (support.ROOT/'src/system/sys_wifi.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define WIFI_MGR_STA_CONNECTED 2
#define SYS_WIFI_CONNECT_RESULT_FAIL_IP 3
#define SYS_WIFI_DHCP_TIMEOUT_MS 15000U
static struct {bool ready,started,connected,dhcp_pending;uint32_t dhcp_started_ms;int connect_result;} s_wifi;
static bool suspended;static int status=2;static uint32_t now;
static bool sys_wifi_radio_suspended(void) {return suspended;}
static int wifi_mgr_sta_get_status(void) {return status;}
static uint64_t lisa_os_get_tick_ms(void) {return now;}
''' + support.c_function(source, 'sys_wifi_ip_recovery_needed') + r'''
int main(void) {
 s_wifi.ready=s_wifi.started=s_wifi.dhcp_pending=true;
 s_wifi.dhcp_started_ms=UINT32_MAX-10000U;now=s_wifi.dhcp_started_ms+14999U;
 assert(!sys_wifi_ip_recovery_needed());now++;assert(sys_wifi_ip_recovery_needed());
 s_wifi.connected=true;assert(!sys_wifi_ip_recovery_needed());
 s_wifi.connected=false;suspended=true;assert(!sys_wifi_ip_recovery_needed());
 suspended=false;status=0;assert(!sys_wifi_ip_recovery_needed());
 status=2;now=0;s_wifi.dhcp_started_ms=0;s_wifi.dhcp_pending=false;s_wifi.connect_result=3;
 assert(sys_wifi_ip_recovery_needed());
 s_wifi.started=false;assert(!sys_wifi_ip_recovery_needed());
 return 0;
}
''')

    def test_network_reminder_only_marks_an_accepted_tone(self):
        source = (support.ROOT/'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define sys_network_diag_mark(...) ((void)0)
#define LOGI(...) ((void)0)
static bool s_tone_playing,allowed=true,pending,accept;
static bool s_network_reminder_started,s_network_reminder_pending;
static uint32_t s_network_reminder_ms,now;
#define NETWORK_REMINDER_INTERVAL_MS 15000U
static int locked;
#define taskENTER_CRITICAL() do {assert(!locked);locked=1;} while(0)
#define taskEXIT_CRITICAL() do {assert(locked);locked=0;} while(0)
static uint64_t lisa_os_get_tick_ms(void) {assert(locked);return now;}
static unsigned queues;
static bool reenter,nested;
static bool voice_player_play_network_tone(uint32_t tone_id);
static const char *app_tone_get_url(uint32_t id) {return "tone";}
static bool voice_player_network_tone_allowed(const char *s) {return allowed;}
static bool voice_player_tone_url_is_active_or_queued(const char *s) {return pending;}
static bool voice_player_queue_priority_tone(const char *s) {
 assert(!locked);queues++;
 if(reenter){reenter=false;nested=voice_player_play_network_tone(65);}
 return accept;
}
''' + support.c_function(source, 'voice_player_play_network_tone') + r'''
int main(void) {
 reenter=true;assert(!voice_player_play_network_tone(60) && queues==1 && !nested);
 assert(!s_network_reminder_pending && !s_network_reminder_started);
 accept=true;reenter=true;assert(voice_player_play_network_tone(60) && queues==2 && !nested);
 pending=true;assert(voice_player_play_network_tone(65) && queues==2);
 allowed=false;assert(!voice_player_play_network_tone(65) && queues==2);
 allowed=true;pending=false;
 for(int i=0;i<100;i++) assert(voice_player_play_network_tone(64));
 assert(queues==2); /* Suppressed repeats must not fill the player's queue. */
 now=14999;assert(voice_player_play_network_tone(65) && queues==2);
 now=15000;assert(voice_player_play_network_tone(65) && queues==3);
 now=UINT32_MAX-7000;s_network_reminder_ms=now;
 now+=14999;assert(voice_player_play_network_tone(60) && queues==3);
 now++;assert(voice_player_play_network_tone(60) && queues==4);
 return 0;
}
''')
        self.assertIn('s_disconnect_tone_played = voice_player_play_network_tone(TONE_ID_60)', source)
        self.assertIn('s_disconnect_tone_played = voice_player_play_network_tone(TONE_ID_65)', source)

    def test_failed_radio_queries_release_their_buffer(self):
        source = (support.ROOT/'arcs-sdk/modules/wifi_manager/port/wifi_ops/arcs/wifi_arcs.c').read_text(encoding='utf-8')
        fn = source[source.index('wifi_mgr_wifi_status_t arcs_wifi_sta_get_status(void)'):]
        fn = fn[:fn.index('\n}\n')+2]
        self.run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
typedef int wifi_mgr_wifi_status_t;
typedef struct {int state;} wifi_link_status_t;
enum {STA_INACTIVE,STA_IN_CONNECTING,STA_CONNECTED,STA_DISCONNECTED};
enum {WIFI_MGR_WIFI_STATUS_STA_UNKNOWN,WIFI_MGR_WIFI_STATUS_STA_DISCONNECTED,WIFI_MGR_WIFI_STATUS_STA_CONNECTED};
#define LISA_LOGE(...) ((void)0)
#define LISA_LOGI(...) ((void)0)
static unsigned allocations,frees;
static void *alloc(unsigned n) {allocations++;return malloc(n);}
static void release(void *p) {frees++;free(p);}
#define PLATFORM_MEM_NOCACHE_MALLOC alloc
#define PLATFORM_MEM_FREE release
static int wifi_get_link_status(wifi_link_status_t *s) {return -1;}
''' + fn + r'''
int main(void) {
 for(int i=0;i<1000;i++) assert(arcs_wifi_sta_get_status()==WIFI_MGR_WIFI_STATUS_STA_UNKNOWN);
 assert(allocations==frees);return 0;
}
''')

    def test_only_current_abnormal_session_exit_invalidates_network(self):
        source = (support.ROOT/'src/server/lingxin_server/lingxin_server.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define EXIT_REASON_WEBSOCKET_DISCONNECT 2
#define EXIT_REASON_WEBSOCKET_CONNECTION_FAILED 3
#define LOGW(...) ((void)0)
static bool s_lingxin_chat_running;
static uint32_t s_lingxin_offline_owner_generation=1,current=1;
static uint32_t s_lingxin_requested_owner_generation;
static char s_lingxin_requested_task_id[160];
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static unsigned failures;
static bool service_offline_action_online_current(uint32_t gen) {return gen && gen==current;}
static void sys_network_report_transport_failure(void) {failures++;}
''' + support.c_function(source,'lingxin_report_transport_exit') + r'''
int main(void) {
 s_lingxin_chat_running=true;
 lingxin_report_transport_exit(0);lingxin_report_transport_exit(4);assert(!failures);
 current=2;lingxin_report_transport_exit(2);assert(!failures);
 current=1;lingxin_report_transport_exit(2);assert(failures==1);
 /* Handshake succeeded but task_started has not arrived yet. */
 s_lingxin_offline_owner_generation=0;s_lingxin_requested_owner_generation=1;
 lingxin_report_transport_exit(3);assert(failures==1); /* No accepted request. */
 s_lingxin_requested_task_id[0]='x';
 lingxin_report_transport_exit(3);assert(failures==2);
 current=2;lingxin_report_transport_exit(3);assert(failures==2);
 s_lingxin_chat_running=false;lingxin_report_transport_exit(3);assert(failures==2);
 return 0;
}
''')
        listener = support.c_function(source,'lingxin_chat_life_cycle_listener')
        self.assertLess(listener.index('lingxin_report_transport_exit(exit_code)'), listener.index('lingxin_publish_chat_finished()'))

    def test_real_idle_transition_preserves_exit_reason_before_cleanup(self):
        source = (support.ROOT/'third_party/AI2T_LingXinEngine/src/voice_chat/chat_state_machine.c').read_text(encoding='utf-8').replace('static void cleanup_on_enter_idle()', 'static void cleanup_on_enter_idle(void)')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef int ChatState;typedef int StateEvent;typedef int ExitCode;
enum {State_Idle,State_Welcome,State_Upload_Init,State_NoVoice_Start,State_Upload_Transfer,
State_NoVoice_Terminate,State_Terminate,State_Exit,State_Task_Complete,State_Download_Init,State_Download_End};
enum {EXIT_REASON_USER_INITIATED,EXIT_REASON_WEBSOCKET_DISCONNECT,EXIT_REASON_WEBSOCKET_CONNECTION_FAILED};
enum {CHAT_LIFE_CYCLE_EVENT_EXIT,CHAT_LIFE_CYCLE_EVENT_CHAT_PHASE_CHANGE};
typedef struct {int unused;} InnerStateCollection;
typedef struct {int phase_code;} ChatPhaseChangePayload;
typedef struct {int exit_code;} ExitPayload;
typedef struct {int exit_code,upload_type;} Context;
static Context old_ctx,new_ctx,*current;
static int chat_current_state,chat_last_state,module_timeout_need_callback;
static int lingxin_config_get_state_mutex,locked,cleaned,received,exit_events;
static bool reenter;
static Context *get_current_context(void) {return current;}
static void destroy_chat_runtime_context(void) {assert(locked);current=NULL;cleaned++;}
static void delete_lingxin_chat_timer(void) {}
static void lingxin_websocket_control_del(void) {}
static void lingxin_mutex_lock(int m) {assert(!locked);locked=1;}
static void lingxin_mutex_unlock(int m) {assert(locked);locked=0;}
static bool validate_state_transition(int s,int e) {return true;}
static void clear_all_inner_state(void) {}
static void set_preset_inner_state(InnerStateCollection *p) {}
static void updateChatState(int s,int e,ChatPhaseChangePayload *p) {chat_last_state=chat_current_state;chat_current_state=s;}
static void lingxin_emit_chat_event(int event,void *payload) {
 assert(!locked && cleaned && current!=&old_ctx);
 if(event==CHAT_LIFE_CYCLE_EVENT_EXIT) {received=((ExitPayload*)payload)->exit_code;exit_events++;}
 else if(reenter) {new_ctx.exit_code=0;current=&new_ctx;}
}
#define lingxin_trace_transition(...) ((void)0)
#define lingxin_trace_set(...) ((void)0)
#define lingxin_log_debug(...) ((void)0)
#define module_local_play_welcome_audio(...) ((void)0)
#define reset_cloud_vad_round_state(...) ((void)0)
#define upload_manager_init(...) ((void)0)
#define noVoiceTerminateEndCallback(...) ((void)0)
#define update_current_context(...) ((void)0)
#define upload_manager_start(...) ((void)0)
#define turn_to_upload_transfer(...) ((void)0)
#define upload_manager_terminate(...) ((void)0)
#define playback_manager_terminate(...) ((void)0)
#define turn_to_exit(...) ((void)0)
#define turn_to_task_complete(...) ((void)0)
#define turn_to_download_init(...) ((void)0)
#define turn_to_download_end(...) ((void)0)
''' + ''.join(support.c_function(source,n) for n in ('emit_exit_code','cleanup_on_enter_idle','execute_state_entry_action','turn_to_with_preset_inner_state')) + r'''
int main(void) {
 for(int code=0;code<=2;code++) {
  old_ctx.exit_code=code;current=&old_ctx;chat_current_state=State_Exit;
  int before=exit_events;reenter=true;
  turn_to_with_preset_inner_state(State_Idle,0,NULL);
  assert(received==code && exit_events==before+1 && !locked);
 }
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
