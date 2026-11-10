"""Execute production offline policy on the RISC-V simulator."""
import subprocess
import tempfile
import unittest
import sys
import re
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN
from tools.test_servo_output_gate import function


def c_function(source, name):
    match = re.search(r'^(?:static )?(?:bool|int|void|uint32_t) ' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


class OfflineActionTest(unittest.TestCase):
    def test_goodbye_owner_binds_only_matching_started_task(self):
        server = (ROOT/'src/server/lingxin_server/lingxin_server.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static uint32_t s_lingxin_offline_owner_generation,s_lingxin_requested_owner_generation;
static char s_lingxin_requested_task_id[160];
''' + c_function(server, 'lingxin_server_task_started') + r'''
int main(void) {
 s_lingxin_requested_owner_generation=8;
 lingxin_server_task_started("old-task");assert(s_lingxin_offline_owner_generation==0);
 strcpy(s_lingxin_requested_task_id,"new-task");
 lingxin_server_task_started("old-task");assert(s_lingxin_offline_owner_generation==0);
 lingxin_server_task_started("new-task");assert(s_lingxin_offline_owner_generation==8);
 lingxin_server_task_started("old-task");assert(s_lingxin_offline_owner_generation==8);
 return 0;
}
''')
        start = c_function(server, 'lingxin_server_chat_start_internal')
        self.assertLess(start.index('s_lingxin_offline_owner_generation = 0'), start.index('lingxin_server_init()'))
        self.assertNotIn('s_lingxin_offline_owner_generation = service_offline_action_generation()', start)

    def test_provisioning_control_across_modes_and_rapid_switches(self):
        self.run_c(r"""
#include <assert.h>
#include "service_offline_action_policy.h"
int main(void) {
 offline_policy_t p={0};
 for (unsigned i=0;i<10000;++i) {
  offline_begin(&p,false,i*10);
  unsigned old=p.generation;
  assert(offline_enqueue(&p,old,OFFLINE_CMD_NETCFG,i*10+1));
  assert(p.netcfg_requested && p.generation!=old && !p.count);
  assert(!offline_enqueue(&p,old,OFFLINE_CMD_ENTER_OFFLINE,i*10+2));
  assert(offline_enqueue(&p,p.generation,OFFLINE_CMD_ENTER_OFFLINE,i*10+3));
  assert(!p.netcfg_requested && p.local_request==OFFLINE_CMD_ENTER_OFFLINE);
  assert(offline_enqueue(&p,p.generation,OFFLINE_CMD_NETCFG,i*10+4));
  assert(p.netcfg_requested && !p.local_request);
  assert(offline_enqueue(&p,p.generation,OFFLINE_CMD_ENTER_ONLINE,i*10+5));
  assert(!p.netcfg_requested && p.local_request==OFFLINE_CMD_ENTER_ONLINE);
 }
 return 0;
}
""")

    def test_mode_survives_window_cancel_but_not_policy_initialization(self):
        source=(ROOT/'apps/arcs-mini/services/service_offline_action_policy.c').read_text(encoding='utf-8')
        fallback='' if 'void offline_policy_init(' in source else '#define offline_policy_init(p) offline_cancel(p)\n'
        self.run_c(fallback+r'''
#include <assert.h>
#include "service_offline_action_policy.h"
int main(void) {
 offline_policy_t p={0};
 offline_policy_init(&p);assert(!p.forced_offline && p.state==OFFLINE_IDLE);
 p.forced_offline=true;
 offline_begin(&p,true,0);offline_prompt_finished(&p,p.generation,true,1);
 offline_poll(&p,201,false);offline_poll(&p,30201,false);
 assert(p.state==OFFLINE_IDLE && p.forced_offline);
 offline_policy_init(&p);assert(!p.forced_offline && p.state==OFFLINE_IDLE);
 offline_begin(&p,true,0);offline_prompt_finished(&p,p.generation,true,1);offline_poll(&p,201,false);
 assert(offline_enqueue(&p,p.generation,3,202));
 unsigned old=p.generation;
 assert(offline_enqueue(&p,old,261,203));
 assert(p.count==0 && p.control_count==0 && p.local_request==261 && p.generation!=old);
 unsigned pending=p.generation;
 assert(offline_enqueue(&p,pending,261,204) && p.generation==pending);
 assert(offline_enqueue(&p,pending,262,205) && p.generation!=pending && p.local_request==262);
 p.local_request=0;p.last_mode_command=262;p.state=OFFLINE_ONLINE;
 unsigned retry=p.generation;
 assert(offline_enqueue(&p,retry,262,2000) && p.generation!=retry && p.local_request==262);
 return 0;
}
''')

    def test_runtime_mode_control_is_available_online_and_invalidates_old_owner(self):
        # A mode control must be reachable with a healthy cloud owner, while
        # ordinary physical actions remain cloud-only.
        self.run_c(r'''
#include <assert.h>
#include "service_offline_action_policy.h"
int main(void) {
 offline_policy_t p={0};
 assert(offline_map("jin4 ru4 li2 xian4 mo2 shi4")==261);
 assert(offline_map("jin4 ru4 lian2 wang3 mo2 shi4")==262);
 assert(offline_map("cha2 xun2 xi4 tong3 ban3 ben3")==263);
 offline_begin(&p,false,0);
 unsigned old=p.generation;
 assert(!offline_enqueue(&p,old,3,1));
 assert(offline_enqueue(&p,old,261,2));
 assert(p.generation!=old);
 assert(!offline_enqueue(&p,old,262,3));
 return 0;
}
''')

    def test_full_export_catalog_maps_and_queues_without_internal_actions(self):
        # 2026-09-23 export semantics must reach the action queue; a retired
        # single-flip mapping must not steal the renamed frontflip command.
        self.run_c(r'''
#include <assert.h>
#include "service_offline_action_policy.h"
int main(void) {
 const struct {const char *intent; int command;} cases[] = {
  {"qian2 jin4",3}, {"hou4 tui4",4}, {"zuo3 zhuan3",7}, {"you4 zhuan3",8},
  {"wang3 qian2 pao3",5}, {"wang3 hou4 pao3",6}, {"zhan4 qi3 lai2",1},
  {"pa1 xia4 lai2",2}, {"dao4 li4",SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
  {"fan1 gen1 tou4",OFFLINE_CMD_FLIP_CONTINUOUS},
  {"hou4 kong1 fan1",SERVICE_V2_GAIT_ACTION_BACKFLIP},
  {"chang4 ge1",SERVICE_V2_GAIT_ACTION_MUSIC_BEAT},
  {"wang3 qiu2 fa1 qiu2",12}, {"wang3 qiu2 zheng4 shou3",13},
  {"wang3 qiu2 dan1 fan3",14}, {"wang3 qiu2 shuang1 fan3",15},
  {"wo4 shou3",17}, {"da3 zhao1 hu1",18}, {"zhuan4 quan1 quan1",20},
  {"zuo4 xia4 lai2",21}, {"qin1 qin1 wo3",22}, {"tiao4 wu3",23},
  {"tiao4 yue4",24}, {"niu3 pi4 gu3",25}, {"nao2 yang3 yang3",19}
 };
 offline_policy_t p={0};
 offline_begin(&p,true,0);offline_prompt_finished(&p,p.generation,true,1);
 offline_poll(&p,201,false);
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
  assert(offline_map(cases[i].intent)==cases[i].command);
  assert(offline_enqueue(&p,p.generation,cases[i].command,202+i));
  assert(offline_take(&p,p.generation)==cases[i].command);
 }
 assert(offline_map("ting2 zhi3 yi2 dong4")==0);
 assert(offline_map("xiu1 xi1")==259);
 assert(offline_map("jin4 ru4 pei4 wang3 mo2 shi4")==256);
 assert(offline_map("zeng1 da4 yin1 liang4")==257);
 assert(offline_map("jian3 xiao3 yin1 liang4")==258);
 assert(offline_map("jin4 ru4 li2 xian4 mo2 shi4")==OFFLINE_CMD_ENTER_OFFLINE);
 assert(offline_map("jin4 ru4 lian2 wang3 mo2 shi4")==OFFLINE_CMD_ENTER_ONLINE);
 assert(offline_map("cha2 xun2 xi4 tong3 ban3 ben3")==OFFLINE_CMD_VERSION);
 assert(offline_map("qian2 jin4 extra")==-1);
 assert(offline_map("DOG_DANCE")==-1);
 assert(!offline_enqueue(&p,p.generation,9,300)); /* internal rise pose */
 assert(!offline_enqueue(&p,p.generation,26,300)); /* IMU-only wiggle */
 assert(!offline_enqueue(&p,p.generation,27,300)); /* raw output-off */
 assert(!offline_enqueue(&p,p.generation,28,300)); /* assembly */
 assert(!offline_enqueue(&p,p.generation,29,300)); /* sleep fold */
 return 0;
}
''')

    def test_stop_and_goodbye_outrank_full_queues_and_invalidate_old_generation(self):
        self.run_c(r'''
#include <assert.h>
#include "service_offline_action_policy.h"
int main(void) {
 offline_policy_t p={0};
 offline_begin(&p,true,0);offline_prompt_finished(&p,p.generation,true,1);
 offline_poll(&p,201,false);
 unsigned old=p.generation;
 for(int i=0;i<3;++i) assert(offline_enqueue(&p,old,13,202));
 assert(offline_enqueue(&p,old,257,202));
 assert(offline_enqueue(&p,old,0,203));
 assert(p.state==OFFLINE_ACTIVE && p.count==0 && p.control_count==0);
 assert(p.generation!=old && !offline_enqueue(&p,old,13,204));
 assert(offline_enqueue(&p,p.generation,13,205));
 old=p.generation;
 assert(offline_enqueue(&p,old,259,206));
 assert(p.state==OFFLINE_IDLE && p.count==0 && p.control_count==0);
 assert(p.generation!=old && !offline_enqueue(&p,p.generation,13,207));
 offline_begin(&p,false,208);
 assert(!offline_enqueue(&p,p.generation,0,209));
 assert(!offline_enqueue(&p,p.generation,259,209));
 return 0;
}
''')

    def test_volume_adjustment_clamps_and_survives_restore(self):
        source = (ROOT/'apps/arcs-mini/services/service_volume.c').read_text(encoding='utf-8')
        production = re.sub(r'^#include[^\n]*', '', source, flags=re.M)
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_VOICE_PLAYER_DEFAULT_VOLUME 70
#define KV_KEY_USER_VOLUME 1
#define KV_KEY_USER_MIN_VOLUME 2
#define LOGI(...) ((void)0)
static int live=95,saved=95,floor_value=10,writes;
static int lisa_kv_get_int(int key,int *v) {*v=key==1?saved:floor_value;return 0;}
static int lisa_kv_set_int(int key,int v) {assert(key==1);saved=v;++writes;return 0;}
static int voice_player_get_system_volume(void) {return live;}
static int voice_player_set_system_volume(int v) {live=v;return 0;}
''' + production + r'''
int main(void) {
 service_volume_adjust(10);assert(live==100 && saved==100);
 service_volume_adjust(10);assert(live==100);
 live=15;service_volume_adjust(-10);assert(live==10 && saved==10);
 service_volume_adjust(-10);assert(live==10);
 live=70;service_volume_restore_from_kv();assert(live==10 && writes==4);
 floor_value=0;service_volume_adjust(-10);assert(live==0 && saved==0);
 service_volume_adjust(10);assert(live==10 && saved==10);
 return 0;
}
'''
        self.run_c(code)

    def test_shared_manual_provisioning_promotes_auto_without_clearing_credentials(self):
        source = (ROOT/'apps/arcs-mini/main.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define CONFIG_VOICE_LINK_LINGXIN 1
#define APP_NETCFG_PORTAL_REASON_USER_REQUEST 1
#define TONE_ID_65 65
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
static bool s_user_forced_netcfg,active;
static int starts,advertise,tones,errors,cloud_stops,ret,requests;
static void service_offline_action_request_netcfg(void) {++requests;}
static bool app_netcfg_portal_is_active(void) {return active;}
static int app_netcfg_portal_start(int reason) {
 assert(reason==1);++starts;
 return ret ? ret : (active ? -EALREADY : 0);
}
static void app_ble_netcfg_adv_start_delayed(void) {++advertise;}
static void app_play_netcfg_enter_prompt(void) {++tones;}
static void service_alarm_deinit(void) {}
static int lingxin_server_chat_stop(void) {++cloud_stops;return 0;}
static int app_tone_get_url(int id) {return id;}
static void voice_player_play_prompt_tone_url(int id) {assert(id==65);++errors;}
''' + c_function(source,'app_enter_manual_netcfg') + c_function(source,'app_enter_web_netcfg_by_button') + r'''
int main(void) {
 assert(app_enter_manual_netcfg()==0 && s_user_forced_netcfg);
 assert(starts==1 && advertise==1 && tones==1 && cloud_stops==1);
 active=true;s_user_forced_netcfg=false;
 app_enter_web_netcfg_by_button();
 assert(requests==1 && starts==1);
 assert(app_enter_manual_netcfg()==0);
 assert(starts==2 && s_user_forced_netcfg && advertise==2 && tones==2);
 ret=-EIO;
 assert(app_enter_manual_netcfg()==-EIO && advertise==2 && tones==2 && errors==0);
 assert(cloud_stops==1); /* Active portal errors must not restart unrelated services. */
 ret=0;
 assert(app_enter_manual_netcfg()==0 && advertise==3 && tones==3); /* repeated acknowledgement */
 active=false;ret=-EBUSY;
 assert(app_enter_manual_netcfg()==-EBUSY && advertise==3 && errors==0);
 ret=-EALREADY;
 assert(app_enter_manual_netcfg()==-EALREADY && advertise==3 && tones==3 && errors==1);
 return 0;
}
'''
        self.run_c(code)

    def test_rewake_before_cloud_worker_starts_preserves_sequence_owner_pair(self):
        source=(ROOT/'src/category/comm/msgs/voice_wakeup_msg.c').read_text(encoding='utf-8')
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static bool s_lingxin_starting,s_lingxin_wifi_recovering;
static uint32_t s_lingxin_wakeup_request_seq,s_lingxin_reserved_seq,s_lingxin_reserved_owner,s_lingxin_pending_owner;
'''+ '\n'.join(c_function(source,name) for name in [
            'lingxin_wakeup_request_register','lingxin_start_worker_reserved',
            'lingxin_start_worker_finish_or_take_pending'])+r'''
int main(void) {
 bool start,retrigger; uint32_t seq,owner;
 assert(lingxin_wakeup_request_register(false,&start,&retrigger,11)==1 && start);
 assert(lingxin_wakeup_request_register(false,&start,&retrigger,12)==2 && !start);
 lingxin_start_worker_reserved(&seq,&owner);
 assert(seq==1 && owner==11); /* Delayed old worker must not claim the new request. */
 assert(lingxin_start_worker_finish_or_take_pending(&seq,&owner));
 assert(seq==2 && owner==12);
 assert(!lingxin_start_worker_finish_or_take_pending(&seq,&owner) && !s_lingxin_starting);
 return 0;
}
'''
        self.run_c(code)

    def test_ble_takeover_preserves_connected_phone_and_standby_pause(self):
        source=(ROOT/'src/middleware/ble/app_ble_common.c').read_text(encoding='utf-8')
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LISA_LOGI(...) ((void)0)
typedef int gap_bdaddr_t;
static bool s_ble_connected,s_ble_offline_paused,s_ble_netcfg_connect_pending,s_ble_netcfg_adv_pending;
static bool s_ble_notify_running,s_ble_netcfg_paused,s_ble_netcfg_resume_adv,s_ble_netcfg_active;
static bool s_ble_transaction_busy;
static bool s_ble_subscribed,s_ble_sleep_disconnect;
static uint8_t s_ble_conidx,s_ble_sleep_disconnect_conidx;
static uint32_t s_ble_generation;
static int disconnects,advertise;
static void lisa_ble_adv_stop(int n) {}
static void lisa_ble_disconnect(int n,int why) {++disconnects;}
static void app_ble_netcfg_adv_schedule(bool n) {++advertise;}
'''+'\n'.join(c_function(source,name) for name in [
            'app_ble_netcfg_offline_blocked','app_ble_netcfg_offline_acquire',
            'app_ble_netcfg_offline_release','app_ble_netcfg_set_paused','app_ble_on_connected'])+r'''
int main(void) {
 s_ble_connected=true;
 assert(app_ble_netcfg_offline_acquire()==0 && disconnects==0 && !s_ble_netcfg_paused);
 s_ble_connected=false;s_ble_netcfg_adv_pending=true;
 assert(app_ble_netcfg_offline_acquire()==0 && !s_ble_netcfg_paused);
 s_ble_netcfg_adv_pending=false;s_ble_netcfg_active=true;
 assert(app_ble_netcfg_offline_acquire()==0 && !s_ble_netcfg_paused);
 app_ble_on_connected(1,1,0); /* connection already in flight wins */
 assert(!app_ble_netcfg_offline_blocked() && disconnects==0);
 app_ble_netcfg_offline_release();assert(!s_ble_netcfg_paused && advertise==0);
 s_ble_transaction_busy=true;
 assert(app_ble_netcfg_offline_blocked() && app_ble_netcfg_offline_acquire()==-EBUSY);
 s_ble_transaction_busy=false;
 s_ble_connected=false;
 assert(app_ble_netcfg_offline_acquire()==0);
 assert(app_ble_netcfg_set_paused(true)==0); /* sleep now owns the pause */
 app_ble_netcfg_offline_release();assert(s_ble_netcfg_paused);
 return 0;
}
'''
        self.run_c(code)

    def test_malformed_recognition_does_not_become_a_command(self):
        source = (ROOT/'src/server/lschat_server/wakeup/app_wakeup.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
''' + function(source, 'app_algo_command_extract') + r'''
int main(void) {
 uint8_t text[64]; bool keyword;
 /* ESR resolves the spoken alias to the new canonical frontflip intent. */
 assert(app_algo_command_extract((uint8_t*)"{\"rlt\":[{\"bMain\":false,\"intent\":\"fan1 gen1 tou4\",\"keyword\":\"fan1 ge4 gen1 tou2\"}]}",text,64,&keyword)==0);
 assert(!keyword && strcmp((char*)text,"fan1 gen1 tou4")==0);
 assert(app_algo_command_extract((uint8_t*)"{\"rlt\":[{\"bMain\":2,\"intentStr\":\"wang3 qiu2 dan1 fan3\"}]}",text,64,&keyword)!=0);
 assert(app_algo_command_extract((uint8_t*)"{\"rlt\":[{\"bMain\":0.5,\"intentStr\":\"wang3 qiu2 dan1 fan3\"}]}",text,64,&keyword)!=0);
 assert(app_algo_command_extract((uint8_t*)"{\"rlt\":[{\"bMain\":0,\"intentStr\":\"123456789\"}]}",text,5,&keyword)!=0);
 return 0;
}
'''
        cjson = ROOT/'arcs-sdk/modules/cjson'
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'parser.c', Path(temp)/'parser.elf'
            cfile.write_text(code, encoding='utf-8')
            build = subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(cjson),
                                    str(cfile), str(cjson/'cJSON.c'), '-lm', '-o', str(elf)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(RUN),str(elf)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    def test_worker_ownership_and_boundaries(self):
        source = ROOT / 'apps/arcs-mini/services/service_offline_action.c'
        self.assertTrue(source.exists(), 'worker ownership integration is missing')
        # Keep the entire production worker; replace only RTOS/device/network boundaries.
        production = re.sub(r'^#include[^\n]*', '', source.read_text(encoding='utf-8'), flags=re.M)
        code = r'''
#include <assert.h>
#include <string.h>
#include <errno.h>
#include "service_offline_action.h"
#include "service_offline_action_policy.h"
static unsigned critical_depth, netcfg_diag_calls;
static int netcfg_diag_value;
static const char *netcfg_diag_source;
#define taskENTER_CRITICAL() (++critical_depth)
#define taskEXIT_CRITICAL() (assert(critical_depth), --critical_depth)
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
#define portTICK_PERIOD_MS 1
#define LISA_LOGE(...) ((void)0)
#define LISA_LOGI(...) ((void)0)
#define ACOMP_ERR_OK 0
static void sys_network_diag_mark(const char *domain,const char *event,int value,const char *detail) {
 assert(critical_depth==0); /* Timeline logging may lock/format; ingress must have exited. */
 assert(!strcmp(domain,"offline") && !strcmp(event,"netcfg_accept"));
 ++netcfg_diag_calls;netcfg_diag_value=value;netcfg_diag_source=detail;
}
#define ACOMP_WAKEUP_ALGO_MODE_WAKEUP 0
#define VOICE_WORK_MODE_VOICE_WAKEUP 1
#define SERVICE_V2_VOICE_STANDBY_ACTIVE 0
#define VOICE_MSG_OFFLINE_ACTION_PROMPT 10
#define VOICE_MSG_WAKEUP_KEYWORD 11
#define VOICE_MSG_OFFLINE_ACTION_FINISHED 12
#define VOICE_MSG_OFFLINE_LOCAL_VOICE 13
#define VOICE_MSG_OFFLINE_AUDIO_CANCEL 14
#define ACOMP_WAKEUP_ALGO_MODE_ESR 1
#define SHELL_EXPORT_CMD(...)
#define shellPrint(...) ((void)0)
#define VOICE_MSG_BUTTON_ACTION_PRESS_DOWN 0
#define VOICE_MSG_BUTTON_ACTION_CLICK 1
#define VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK 8
#define VOICE_MSG_BUTTON_ACTION_LONG_HOLD 13
typedef void *SemaphoreHandle_t;
typedef int ota_state_e;
typedef int lingxin_dialog_phase_t;
enum {OTA_STATE_IDLE, OTA_STATE_CHECKING, OTA_STATE_PACKAGE_INFO, OTA_STATE_UPDATING};
typedef struct {int mode, active_bearer; bool connected,switching,wifi_available,wifi_connected,modem_connected;} sys_network_status_t;
typedef struct {uint32_t generation; int command;} voice_msg_offline_local_voice_t;
struct app_datas {int can_wakeup, voice_work_mode;};
static struct app_datas app={1,1};
static unsigned ticks, prompt, cloud, motion, stops, timeout_value, mode_calls;
static int last_motion;
static unsigned last_cycles;
static bool online, provision, ble_phone, busy, suspended, blocked, fail_control;
static bool submit_busy_once, supersede_online_publish;
static bool tts_playing, local_tone_playing;
static bool voice_player_mode_command_blocked(void) {return tts_playing || local_tone_playing;}
static bool sleep_during_chat_stop;
static bool exhausted, portal_manual, no_saved_ap, stale_wifi, no_signal;
static bool portal_auto, portal_unknown;
static unsigned wake_takeovers, background_takeovers;
static unsigned stop_busy_count;
static bool cancel_during_submit;
static int cloud_phase;
static int ota, resume_count;
static int volume=70, volume_updates, netcfg_calls;
static unsigned local_voices, reconnects, probes, audio_cancels;
static bool cancel_pending, callbacks_pending;
static bool supersede_netcfg;
static int netcfg_result;
static bool voice_player_offline_audio_cancel_ready(uint32_t generation) {
 return !cancel_pending && !callbacks_pending;
}
static int local_command;
static bool sys_wifi_is_connected(void) {return online || stale_wifi;}
static bool sys_wifi_get_ip_addr(uint32_t *ip) {*ip=0;return online;}
static bool sys_wifi_has_ap(void) {return !no_saved_ap;}
static bool sys_wifi_radio_suspended(void) {return suspended;}
static bool ip_stalled;
static bool sys_wifi_ip_recovery_needed(void) {return ip_stalled;}
static bool sys_wifi_reconnect_saved_exhausted(void) {return exhausted;}
static bool sys_wifi_reconnect_saved_no_signal(void) {return no_signal && exhausted;}
static int sys_wifi_reconnect_saved_once_start(void) {++reconnects;return 0;}
static void sys_wifi_reconnect_saved_once_stop(void) {}
static int network_probe_start(void) {++probes;return 0;}
static void sys_network_report_transport_failure(void) {online=false;}
static int app_netcfg_portal_start(int reason) {++netcfg_calls;return 0;}
static bool app_netcfg_portal_is_active(void) {return portal_manual || portal_auto;}
static int app_netcfg_portal_retry_saved(void) {return 0;}
static int app_netcfg_portal_retry_saved_background(void) {
 if(portal_manual || (portal_auto && portal_unknown)) return -EBUSY;
 if(portal_auto) {++background_takeovers;portal_auto=false;}
 return 0;
}
static int app_netcfg_portal_retry_saved_on_wake(void) {
 if(portal_manual || provision) return -EBUSY;
 if(portal_auto) {++wake_takeovers;portal_auto=false;}
 return 0;
}
static void app_ble_netcfg_adv_start_delayed(void) {}
#define APP_NETCFG_PORTAL_REASON_CONNECT_FAILED 1
static void service_volume_adjust(int delta) {volume+=delta;++volume_updates;}
static int app_enter_manual_netcfg(void) {
 assert(!busy);++netcfg_calls;provision=netcfg_result==0;
 if(supersede_netcfg) {supersede_netcfg=false;service_offline_action_handle_result("hello",true);}
 return netcfg_result;
}
static uint32_t xTaskGetTickCount(void) {return ticks;}
static void vTaskDelay(unsigned n) {ticks+=n;}
static int xTaskCreate(void (*f)(void*),const char*n,int st,void*a,int p,void*t) {return 1;}
static void *xSemaphoreCreateMutex(void) {return (void*)1;}
static int xSemaphoreTake(void*s,int n) {return 1;}
static void xSemaphoreGive(void*s) {}
static bool app_wakeup_product_ready(void) {return true;}
static int sys_network_get_status(sys_network_status_t*s) {s->connected=online;return 0;}
static struct app_datas *get_app_datas(void) {return &app;}
static ota_state_e ota_manager_get_state(void) {return ota;}
bool app_voice_interaction_blocked(void) {return blocked;}
static bool app_netcfg_portal_offline_blocked(void) {return provision;}
static int app_netcfg_portal_offline_takeover(void) {return provision?-1:0;}
static bool app_ble_netcfg_offline_blocked(void) {return ble_phone;}
static int app_ble_netcfg_offline_acquire(void) {return ble_phone?-1:0;}
static void app_ble_netcfg_offline_release(void) {}
static int service_v2_voice_standby_get_state(void) {return suspended?3:0;}
static int service_v2_voice_standby_wake_pose(bool a) {return 0;}
static int service_v2_voice_standby_resume_wait(unsigned ms) {++resume_count;suspended=false;return 0;}
static void service_v2_voice_standby_note_activity(void) {}
static void service_v2_voice_standby_note_keyword(void) {}
static void service_voice_action_cancel_pending(void) {}
static bool query_idle=true;
static bool service_voice_action_query_idle(void) {return query_idle;}
int service_v2_gait_submit(service_v2_gait_action_t a,uint32_t c) {
 assert(a==0);++stops;
 if(stop_busy_count) {--stop_busy_count;return -EBUSY;}
 busy=false;return 0;
}
int service_v2_gait_submit_if_idle(service_v2_gait_action_t a,uint32_t c) {
 if(submit_busy_once) {submit_busy_once=false;return -EBUSY;}
 assert(!busy && !provision && !suspended && !ota);
 last_motion=a;last_cycles=c;
 ++motion;busy=true;
 if(cancel_during_submit) {cancel_during_submit=false;service_offline_action_cancel();}
 return 0;
}
int service_v2_gait_get_status(service_v2_gait_status_t*s) {s->ready=true;s->busy=busy;return 0;}
static int acomp_wakeup_set_timeout(unsigned ms) {timeout_value=ms;return fail_control?-1:0;}
static int acomp_wakeup_set_algo_mode(int m) {assert(m==0 || m==1);++mode_calls;return 0;}
static int lingxin_server_chat_stop(void) {if(sleep_during_chat_stop) suspended=true;return 0;}
static int lingxin_server_audio_uplink_gate_close(const char*r) {return 0;}
static bool lingxin_server_chat_is_active(void) {return false;}
static int lingxin_server_dialog_phase(void) {return cloud_phase;}
#define LINGXIN_DIALOG_PHASE_IDLE 0
#define LINGXIN_DIALOG_PHASE_SPEAKING 2
#define LINGXIN_DIALOG_PHASE_UNAVAILABLE -1
static int voice_msg_pub(unsigned id,void*d,unsigned n) {
 if(id==10) {prompt=*(unsigned*)d;assert(n==4);}
 else if(id==13) {voice_msg_offline_local_voice_t*r=d;prompt=r->generation;local_command=r->command;++local_voices;
 if (supersede_online_publish && r->command==262) {supersede_online_publish=false;service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);}}
 else if(id==14) {++audio_cancels;}
 else if(id!=12) {assert(id==11 && online);++cloud;}
 return 0;
}
''' + c_function((ROOT/'apps/arcs-mini/services/service_v2_gait.c').read_text(encoding='utf-8'),
                 'service_v2_gait_default_cycles') + production + r'''
static void begin(void) {
 assert(service_offline_action_handle_result("hello",true));
 offline_step();
}
static void ready(void) {
 service_offline_action_prompt_finished(prompt,true);
 ticks+=200;offline_step();
}
int main(void) {
 assert(service_offline_action_init()==0);
 assert(s_netcfg_esr_count==0 && s_netcfg_button_count==0);
 offline_step();assert(timeout_value==600000);
 /* A physical CLICK compares and claims one owner atomically; a mismatch
  * must neither increment the owner nor enqueue an offline wake. */
 online=true;
 unsigned button_owner=s_policy.generation,button_accepted=123;
 assert(service_offline_action_begin_button_for_generation(button_owner+1,&button_accepted)==SERVICE_OFFLINE_BUTTON_REJECTED);
 assert(button_accepted==0 && s_policy.generation==button_owner && !s_wake_pending);
 assert(service_offline_action_begin_button_for_generation(button_owner,&button_accepted)==SERVICE_OFFLINE_BUTTON_ONLINE);
 assert(button_accepted==button_owner+1 && s_policy.generation==button_accepted && !s_wake_pending);
 assert(service_offline_action_begin_button_for_generation(button_owner,&button_accepted)==SERVICE_OFFLINE_BUTTON_REJECTED);
 assert(button_accepted==0 && s_policy.generation==button_owner+1);
 service_offline_action_cancel();offline_step();online=false;
 button_owner=s_policy.generation;
 assert(service_offline_action_begin_button_for_generation(button_owner,&button_accepted)==SERVICE_OFFLINE_BUTTON_OFFLINE);
 assert(button_accepted==button_owner+1 && s_wake_pending);
 assert(service_offline_action_begin_button_for_generation(button_accepted,&button_owner)==SERVICE_OFFLINE_BUTTON_REJECTED);
 service_offline_action_cancel();offline_step();assert(!s_wake_pending);
 ota=OTA_STATE_CHECKING;begin();assert(prompt && s_armed);ready();
 assert(s_netcfg_esr_count==0 && s_netcfg_button_count==0);
 service_offline_action_cancel();ota=0;offline_step();prompt=0;
 query_idle=false;unsigned no_wait=ticks;begin();
 assert(service_offline_action_active() && prompt && ticks==no_wait);
 ready();service_offline_action_handle_result("qian2 jin4",false);offline_step();assert(motion==0);
 service_offline_action_cancel();query_idle=true;offline_step();
 begin();assert(cloud==0 && prompt && service_offline_action_active());
 assert(!service_offline_action_button_exit(VOICE_MSG_BUTTON_ACTION_PRESS_DOWN));
 assert(service_offline_action_active());
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);
 offline_step();assert(motion==0);
 ready();
 online=true;offline_step(); /* restoration never steals offline owner */
 submit_busy_once=true;
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);
 offline_step();assert(motion==0 && s_policy.count==1);
 offline_step();assert(motion==1 && cloud==0);
 unsigned old=prompt;
 service_offline_action_cancel();
 service_offline_action_prompt_finished(old,true);
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);
 stop_busy_count=1;
 offline_step();assert(busy && s_stop_pending && s_motion_owned);
 assert(!service_offline_action_diagnostic_acquire());
 assert(!service_offline_action_online_current(service_offline_action_generation()));
 offline_step();assert(motion==1 && !busy && !s_stop_pending && !s_motion_owned && !service_offline_action_active());
 begin();assert(cloud==1 && !service_offline_action_active());
 unsigned online_generation=service_offline_action_generation();
 cloud_phase=3;
 service_offline_action_cloud_failed(online_generation);offline_step();assert(timeout_value==600000);
 cloud_phase=0;
 service_offline_action_cloud_failed(online_generation-1);
 offline_step();assert(timeout_value==600000);
 service_offline_action_cloud_failed(online_generation);
 offline_step();assert(timeout_value==600000); /* definitive failure arms next wake */
 begin();assert(cloud==1 && service_offline_action_active());ready();
 service_offline_action_cancel();offline_step();begin();assert(cloud==2); /* token consumed */
 online_generation=service_offline_action_generation();
 service_offline_action_cloud_failed(online_generation);offline_step();assert(timeout_value==600000);
 service_offline_action_cloud_connected(online_generation);offline_step();assert(timeout_value==600000);
 service_offline_action_cloud_failed(online_generation);offline_step();
 ticks+=60001;offline_step();assert(timeout_value==600000); /* controls remain available */
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);
 offline_step();assert(motion==1 && timeout_value==600000);
 online=false;offline_step();
 assert(service_offline_action_diagnostic_acquire());
 unsigned stops_before_blocked=stops;
 begin();offline_step();assert(!service_offline_action_active() && stops==stops_before_blocked);
 service_offline_action_diagnostic_release(true);offline_step();
 begin();assert(!service_offline_action_diagnostic_acquire());ready();
 provision=true;offline_step();offline_step();
 assert(service_offline_action_active()); /* A credential transaction retains control recognition. */
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);
 offline_step();assert(motion==1);
 stops_before_blocked=stops;
 begin();offline_step();assert(service_offline_action_active() && stops==stops_before_blocked);
 provision=false;offline_step();
 suspended=true;begin();assert(resume_count>0 && !suspended);ready();
 suspended=true;offline_step();assert(!service_offline_action_active());
 suspended=false;offline_step();begin();ready();
 ota=OTA_STATE_UPDATING;offline_step();assert(!service_offline_action_active());
 ota=0;offline_step();begin();ready();
 ble_phone=true;offline_step();assert(service_offline_action_active());
 ble_phone=false;offline_step();begin();ready();
 unsigned stopped_generation=service_offline_action_generation();
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);
 service_offline_action_stop();offline_step();
 assert(motion==1 && service_offline_action_generation()!=stopped_generation);
 assert(service_offline_action_button_exit(VOICE_MSG_BUTTON_ACTION_CLICK));
 offline_step();assert(!service_offline_action_active());begin();ready();
 service_offline_action_engine_idle();offline_step();assert(!service_offline_action_active());
 begin();ready();
 cancel_during_submit=true;stop_busy_count=2;
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);offline_step();
 assert(motion==2 && busy && s_stop_pending && s_motion_owned);
 unsigned previous_prompt=prompt;
 begin();assert(motion==2 && prompt==previous_prompt && s_stop_pending);
 offline_step();assert(!s_stop_pending && !busy && prompt!=previous_prompt);
 ready();service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);offline_step();
 assert(motion==3 && busy);
 unsigned completed_stops=stops;
 offline_step();assert(stops==completed_stops && busy); /* No delayed old STOP hits new motion. */
 service_offline_action_cancel();stop_busy_count=1;offline_step();assert(s_stop_pending);
 busy=false; /* Physical action completes before retry. */
 completed_stops=stops;offline_step();assert(!s_stop_pending && stops==completed_stops);
 /* Utility commands run only after the prompt, independently of a busy gait. */
 begin();
 service_offline_action_handle_result("zeng1 da4 yin1 liang4",false);offline_step();
 assert(volume==70 && volume_updates==0);
 ready();
 service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);offline_step();
 unsigned utility_motion=motion,utility_stops=stops;
 assert(busy);
 ticks+=1000;
 service_offline_action_handle_result("zeng1 da4 yin1 liang4",false);offline_step();
 assert(volume==80 && volume_updates==1 && busy && stops==utility_stops);
 service_offline_action_handle_result("jian3 xiao3 yin1 liang4",false);offline_step();
 assert(volume==70 && volume_updates==2 && motion==utility_motion);
 /* Provisioning outranks a full motion FIFO and a queued volume change. */
 s_policy.forced_offline=true; /* Voice provisioning is an explicit offline-mode control. */
 for(int i=0;i<3;++i) service_offline_action_handle_result("wang3 qiu2 dan1 fan3",false);
 service_offline_action_handle_result("zeng1 da4 yin1 liang4",false);
 unsigned offline_netcfg_before=s_netcfg_esr_count,offline_diag_before=netcfg_diag_calls;
 service_offline_action_handle_result("jin4 ru4 pei4 wang3 mo2 shi4",false);
 assert(s_policy.netcfg_requested && s_netcfg_esr_count==offline_netcfg_before+1);
 assert(netcfg_diag_calls==offline_diag_before+1 && !strcmp(netcfg_diag_source,"esr"));
 stop_busy_count=1;offline_step();assert(netcfg_calls==0 && busy);
 unsigned netcfg_owner=service_offline_action_generation();
 cancel_pending=true;offline_step();assert(netcfg_calls==0);
 cancel_pending=false;callbacks_pending=true;offline_step();assert(netcfg_calls==0);
 callbacks_pending=false;
 offline_step();offline_step();assert(netcfg_calls==1 && !busy && volume==70);
 unsigned netcfg_cancels=audio_cancels;
 for(int i=0;i<20;++i) offline_step();
 assert(service_offline_action_generation()==netcfg_owner && audio_cancels==netcfg_cancels);
 assert(motion==utility_motion && !service_offline_action_active());
 service_offline_action_handle_result("zeng1 da4 yin1 liang4",false);offline_step();assert(volume==70);
 provision=false;offline_step();begin();ready();
 service_offline_action_handle_result("jin4 ru4 pei4 wang3 mo2 shi4",false);
 service_offline_action_cancel();offline_step();assert(netcfg_calls==1);
 begin();ready();
 service_offline_action_handle_result("zeng1 da4 yin1 liang4",false);
 service_offline_action_cancel();offline_step();assert(volume==70);
 s_policy.forced_offline=false;
 online=true;begin();
 /* Product online mode ignores ESR provisioning even after link loss. */
 assert(!s_policy.forced_offline && s_policy.state==OFFLINE_ONLINE);
 for(unsigned connected=0;connected<2;++connected) {
  online=connected!=0;
  offline_policy_t unchanged=s_policy;
  unsigned epoch_before=s_network_epoch,accepted_before=s_accepted,dropped_before=s_dropped;
  unsigned esr_before=s_netcfg_esr_count,diag_before=netcfg_diag_calls;
  bool stop_before=s_stop_pending,reset_before=s_reset;
  assert(service_offline_action_handle_result("jin4 ru4 pei4 wang3 mo2 shi4",false));
  assert(memcmp(&s_policy,&unchanged,sizeof unchanged)==0 && !s_policy.netcfg_requested);
  assert(s_network_epoch==epoch_before && s_accepted==accepted_before && s_dropped==dropped_before+1);
  assert(s_netcfg_esr_count==esr_before && netcfg_diag_calls==diag_before);
  assert(s_stop_pending==stop_before && s_reset==reset_before);
 }
 /* Physical provisioning remains available in online mode. */
 service_offline_action_request_netcfg();
 service_offline_action_handle_result("zeng1 da4 yin1 liang4",false);offline_step();
 assert(netcfg_calls==2 && volume==70);
 provision=false;
 online=false;service_offline_action_cancel();offline_step();
 /* Full-catalog worker dispatch preserves physical action and bounded cycles. */
 begin();ready();
 const struct {const char *intent; int action; unsigned cycles;} dispatch[] = {
  {"qian2 jin4",3,9}, {"hou4 tui4",4,9}, {"zuo3 zhuan3",7,8}, {"you4 zhuan3",8,8},
  {"wang3 qian2 pao3",5,18}, {"wang3 hou4 pao3",6,18},
  {"zhan4 qi3 lai2",1,0}, {"pa1 xia4 lai2",2,0},
  {"dao4 li4",SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,1},
  {"fan1 gen1 tou4",SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,2},
  {"hou4 kong1 fan1",SERVICE_V2_GAIT_ACTION_BACKFLIP,1},
  {"chang4 ge1",SERVICE_V2_GAIT_ACTION_MUSIC_BEAT,0},
  {"wang3 qiu2 fa1 qiu2",12,0}, {"wang3 qiu2 zheng4 shou3",13,0},
  {"wang3 qiu2 dan1 fan3",14,0}, {"wang3 qiu2 shuang1 fan3",15,0},
  {"wo4 shou3",17,3}, {"da3 zhao1 hu1",18,3}, {"nao2 yang3 yang3",19,3},
  {"zhuan4 quan1 quan1",20,20}, {"zuo4 xia4 lai2",21,0},
  {"qin1 qin1 wo3",22,0}, {"tiao4 wu3",23,0}, {"tiao4 yue4",24,0},
  {"niu3 pi4 gu3",25,0}
 };
 for(unsigned i=0;i<sizeof(dispatch)/sizeof(dispatch[0]);++i) {
  unsigned previous=motion;
  service_offline_action_handle_result(dispatch[i].intent,false);offline_step();
  assert(motion==previous+1 && last_motion==dispatch[i].action && last_cycles==dispatch[i].cycles);
  busy=false;offline_step();
 }
 service_offline_action_handle_result("tiao4 wu3",false);offline_step();assert(busy);
 unsigned before_stop_motion=motion;
 for(int i=0;i<3;++i) service_offline_action_handle_result("qian2 jin4",false);
 service_offline_action_handle_result("ting2 zhi3 yi2 dong4",false);
 stop_busy_count=1;offline_step();assert(busy && s_stop_pending);
 offline_step();assert(!busy && s_policy.state==OFFLINE_ACTIVE && motion==before_stop_motion);
 service_offline_action_handle_result("qian2 jin4",false);offline_step();assert(busy);
 service_offline_action_handle_result("xiu1 xi1",false);
 stop_busy_count=1;offline_step();assert(busy && s_stop_pending);
 unsigned before_goodbye_motion=motion;
 service_offline_action_handle_result("qian2 jin4",false);
 offline_step();assert(!busy && !service_offline_action_active() && motion==before_goodbye_motion);
 assert(s_policy.state==OFFLINE_IDLE && timeout_value==600000);
 /* Online physical actions stay cloud-only; mode controls use the worker. */
 online=true;begin();
 unsigned control_cloud=cloud,control_motion=motion;
 service_offline_action_handle_result("qian2 jin4",false);offline_step();
 assert(motion==control_motion);
 unsigned old_owner=service_offline_action_generation();
 service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);
 assert(!service_offline_action_online_current(old_owner));
 offline_step();assert(local_voices==1 && local_command==261);
 assert(s_policy.forced_offline);
 ready();
 ticks+=30001;offline_step();assert(s_policy.state==OFFLINE_IDLE && s_policy.forced_offline);
 assert(!service_offline_action_online_current(service_offline_action_generation()));
 unsigned before_button_modes=mode_calls;
 assert(!service_offline_action_begin_button()); /* Consumed locally, never start cloud. */
 offline_step();ready();
 assert(mode_calls>before_button_modes && cloud==control_cloud && s_policy.state==OFFLINE_ACTIVE);
 service_offline_action_cancel();offline_step();
 begin();ready();assert(cloud==control_cloud);
 service_offline_action_handle_result("cha2 xun2 xi4 tong3 ban3 ben3",false);
 offline_step();assert(local_voices==2 && local_command==263);ready();
 /* Explicit online while already connected must preserve the usable link. */
 online=true;unsigned healthy_probes=probes,healthy_reconnects=reconnects;
 service_offline_action_handle_result("jin4 ru4 lian2 wang3 mo2 shi4",false);offline_step();offline_network_step();
 assert(online && probes==healthy_probes && reconnects==healthy_reconnects && !s_online_pending);
 online=false;stale_wifi=true;ticks+=2000;
 provision=true;
 service_offline_action_handle_result("jin4 ru4 lian2 wang3 mo2 shi4",false);
 offline_step();assert(local_voices==4 && local_command==262 && !s_policy.forced_offline);
 offline_network_step();assert(probes==0 && s_online_pending); /* Mode controls survive short credential transactions. */
 provision=false;offline_step();offline_network_step();
 assert(probes==1);
 ticks+=30001;offline_step();offline_network_step();
 assert(netcfg_calls==2 && s_online_pending && !s_online_started);
 unsigned retry_before=reconnects;
 for(unsigned i=0;i<20;++i) {begin();assert(!service_offline_action_wake_network_allowed(service_offline_action_generation()));ready();offline_network_step();}
 assert(reconnects==retry_before); /* Wakes cannot restart scans or bypass backoff. */
 ticks+=10001;offline_network_step();assert(reconnects==retry_before+1);
 /* A wake queued after mailbox dequeue but before attempt publication merges. */
 s_connect_request=true;s_connect_request_explicit=false;
 s_connect_request_epoch=service_offline_action_network_epoch();
 offline_network_step();assert(reconnects==retry_before+1);
 exhausted=true;offline_network_step();assert(s_online_pending && !s_online_started);
 exhausted=false;ticks+=10001;portal_manual=true;offline_network_step();
 assert(reconnects==retry_before+1); /* Never close a user's manual portal. */
 portal_manual=false;offline_network_step();assert(reconnects==retry_before+2);
 online=true;offline_network_step();assert(!s_online_pending); /* Hotspot restored. */
 online=false;stale_wifi=true;
 offline_network_request(service_offline_action_network_epoch(),false);offline_network_step();
 unsigned stale_before=reconnects;
 ticks+=30001;offline_network_step();ticks+=10001;offline_network_step();
 assert(reconnects==stale_before+1); /* Stale association must eventually reassociate. */
 stale_wifi=false;online=true;offline_network_step();
 online=false;no_saved_ap=true;portal_manual=true;
 offline_network_request(service_offline_action_network_epoch(),false);offline_network_step();
 assert(!s_online_pending && reconnects==stale_before+1 && netcfg_calls==2);
 no_saved_ap=false;portal_manual=false;
 /* Scan failure/visible-but-failed association keeps retrying; only confirmed
  * absent saved SSIDs enters provisioning. */
 offline_network_request(service_offline_action_network_epoch(),false);offline_network_step();
 unsigned before_no_signal=netcfg_calls;
 exhausted=true;no_signal=true;offline_network_step();
 assert(s_online_pending && s_connect_backoff && netcfg_calls==before_no_signal);
 online=true;offline_network_step();online=false;
 exhausted=false;no_signal=false;
 /* If an attempt ends after keyword receipt, this wake still does no networking. */
 offline_network_request(service_offline_action_network_epoch(),false);
 service_offline_action_handle_result("hello",true);
 s_connect_request=false;offline_network_finish();offline_step();
 assert(!s_connect_request && !service_offline_action_wake_network_allowed(service_offline_action_generation()));
 ready();



 service_offline_action_cancel();offline_step();
 online=true;begin();supersede_online_publish=true;
 unsigned attempts_before=reconnects,probes_before=probes;
 service_offline_action_handle_result("jin4 ru4 lian2 wang3 mo2 shi4",false);offline_step();
 offline_network_step();assert(s_policy.local_request==OFFLINE_CMD_ENTER_OFFLINE && !s_online_pending);
 assert(reconnects==attempts_before && probes==probes_before);
 offline_step();assert(s_policy.forced_offline);
 /* Repeated online/offline/provision transitions, with late completions and busy transactions. */
 for (unsigned cycle=0;cycle<1000;++cycle) {
  provision=false;ble_phone=false;online=true;ota=0;
  service_offline_action_cancel();offline_step();begin();
  unsigned stale=service_offline_action_generation();
  service_offline_action_handle_result("jin4 ru4 lian2 wang3 mo2 shi4",false);offline_step();
  provision=true;
  unsigned netcfg_before=s_netcfg_esr_count;
  unsigned diag_before=netcfg_diag_calls;
  service_offline_action_handle_result("jin4 ru4 pei4 wang3 mo2 shi4",false);
  assert(s_netcfg_esr_count==netcfg_before && netcfg_diag_calls==diag_before);
  assert(!s_policy.netcfg_requested); /* Link/credential transactions do not change product mode. */
  unsigned button_count=s_netcfg_button_count;
  service_offline_action_request_netcfg();
  assert(s_netcfg_button_count==button_count+1 && strcmp(s_netcfg_last_source,"button")==0);
  assert(netcfg_diag_calls==diag_before+1 && netcfg_diag_value==(int)(button_count+1));
  assert(!strcmp(netcfg_diag_source,"button"));
  assert(s_policy.netcfg_requested);
  unsigned epoch=service_offline_action_network_epoch();offline_step();offline_network_step();
  assert(!s_online_pending && s_policy.netcfg_requested);
  service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);offline_step();
  assert(s_policy.forced_offline && !s_policy.netcfg_requested);
  service_offline_action_local_audio_finished(stale,true);
  assert(s_policy.state==OFFLINE_PROMPT && service_offline_action_network_epoch()!=epoch);
  service_offline_action_local_audio_finished(s_policy.generation,true);ticks+=200;offline_step();
  unsigned before_wake=ticks;begin();assert(ticks==before_wake && s_policy.state==OFFLINE_PROMPT);
  provision=false;service_offline_action_cancel();offline_step();
 }
 s_policy.forced_offline=false;online=false;
 unsigned button_before=s_netcfg_button_count,esr_before=s_netcfg_esr_count;
 unsigned diag_before=netcfg_diag_calls;
 service_offline_action_request_netcfg();
 assert(s_netcfg_button_count==button_before+1 && s_netcfg_esr_count==esr_before);
 assert(netcfg_diag_calls==diag_before+1 && netcfg_diag_value==(int)(button_before+1));
 assert(!strcmp(netcfg_diag_source,"button"));
 assert(strcmp(s_netcfg_last_source,"button")==0);
 assert(s_policy.netcfg_requested && s_stop_pending);
 provision=false;offline_step();assert(!s_policy.netcfg_requested);
 assert(s_netcfg_button_count==button_before+1);

 /* First boot DHCP stall can recover without a previous healthy session. */
 s_keep_connected=false;online=false;ip_stalled=false;provision=false;
 offline_network_step();assert(!s_online_pending);
 ip_stalled=true;portal_manual=true;offline_network_step();assert(!s_online_pending);
 portal_manual=false;unsigned ip_reconnects=reconnects;
 offline_network_step();assert(s_online_pending && reconnects==ip_reconnects+1);
 ip_stalled=false;online=true;offline_network_step();assert(!s_online_pending);
 /* Awake link loss retries without waiting for another keyword. */
 provision=false;online=true;offline_network_step();
 unsigned keep_reconnects=reconnects,keep_portals=netcfg_calls;
 online=false;stale_wifi=false;
 offline_network_step();offline_network_step();
 assert(s_online_pending && reconnects==keep_reconnects+1);
 no_signal=true;exhausted=true;offline_network_step();
 assert(s_online_pending && s_connect_backoff && netcfg_calls==keep_portals);
 no_signal=false;exhausted=false;online=true;offline_network_step();
 assert(!s_online_pending);
 /* Rest must retire the online command window without changing user mode. */
 online=true;ticks+=2000;begin();
 assert(s_policy.state==OFFLINE_ONLINE && !s_policy.forced_offline);
 unsigned goodbye_generation=s_policy.generation,goodbye_epoch=s_network_epoch;
 service_offline_action_close_online_window(goodbye_generation);
 assert(s_policy.state==OFFLINE_IDLE && s_policy.generation==goodbye_generation && s_network_epoch==goodbye_epoch);
 assert(service_offline_action_online_current(goodbye_generation)); /* Finish goodbye audio normally. */
 service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);
 assert(!s_policy.local_request && !s_policy.forced_offline);
 service_offline_action_handle_result("hello",true);
 service_offline_action_close_online_window(goodbye_generation);assert(s_wake_pending);
 offline_step();assert(s_policy.state==OFFLINE_ONLINE);
 service_offline_action_close_online_window(goodbye_generation);assert(s_policy.state==OFFLINE_ONLINE);
 suspended=true;offline_step();
 assert(s_policy.state==OFFLINE_IDLE && !s_policy.forced_offline);
 unsigned rest_generation=s_policy.generation,rest_epoch=s_network_epoch;
 service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);
 assert(!s_policy.local_request && s_policy.generation==rest_generation && s_network_epoch==rest_epoch);
 begin();assert(!suspended && s_policy.state==OFFLINE_ONLINE);
 /* Speaker echo cannot switch modes; keyword barge-in remains available. */
 for (unsigned cycle=0;cycle<1000;++cycle) {
  tts_playing=(cycle%3)==0;local_tone_playing=(cycle%3)==1;
  cloud_phase=(cycle%3)==2?LINGXIN_DIALOG_PHASE_SPEAKING:0;
  unsigned old_generation=s_policy.generation,old_epoch=s_network_epoch;
  service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);
  offline_step();
  assert(!s_policy.forced_offline && !s_policy.local_request && s_network_epoch==old_epoch);
  service_offline_action_handle_result("hello",true);
  assert(s_wake_pending && s_policy.generation!=old_generation);
  tts_playing=false;local_tone_playing=false;cloud_phase=0;offline_step();
 }
 /* Standby may start while chat_stop runs: recheck before committing mode. */
 unsigned before_rest_voices=local_voices;
 service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);
 sleep_during_chat_stop=true;offline_step();sleep_during_chat_stop=false;
 assert(!s_policy.forced_offline && local_voices==before_rest_voices);
 offline_step();assert(!s_policy.local_request);begin();ticks+=2000;
 /* A fresh awake command still works; resting preserves explicit offline too. */
 service_offline_action_handle_result("jin4 ru4 li2 xian4 mo2 shi4",false);offline_step();
 assert(s_policy.forced_offline);
 assert(service_offline_action_local_audio_current(s_policy.generation));
 suspended=true;assert(!service_offline_action_local_audio_current(s_policy.generation));
 offline_step();assert(s_policy.forced_offline && s_policy.state==OFFLINE_IDLE);
 begin();assert(!suspended && s_policy.forced_offline);
 s_policy.forced_offline=false;online=false;service_offline_action_cancel();offline_step();

 /* Retries/failures and repeated requests preserve the newly handed-off tone. */
 for(int i=0;i<1000;++i) {
  provision=false;service_offline_action_request_netcfg();
  unsigned owner=service_offline_action_generation();
  netcfg_result=-EAGAIN;offline_step();assert(s_policy.netcfg_requested);
  netcfg_result=(i%2)?0:-EIO;offline_step();
  unsigned cancellations=audio_cancels;
  offline_step();offline_step();
  assert(!s_policy.netcfg_requested && s_policy.generation==owner);
  assert(audio_cancels==cancellations);
 }
 provision=false;netcfg_result=0;service_offline_action_request_netcfg();
 supersede_netcfg=true;offline_step();
 assert(s_wake_pending); /* A wake during portal handoff retains its new owner. */
 offline_step();assert(!s_wake_pending && s_policy.state==OFFLINE_PROMPT);
 provision=false;service_offline_action_cancel();offline_step();

 /* A natural wake can recover an automatic portal with unknown clients. */
 online=true;offline_network_step();online=false;
 portal_auto=true;portal_unknown=true;
 unsigned takeovers_before=wake_takeovers, reconnects_before=reconnects;
 button_before=s_netcfg_button_count;esr_before=s_netcfg_esr_count;
 diag_before=netcfg_diag_calls;
 begin();offline_network_step();
 assert(!portal_auto && wake_takeovers==takeovers_before+1 && reconnects==reconnects_before+1);
 for(unsigned i=0;i<20;++i) {begin();offline_network_step();}
 assert(s_netcfg_button_count==button_before && s_netcfg_esr_count==esr_before);
 assert(netcfg_diag_calls==diag_before && critical_depth==0);
 assert(reconnects==reconnects_before+1); /* Never restart a live connection. */
 online=true;offline_network_step();online=false;
 /* Upgrade an existing background attempt blocked by the portal. */
 offline_network_step();assert(s_online_started);
 exhausted=true;offline_network_step();exhausted=false;ticks+=10001;
 portal_auto=true;portal_unknown=true;
 offline_network_step();assert(portal_auto && s_online_pending && !s_online_started);
 takeovers_before=wake_takeovers;reconnects_before=reconnects;
 begin();offline_network_step();
 assert(!portal_auto && wake_takeovers==takeovers_before+1 && reconnects==reconnects_before+1);
 /* Even a user wake cannot evict manual setup or submitted credentials. */
 online=true;offline_network_step();online=false;portal_manual=true;
 begin();offline_network_step();assert(portal_manual && !s_online_started);
 portal_manual=false;portal_auto=true;provision=true;
 begin();offline_network_step();assert(portal_auto && !s_online_started);
 provision=false;offline_network_step();assert(!portal_auto && s_online_started);
 online=true;offline_network_step();online=false;
 /* Idle recovery may reclaim a known empty automatic portal. */
 portal_auto=true;portal_unknown=false;
 unsigned background_before=background_takeovers;
 offline_network_step();assert(!portal_auto && background_takeovers==background_before+1);
 online=true;offline_network_step();online=false;
 unsigned before_last_cloud=cloud;
 service_offline_action_shutdown();assert(s_shutdown);
 assert(!service_offline_action_online_current(s_policy.generation));
 service_offline_action_shutdown_cancelled();assert(!s_shutdown && s_reset);
 fail_control=true;service_offline_action_cancel();offline_step();
 begin();assert(!service_offline_action_active() && s_failed);
 online=true;begin();assert(cloud==before_last_cloud+1); /* AP failure disables only offline. */
 return 0;
}
'''
        self.run_c(code)

    def run_c(self, code):
        services = ROOT / 'apps/arcs-mini/services'
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'offline.c', Path(temp)/'offline.elf'
            cfile.write_text(code, encoding='utf-8')
            build = subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(services),
                                    str(cfile), str(services/'service_offline_action_policy.c'),
                                    '-o', str(elf)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_whitelist_gates_queue_and_generation(self):
        source = ROOT / 'apps/arcs-mini/services/service_offline_action_policy.c'
        self.assertTrue(source.exists(), 'offline owner policy is missing')
        code = r'''
#include <assert.h>
#include "service_offline_action_policy.h"
int main(void) {
 offline_policy_t p = {0};
 assert(offline_map("wang3 qiu2 zheng4 shou3") == SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND);
 assert(offline_map("wang3 qiu2 dan1 fan3") == SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND);
 assert(offline_map("wang3 qiu2 shuang1 fan3") == SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND);
 assert(offline_map("jin4 ru4 pei4 wang3 mo2 shi4") == 256);
 assert(offline_map("zeng1 da4 yin1 liang4") == 257);
 assert(offline_map("jian3 xiao3 yin1 liang4") == 258);
 assert(offline_map("zheng4 shou3 da3 wang3 qiu2") == -1);
 assert(offline_map("wang3 qiu2 zheng4 shou3 suffix") == -1);
 assert(offline_map(0) == -1 && offline_map("") == -1);
 offline_begin(&p, false, 10);
 assert(!offline_enqueue(&p, p.generation, 13, 11));
 offline_begin(&p, true, 20);
 unsigned old = p.generation;
 assert(!offline_enqueue(&p, old, 13, 21));
 offline_prompt_finished(&p, old-1, true, 30);
 offline_poll(&p, 1000, false);
 assert(p.state == OFFLINE_PROMPT);
 offline_prompt_finished(&p, old, true, 1000);
 offline_poll(&p, 1199, false);
 assert(p.state == OFFLINE_TAIL);
 offline_poll(&p, 1200, false);
 assert(p.state == OFFLINE_ACTIVE);
 assert(offline_enqueue(&p, old, 13, 1201));
 assert(offline_enqueue(&p, old, 14, 1202));
 assert(offline_enqueue(&p, old, 15, 1203));
 assert(!offline_enqueue(&p, old, 13, 1204));
 assert(offline_take(&p, old) == 13);
 assert(offline_take(&p, old) == 14);
 assert(offline_take(&p, old) == 15);
 assert(offline_take(&p, old) == -1);
 offline_poll(&p, 31203, true);
 assert(p.state == OFFLINE_ACTIVE);
 offline_poll(&p, 31204, false); /* completion starts a new idle window */
 assert(p.state == OFFLINE_ACTIVE);
 offline_poll(&p, 61203, false);
 assert(p.state == OFFLINE_ACTIVE);
 offline_poll(&p, 61204, false);
 assert(p.state == OFFLINE_IDLE);
 offline_begin(&p, true, 70000);
 assert(!offline_enqueue(&p, old, 13, 70001));
 offline_cancel(&p);
 assert(p.state == OFFLINE_IDLE && p.count == 0);
 assert(!offline_enqueue(&p, old, 13, 70002));
 offline_begin(&p, true, 80000);
 offline_prompt_finished(&p, p.generation, false, 80001);
 assert(p.state == OFFLINE_IDLE);
 offline_begin(&p, true, 90000);
 offline_poll(&p, 120000, false);
 assert(p.state == OFFLINE_IDLE); /* A lost completion fails closed, never unlocks. */
 assert(!offline_eligible(true, false, false, false)); /* online */
 assert(!offline_eligible(false, true, false, false)); /* provisioning */
 assert(!offline_eligible(false, false, true, false)); /* sleep */
 assert(!offline_eligible(false, false, false, true)); /* OTA/shutdown */
 assert(offline_eligible(false, false, false, false));
 offline_begin(&p,true,200000);offline_prompt_finished(&p,p.generation,true,200010);
 offline_poll(&p,200210,false);
 unsigned utility_gen=p.generation;
 assert(offline_enqueue(&p,utility_gen,257,229000));
 assert(offline_take_control(&p,utility_gen)==257);
 offline_poll(&p,250000,false);assert(p.state==OFFLINE_ACTIVE);
 assert(!offline_enqueue(&p,utility_gen,-1,250001));
 offline_poll(&p,259000,false);assert(p.state==OFFLINE_IDLE);
 offline_begin(&p,true,300000);offline_prompt_finished(&p,p.generation,true,300010);
 offline_poll(&p,300210,false);utility_gen=p.generation;
 for(int i=0;i<3;++i) assert(offline_enqueue(&p,utility_gen,257,300211+i));
 assert(!offline_enqueue(&p,utility_gen,258,300220));
 assert(offline_take_control(&p,utility_gen)==257);
 assert(offline_enqueue(&p,utility_gen,258,300221));
 assert(offline_take_control(&p,utility_gen)==257);
 assert(offline_take_control(&p,utility_gen)==257);
 assert(offline_take_control(&p,utility_gen)==258);
 assert(offline_enqueue(&p,utility_gen,13,300222));
 assert(offline_enqueue(&p,utility_gen,257,300223));
 assert(offline_enqueue(&p,utility_gen,256,300224));
 assert(p.netcfg_requested && p.generation!=utility_gen && p.count==0 && p.control_count==0);
 assert(!offline_enqueue(&p,p.generation,257,300225));
 offline_cancel(&p);assert(!p.netcfg_requested);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'offline.c', Path(temp)/'offline.elf'
            cfile.write_text(code, encoding='utf-8')
            build = subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(source.parent),
                                    str(cfile), str(source), '-o', str(elf)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
