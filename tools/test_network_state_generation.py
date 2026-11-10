"""Execute real bearer commits against DHCP/grace/probe interleavings."""
import re
import unittest

from tools import test_offline_action as support


class NetworkStateGenerationTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def manager_source(self):
        source = (support.ROOT / "src/system/sys_network_manager.c").read_text(encoding="utf-8")
        header = (support.ROOT / "src/system/sys_network_manager.h").read_text(encoding="utf-8")
        context = re.search(r"typedef struct \{.*?\} sys_network_manager_ctx_t;", source, re.S).group()
        names = (
            "sys_network_generation", "sys_network_generation_is_current",
            "sys_network_commit_bearer_locked", "sys_network_publish_bearer",
            "sys_network_set_bearer", "sys_network_wifi_ip_got",
            "sys_network_wifi_disconnected", "sys_network_wifi_disconnect_worker",
            "sys_network_report_probe_result_for_generation", "sys_network_report_probe_result",
        )
        return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#define VOICE_MSG_SYSTEM_NETWORK_CONNECTED 11U
#define VOICE_MSG_SYSTEM_NETWORK_DISCONNECTED 12U
#define WIFI_DISCONNECT_GRACE_MS 8000U
#define LISA_OS_WAIT_FOREVER 0
typedef void lisa_semaphore_t;
''' + header + context + r'''
static sys_network_manager_ctx_t s_mgr;
static bool radio_connected, suspended, injected, inject_confirmed;
static unsigned starts, published[16], generations[16], count, sem_takes, signals, syncs;
static unsigned depth;
static jmp_buf finished;
static void sys_network_wifi_ip_got(void*,uint32_t,void*,uint32_t,void*);
static void enter_critical(void) { depth++; }
static void exit_critical(void) { assert(depth);depth--; }
#define taskENTER_CRITICAL() enter_critical()
#define taskEXIT_CRITICAL() exit_critical()
static bool sys_wifi_is_connected(void) { return radio_connected && !suspended; }
static bool sys_wifi_radio_suspended(void) { return suspended; }
static void sys_network_sync_app_data(void) { syncs++; }
static int voice_msg_pub(unsigned event,void*p,unsigned n) {
 assert(!depth && count<16);published[count]=event;
 generations[count++]=p && n==sizeof(uint32_t) ? *(uint32_t*)p : 0;
 return 0;
}
static int network_probe_start(void) { assert(!depth);starts++;return 0; }
static void lisa_thread_mdelay(unsigned ms) { assert(!depth && ms==8000); }
static int lisa_semaphore_take(void*s,unsigned timeout) {
 assert(!depth);if (++sem_takes>1) longjmp(finished,1);return 0;
}
static int lisa_semaphore_give(void*s) { assert(!depth);signals++;return 0; }
static void sys_network_diag_mark(const char*d,const char*event,int v,const char*detail) {
 assert(!depth);
 if (inject_confirmed && !injected && !strcmp(event,"wifi_disconnect_confirmed")) {
  injected=true;radio_connected=true;
  sys_network_wifi_ip_got(NULL,0,NULL,0,NULL);
 }
}
''' + "\n".join(support.c_function(source, name) for name in names
                     if re.search(r"\b" + name + r"\(", source))

    def test_dhcp_after_grace_check_cannot_be_overwritten_or_emit_stale_disconnect(self):
        self.run_c(self.manager_source() + r'''
int main(void) {
 s_mgr.inited=true;s_mgr.active_bearer=1;
 s_mgr.wifi_disconnect_pending=true;s_mgr.wifi_disconnect_seq=3;
 inject_confirmed=true;
 if (!setjmp(finished)) sys_network_wifi_disconnect_worker(NULL);
 assert(injected && radio_connected && s_mgr.wifi_disconnect_seq==4);
 assert(s_mgr.active_bearer==1 && !s_mgr.wifi_disconnect_pending);
 assert(count==1 && published[0]==11 && starts==1 && !depth);
 return 0;
}
''')

    def test_delayed_probe_result_cannot_change_new_association_or_outage(self):
        source = self.manager_source()
        self.assertTrue("bool sys_network_report_probe_result_for_generation(" in source)
        self.run_c(source + r'''
int main(void) {
 s_mgr.inited=true;radio_connected=true;
 sys_network_wifi_ip_got(NULL,0,NULL,0,NULL);
 uint32_t old=sys_network_generation();
 assert(sys_network_report_probe_result_for_generation(true,old) && s_mgr.network_connected);
 radio_connected=false;sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 assert(!s_mgr.network_connected && !sys_network_generation_is_current(old));
 assert(!sys_network_report_probe_result_for_generation(true,old) && !s_mgr.network_connected);
 radio_connected=true;sys_network_wifi_ip_got(NULL,0,NULL,0,NULL);
 uint32_t current=sys_network_generation();
 assert(sys_network_report_probe_result_for_generation(true,current) && s_mgr.network_connected);
 assert(!sys_network_report_probe_result_for_generation(false,old) && s_mgr.network_connected);
 sys_network_report_probe_result(false);assert(!s_mgr.network_connected);
 return 0;
}
''')

    def test_immediate_probe_during_mode_switch_is_retried_after_switch_done(self):
        manager = (support.ROOT / "src/system/sys_network_manager.c").read_text(encoding="utf-8")
        source = self.manager_source().replace(
            "static int network_probe_start(void) { assert(!depth);starts++;return 0; }",
            "static int network_probe_start(void) { assert(!depth);starts++;"
            "sys_network_report_probe_result_for_generation(true,sys_network_generation());return 0; }",
        )
        self.run_c(source + r'''
#define CONFIG_LISA_MODEM 0
#define KV_KEY_NETWORK_MODE 0
#define VOICE_MSG_SYSTEM_NETWORK_SWITCHING 13U
#define VOICE_MSG_SYSTEM_NETWORK_SWITCH_DONE 14U
#define VOICE_MSG_SYSTEM_NETWORK_SWITCH_FAIL 15U
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
static bool start_fails;
static int sys_wifi_start(bool connect) { assert(!depth);return start_fails ? -1 : 0; }
static int sys_network_start_modem(void) { assert(!depth);s_mgr.modem_started=true;return 0; }
static int lisa_kv_set_int(int key,int value) { assert(!depth);return 0; }
static void voice_cloud_disconnect(void) { assert(!depth); }
static void sys_network_stop_bearer(sys_network_bearer_t bearer) {
 assert(!depth);if(s_mgr.active_bearer==bearer)sys_network_set_bearer(0,0);
}
''' + support.c_function(manager, "sys_network_start_bearer")
           + support.c_function(manager, "sys_network_apply_mode") + r'''
int main(void) {
 s_mgr.inited=true;s_mgr.wifi_available=true;radio_connected=true;
 sys_network_apply_mode(SYS_NETWORK_MODE_WIFI,false);
 assert(!s_mgr.switching && s_mgr.active_bearer==SYS_NETWORK_BEARER_WIFI);
 assert(starts==2 && s_mgr.network_connected && !depth);
 /* An asynchronous station still waits for DHCP; failure must not probe. */
 s_mgr.active_bearer=0;s_mgr.network_connected=false;starts=count=0;radio_connected=false;
 sys_network_apply_mode(SYS_NETWORK_MODE_WIFI,false);
 assert(!starts && !s_mgr.network_connected && !s_mgr.switching);
 start_fails=true;sys_network_apply_mode(SYS_NETWORK_MODE_WIFI,false);
 assert(!starts && !s_mgr.network_connected && !s_mgr.switching);
 /* A ready modem has the same immediate-result race. */
 starts=count=0;sys_network_apply_mode(SYS_NETWORK_MODE_MODEM,false);
 assert(starts==2 && s_mgr.network_connected && !s_mgr.switching && !depth);
 return 0;
}
''')

    def test_grace_duplicates_sleep_and_switch_keep_existing_behavior(self):
        self.run_c(self.manager_source() + r'''
int main(void) {
 s_mgr.inited=true;radio_connected=true;sys_network_wifi_ip_got(NULL,0,NULL,0,NULL);
 count=starts=0;radio_connected=false;
 sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 unsigned first=s_mgr.wifi_disconnect_seq;
 for(unsigned i=0;i<20;i++)sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 assert(s_mgr.wifi_disconnect_pending && signals==1 && s_mgr.wifi_disconnect_seq==first && !count);
 if (!setjmp(finished))sys_network_wifi_disconnect_worker(NULL);
 assert(s_mgr.active_bearer==0 && !s_mgr.wifi_disconnect_pending && count==1 && published[0]==12);
 radio_connected=true;sys_network_wifi_ip_got(NULL,0,NULL,0,NULL);count=0;
 suspended=true;radio_connected=false;sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 assert(s_mgr.active_bearer==0 && !count && !s_mgr.wifi_disconnect_pending);
 suspended=false;radio_connected=true;sys_network_wifi_ip_got(NULL,0,NULL,0,NULL);count=0;
 s_mgr.switching=true;radio_connected=false;sys_network_wifi_disconnected(NULL,0,NULL,0,NULL);
 assert(s_mgr.active_bearer==0 && !count && !s_mgr.wifi_disconnect_pending && !depth);
 return 0;
}
''')

    def test_audio_consumer_rejects_stale_generation_and_legacy_wrong_state(self):
        source = (support.ROOT / "apps/arcs-mini/voice_player.c").read_text(encoding="utf-8")
        self.assertTrue("static bool voice_player_network_event_current(" in source)
        header = (support.ROOT / "src/system/sys_network_manager.h").read_text(encoding="utf-8")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define VOICE_MSG_SYSTEM_NETWORK_CONNECTED 11U
#define VOICE_MSG_SYSTEM_NETWORK_DISCONNECTED 12U
''' + header + r'''
static uint32_t generation=5;
bool sys_network_generation_is_current(uint32_t g) { return g==generation; }
''' + support.c_function(source,"voice_player_network_event_current") + r'''
int main(void) {
 sys_network_status_t status={.active_bearer=SYS_NETWORK_BEARER_WIFI,.wifi_connected=true};
 uint32_t old=4,current=5;
 assert(!voice_player_network_event_current(12,&old,4,&status,true));
 assert(!voice_player_network_event_current(12,&current,2,&status,true));
 assert(!voice_player_network_event_current(12,NULL,4,&status,true));
 assert(!voice_player_network_event_current(12,&current,4,&status,true));
 assert(!voice_player_network_event_current(12,NULL,0,&status,true));
 assert(voice_player_network_event_current(11,&current,4,&status,true));
 assert(voice_player_network_event_current(11,NULL,0,&status,true));
 status.active_bearer=0;status.wifi_connected=false;
 assert(voice_player_network_event_current(12,&current,4,&status,true));
 assert(voice_player_network_event_current(12,NULL,0,&status,true));
 assert(!voice_player_network_event_current(11,NULL,0,&status,true));
 assert(!voice_player_network_event_current(12,NULL,0,&status,false));
 status.switching=true;assert(!voice_player_network_event_current(12,NULL,0,&status,true));
 return 0;
}
''')

    def test_probe_notification_validates_owner_generation_length_and_realtime_state(self):
        source = (support.ROOT / "src/system/sys_network_probe.c").read_text(encoding="utf-8")
        manager = (support.ROOT / "src/system/sys_network_manager.h").read_text(encoding="utf-8")
        probe = (support.ROOT / "src/system/sys_network_probe.h").read_text(encoding="utf-8")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
''' + manager + probe + r'''
static uint32_t owner=2,generation=5;
static sys_network_status_t status={.active_bearer=SYS_NETWORK_BEARER_WIFI,.wifi_connected=true,.connected=true};
static uint32_t service_offline_action_network_epoch(void){return owner;}
bool sys_network_generation_is_current(uint32_t g){return generation==g;}
int sys_network_get_status(sys_network_status_t *s){*s=status;return 0;}
''' + support.c_function(source,"network_probe_event_current") + r'''
int main(void){
 sys_network_probe_event_t event={2,5};
 char unaligned[sizeof(event)+1];memcpy(unaligned+1,&event,sizeof(event));
 assert(network_probe_event_current(unaligned+1,sizeof(event),true));
 assert(network_probe_event_current(&owner,sizeof(owner),true));
 assert(network_probe_event_current(NULL,0,true));
 assert(!network_probe_event_current(NULL,4,true));
 assert(!network_probe_event_current(&event,0,true));
 for(unsigned n=1;n<=12;n++)if(n!=4 && n!=8)assert(!network_probe_event_current(&event,n,true));
 generation++;assert(!network_probe_event_current(&event,sizeof(event),true));
 event.generation=generation;owner++;assert(!network_probe_event_current(&event,sizeof(event),true));
 event.owner=owner;status.connected=false;
 assert(!network_probe_event_current(&event,sizeof(event),true));
 assert(network_probe_event_current(&event,sizeof(event),false));
 status.switching=true;assert(!network_probe_event_current(&event,sizeof(event),false));
 return 0;
}
''')

    def test_platform_stale_events_and_failed_ota_dispatch_do_not_block_cloud(self):
        source = (support.ROOT / "src/category/comm/msgs/voice_platform.c").read_text(encoding="utf-8")
        header = (support.ROOT / "src/system/sys_network_manager.h").read_text(encoding="utf-8")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_OTA 1
#define CONFIG_VOICE_LINK_LINGXIN 1
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
''' + header + r'''
static bool current, s_ota_checked_once, ota_busy;
static unsigned ota_calls,cloud_calls;
static int ota_result;
struct app_datas {int unused;};
static struct app_datas app;
static struct app_datas*get_app_datas(void){return &app;}
static bool network_probe_event_current(const void*d,uint32_t n,bool ok){return current;}
int sys_network_get_status(sys_network_status_t*s){*s=(sys_network_status_t){.active_bearer=1,.connected=true};return 0;}
static int ota_manager_check_all(void){ota_calls++;return ota_result;}
static bool voice_ota_is_active(void){return ota_busy;}
static void do_lingxin_cloud_connect(void){cloud_calls++;}
''' + support.c_function(source,"voice_system_network_probe_success") + r'''
int main(void){
 voice_system_network_probe_success(NULL,0,NULL,0,NULL);
 assert(!ota_calls && !cloud_calls && !s_ota_checked_once);
 current=true;ota_result=-1;
 voice_system_network_probe_success(NULL,0,NULL,0,NULL);
 assert(ota_calls==1 && cloud_calls==1 && !s_ota_checked_once);
 ota_result=0;voice_system_network_probe_success(NULL,0,NULL,0,NULL);
 assert(ota_calls==2 && cloud_calls==1 && s_ota_checked_once);
 ota_busy=true;voice_system_network_probe_success(NULL,0,NULL,0,NULL);
 assert(ota_calls==2 && cloud_calls==1);
 ota_busy=false;voice_system_network_probe_success(NULL,0,NULL,0,NULL);
 assert(ota_calls==2 && cloud_calls==2);
 return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
