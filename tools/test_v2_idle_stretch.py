import subprocess
import tempfile
import unittest
from pathlib import Path
from test_v2_rgb_effect_contract import ROOT, CC, RUN
from test_lingxin_round_races import body, run_c, COMMON


class IdleStretchTest(unittest.TestCase):
    def test_idle_scheduler_randomizes_three_gestures_and_distinguishes_foreground_pushup(self):
        path = 'apps/arcs-mini/services/service_v2_idle_action.c'
        run_c(COMMON + r'''
#define IDLE_INTERVAL_MS 60000U
#define portTICK_PERIOD_MS 1U
#define SERVICE_V2_SERVO_ALL_MASK 15U
#define SERVICE_V2_IMU_POSE_NORMAL 0
enum {SERVICE_V2_GAIT_ACTION_IDLE_STRETCH=30,SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME=31,
      SERVICE_V2_GAIT_ACTION_PUSHUP=32,SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH=35};
typedef int service_v2_gait_action_t;
typedef struct {bool ready,busy;int current;bool background;} service_v2_gait_status_t;
typedef struct {bool ready;int pose;} service_v2_imu_interaction_status_t;
typedef struct {bool frame_active,sweep_active,watchdog_expired;unsigned watchdog_ms,enabled_mask;
 uint16_t current_us[4],target_us[4];} service_v2_servo_status_t;
static bool s_idle_valid,allowed=true;
static uint32_t s_idle_since,s_idle_generation,now,generation=1;
static unsigned submitted,selected_mask,voltage=3900,random_calls;
static service_v2_gait_status_t gait={.ready=true};
static service_v2_imu_interaction_status_t imu={.ready=true};
static service_v2_servo_status_t servo={.enabled_mask=15,.current_us={2100,900,2050,870},.target_us={2100,900,2050,870}};
static bool service_v2_background_motion_allowed(void){return allowed;}
static bool power_is_usb_plugged(void){return false;}
static bool business_idle(bool audio_idle){return allowed;}
static bool voice_player_is_audio_active(void){return false;}

static int service_v2_gait_release_idle_pwm(unsigned generation){assert(0);return -1;}
static unsigned xTaskGetTickCount(void){return now;}
static unsigned service_offline_action_generation(void){return generation;}
static unsigned lisa_rand32(void){random_calls++;return 0x24681357;}
static unsigned battery_get_voltage_mv(void){return voltage;}
static int service_v2_gait_get_status(service_v2_gait_status_t*p){*p=gait;return 0;}
static int service_v2_imu_interaction_get_status(service_v2_imu_interaction_status_t*p){*p=imu;return 0;}
static int service_v2_actuators_servo_get_status(service_v2_servo_status_t*p){*p=servo;return 0;}
static int service_v2_gait_submit_background(int action,unsigned cycles){
 assert(action==30||action==35||action==32);assert(cycles==0);submitted++;
 selected_mask|=action==30?1:(action==35?2:4);return 0;
}
static service_v2_gait_action_t idle_random_action(void)
''' + body(path, 'idle_random_action') + '\nstatic bool idle_due(uint32_t now,bool idle,uint32_t generation)\n' +
              body(path, 'idle_due') + '\nvoid service_v2_idle_action_poll(bool enabled)\n' +
              body(path, 'service_v2_idle_action_poll') + r'''
int main(void){
 service_v2_idle_action_poll(true);now=59999;service_v2_idle_action_poll(true);assert(!submitted && !random_calls);
 now=60000;service_v2_idle_action_poll(true);assert(submitted==1);
 gait.busy=true;gait.background=true;gait.current=35;now=60100;service_v2_idle_action_poll(true);
 gait.busy=false;now=70000;service_v2_idle_action_poll(true);
 now=119999;service_v2_idle_action_poll(true);assert(submitted==1);
 now=120000;service_v2_idle_action_poll(true);assert(submitted==2);
 for(int i=0;i<30;i++){
  /* Each background gesture preserves the minute measured from its start. */
  unsigned previous=now;
  gait.busy=true;gait.background=true;gait.current=i%3==0?30:(i%3==1?35:32);
  now=previous+100;service_v2_idle_action_poll(true);
  gait.busy=false;now=previous+60000;service_v2_idle_action_poll(true);
 }
 assert(selected_mask==7 && random_calls==1);
 /* A manual/cloud pushup is foreground despite sharing the same action ID. */
 unsigned before_manual=submitted;
 gait.busy=true;gait.background=false;gait.current=32;now++;service_v2_idle_action_poll(true);
 gait.busy=false;now+=1000;service_v2_idle_action_poll(true);
 now+=59999;service_v2_idle_action_poll(true);assert(submitted==before_manual);
 now++;service_v2_idle_action_poll(true);assert(submitted==before_manual+1);
 unsigned before=submitted;
 allowed=false;now++;service_v2_idle_action_poll(true);
 now+=60000;service_v2_idle_action_poll(true);assert(submitted==before);
 allowed=true;service_v2_idle_action_poll(true);now+=59999;service_v2_idle_action_poll(true);assert(submitted==before);
 now++;voltage=3200;service_v2_idle_action_poll(true);assert(submitted==before);
 voltage=3900;now++;service_v2_idle_action_poll(true);assert(submitted==before);
 /* Unsafe opportunity is consumed rather than queued for a later catch-up. */
 now+=59999;service_v2_idle_action_poll(true);assert(submitted==before+1);
 generation++;now++;service_v2_idle_action_poll(true);
 now+=59999;service_v2_idle_action_poll(true);assert(submitted==before+1);
 now++;service_v2_idle_action_poll(true);assert(submitted==before+2);
}
''')

    def test_stretch_keeps_rear_near_standing_and_recovers_front_first(self):
        import re
        path = 'apps/arcs-mini/services/service_v2_gait.c'
        source = (ROOT / path).read_text(encoding='utf-8')
        arrays = '\n'.join(re.search(r'static const uint16_t '+name+r'\[SERVICE_V2_SERVO_COUNT\] = \{[^}]+\};', source).group()
                           for name in ('s_idle_stretch_support', 's_idle_stretch_extend', 's_idle_stretch_a', 's_idle_stretch_b'))
        run_c(COMMON + r'''
#include <errno.h>
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_SERVO_EASING_SMOOTH 1
#define SERVICE_V2_GAIT_ACTION_IDLE_STRETCH 30
static int frames,recovered,paused,disabled,fail_at,frame_error;
static uint16_t history[16][4];
static unsigned durations[16],hold_total;
static const uint16_t s_pose_stand[4]={2100,900,2050,870};
static int hold_pose(unsigned ms){hold_total+=ms;return 0;}
static int prepare_normal_face_motion(const char*s,bool*b){*b=true;return 0;}
static int play_frame(const uint16_t*p,unsigned ms,int easing){durations[frames]=ms;memcpy(history[frames++],p,8);return frames==fail_at?frame_error:0;}
static int play_frame_settled(const uint16_t*p,unsigned ms,int easing){return play_frame(p,ms,easing);}
static int recover_normal_face_home_two_stage(void){recovered++;return 0;}
static void background_motion_pause(void){paused++;}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void status_set(bool busy,int action,unsigned done,int error){}
''' + arrays + '\nstatic int recover_idle_stretch_home(void)\n' + body(path, 'recover_idle_stretch_home') +
              '\nstatic void run_idle_stretch(void)\n' + body(path, 'run_idle_stretch') + r'''
int main(void){
 run_idle_stretch();assert(frames==10 && !paused && !disabled);
 assert(history[0][0]==2100 && history[0][1]==900);
 /* Rear bend is about 10 degrees (111 us), including recovery. */
 assert(history[0][2]-2050 == 870-history[0][3]);
 for(int i=0;i<9;i++)assert(history[i][2]==2161 && history[i][3]==759);
 /* Stay on the lower, prone side of the front-leg sweep, not assembly-horizontal. */
 assert(history[1][0]==1750 && history[1][1]==1250);
 assert(durations[0]>=1000 && durations[1]>=1400 && durations[8]>=1200 && durations[9]>=1000);
 assert(hold_total>=2500);
 assert(history[2][0]!=history[3][0] && history[2][1]!=history[3][1]);
 assert(history[2][0]>=history[1][0]+140 && history[2][1]<=history[1][1]-140);
 for(int i=2;i<8;i++){
  assert(history[i][0]>=history[1][0] && history[i][0]<=history[1][0]+150);
  assert(history[i][1]<=history[1][1] && history[i][1]>=history[1][1]-150);
  assert(durations[i]>=500);
 }
 assert(!memcmp(history[8],history[0],8));assert(!memcmp(history[9],s_pose_stand,8));
 frames=recovered=0;fail_at=3;frame_error=-ECANCELED;run_idle_stretch();
 assert(frames==3 && !recovered && paused==1 && !disabled);
 frames=paused=0;frame_error=-EIO;run_idle_stretch();assert(frames==3 && disabled==1 && !paused);
}
''')

    def test_background_never_runs_foreground_automatic_flip(self):
        path = 'apps/arcs-mini/services/service_v2_gait.c'
        run_c(COMMON + r'''
#include <errno.h>
typedef struct {bool background;int action;} gait_cmd_t;
static bool pending;
static int auto_flip;
static bool cmd_pending(void){return pending;}
static int gait_preflight_normal_face(int action){auto_flip++;return 7;}
static int command_preflight(const gait_cmd_t *cmd)
''' + body(path, 'command_preflight') + r'''
int main(void){
 gait_cmd_t cmd={.background=true,.action=31};
 assert(command_preflight(&cmd)==0 && auto_flip==0);
 pending=true;assert(command_preflight(&cmd)==-ECANCELED && auto_flip==0);
 cmd.background=false;assert(command_preflight(&cmd)==7 && auto_flip==1);
}
''')

    def test_cancel_defers_home_until_idle_and_yields_to_other_owners(self):
        path = 'apps/arcs-mini/services/service_v2_gait.c'
        run_c(COMMON + r'''
#define SERVICE_V2_SERVO_ALL_MASK 15U
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME 31
#define SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH 35
#define SERVICE_V2_GAIT_ACTION_PUSHUP 32
#define SERVICE_V2_GAIT_ACTION_CODEX_SQUAT 36
#define SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK 11
#define SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE 16
#define LISA_WAIT_FOREVER 0
#include <errno.h>
#define GAIT_HOME_RETURN_MS 400U
#define SERVICE_V2_SERVO_EASING_SMOOTH 1
typedef struct {unsigned sequence,enabled_mask,watchdog_ms;bool frame_active,sweep_active,watchdog_expired;uint16_t current_us[4],target_us[4];} service_v2_servo_status_t;
typedef struct {bool background;unsigned generation;int action;} gait_cmd_t;
static bool s_background_home_pending,s_background_command,s_worker_active,allowed;
static bool s_background_home_two_stage;
static int s_background_home_action;
static service_v2_servo_status_t s_background_home_snapshot;
static unsigned s_background_generation,s_pending_commands;
static struct {bool busy;int current,cycles_done,last_error;} s_status;
static void *s_lock;
static service_v2_servo_status_t servo={.sequence=4,.enabled_mask=15};
static int stop_error,status_error,submit_error,submits,stops;
static int prepare_error,two_stage,direct,disabled;
static const uint16_t s_pose_stand[4]={2100,900,2050,870};
static int prepare_normal_face_motion(const char*s,bool*p){*p=true;return prepare_error;}
static int recover_idle_stretch_home(void){two_stage++;return 0;}
static int recover_idle_scratch_home(void){return 0;}
static int recover_pushup_home(void){return 0;}
static int recover_codex_squat_home(void){assert(0);return 0;}
static int play_frame_settled(const uint16_t*p,unsigned ms,int easing){direct++;return 0;}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void status_set(bool busy,int action,unsigned cycles,int error){s_status.current=action;}
static int service_v2_actuators_servo_frame_stop(void){stops++;servo.sequence++;servo.frame_active=false;return stop_error;}
static int service_v2_actuators_servo_get_status(service_v2_servo_status_t*p){*p=servo;return status_error;}
static bool service_v2_background_motion_allowed(void){return allowed;}
static int service_v2_gait_submit_background(int action,unsigned cycles){assert(allowed);assert(action==31);submits++;return submit_error;}
static void lisa_mutex_lock(void*p,int wait){}
static void lisa_mutex_unlock(void*p){}
static void background_motion_pause(void)
''' + body(path, 'background_motion_pause') + r'''
static bool background_home_owns_pose(const service_v2_servo_status_t *servo)
''' + body(path, 'background_home_owns_pose') + r'''
static void background_home_retry(void)
''' + body(path, 'background_home_retry') + r'''
static void command_begin(const gait_cmd_t *cmd)
''' + body(path, 'command_begin') + r'''
static void run_background_home(void)
''' + body(path, 'run_background_home') + r'''
int main(void){
 background_motion_pause();assert(s_background_home_pending && stops==1);
 /* No restoration during dialogue, sleeping, USB, or another audio owner. */
 for(int i=0;i<10;i++)background_home_retry();assert(submits==0 && s_background_home_pending);
 allowed=true;submit_error=-1;background_home_retry();assert(submits==1 && s_background_home_pending);
 submit_error=0;background_home_retry();assert(submits==2 && !s_background_home_pending);
 background_home_retry();assert(submits==2);
 /* New local pose/voice/standby command supersedes the old recovery. */
 background_motion_pause();gait_cmd_t cmd={.background=false};command_begin(&cmd);
 assert(!s_background_home_pending);background_home_retry();assert(submits==2);
 background_motion_pause();servo.sequence++;background_home_retry();assert(!s_background_home_pending && submits==2);
 background_motion_pause();servo.watchdog_ms=100;background_home_retry();assert(!s_background_home_pending);
 servo.watchdog_ms=0;background_motion_pause();servo.enabled_mask=0;background_home_retry();assert(!s_background_home_pending);
 servo.enabled_mask=15;background_motion_pause();servo.current_us[0]++;
 background_home_retry();assert(!s_background_home_pending && submits==2);
 background_motion_pause();servo.target_us[3]++;
 background_home_retry();assert(!s_background_home_pending && submits==2);
 /* Recover the original touch path, and preserve it across another pause. */
 s_status.current=11;background_motion_pause();s_status.current=31;run_background_home();
 assert(two_stage==1 && direct==0 && disabled==0);
 prepare_error=-ECANCELED;run_background_home();assert(s_background_home_pending);
 prepare_error=0;run_background_home();assert(two_stage==2 && direct==0);
 s_status.current=30;background_motion_pause();s_status.current=31;run_background_home();
 assert(two_stage==3 && direct==0);
 /* A manual pose change after queueing must also cancel without PWM off. */
 servo.current_us[1]++;run_background_home();assert(direct==0 && disabled==0);
}
''')

    def test_background_gate_rejects_dialogue_sleep_and_owned_work(self):
        path = 'apps/arcs-mini/services/service_v2_idle_action.c'
        run_c(COMMON + r'''
typedef int lingxin_dialog_phase_t;
typedef int ota_state_e;
enum {LINGXIN_DIALOG_PHASE_UNAVAILABLE=-1,LINGXIN_DIALOG_PHASE_IDLE=0};
enum {SERVICE_V2_VOICE_STANDBY_ACTIVE=0};
enum {OTA_STATE_CHECKING=1,OTA_STATE_PACKAGE_INFO=2,OTA_STATE_UPDATING=3};
typedef struct {bool ready,action_active;unsigned queued_count;} service_voice_action_status_t;
static int phase,ota,sleeping,voice_error;
static bool owned,usb,chat,query,tts,audio,alarm,portal;
static service_voice_action_status_t voice={.ready=true};
static int lingxin_server_dialog_phase(void){return phase;}
static int ota_manager_get_state(void){return ota;}
static int service_v2_voice_standby_get_state(void){return sleeping;}
static bool service_offline_action_background_allowed(void){return !owned;}
static bool power_is_usb_plugged(void){return usb;}
static bool lingxin_server_chat_is_active(void){return chat;}
static bool service_voice_action_query_idle(void){return !query;}
static int service_voice_action_get_status(service_voice_action_status_t*p){*p=voice;return voice_error;}
static bool voice_player_tts_is_active(void){return tts;}
static bool voice_player_is_audio_active(void){return audio;}
static bool alarm_ring_is_active(void){return alarm;}
static bool app_netcfg_portal_is_active(void){return portal;}
static bool business_idle(bool audio_idle)
''' + body(path, 'business_idle') + '\nbool service_v2_background_motion_allowed(void)\n' +
              body(path, 'service_v2_background_motion_allowed') + r'''
int main(void){
 assert(service_v2_background_motion_allowed());
 for(phase=1;phase<=3;phase++)assert(!service_v2_background_motion_allowed());phase=0;
 for(sleeping=1;sleeping<=4;sleeping++)assert(!service_v2_background_motion_allowed());sleeping=0;
 bool* gates[]={&owned,&usb,&chat,&query,&tts,&audio,&alarm,&portal,&voice.action_active};
 for(unsigned i=0;i<sizeof(gates)/sizeof(gates[0]);i++){
  *gates[i]=true;assert(!service_v2_background_motion_allowed());*gates[i]=false;
 }
 for(ota=1;ota<=3;ota++)assert(!service_v2_background_motion_allowed());ota=0;
 voice.queued_count=1;assert(!service_v2_background_motion_allowed());voice.queued_count=0;
 voice_error=-1;assert(!service_v2_background_motion_allowed());voice_error=0;
 assert(service_v2_background_motion_allowed());
}
''')

    def test_background_frame_yields_within_one_tick_without_changing_normal_actions(self):
        path = 'apps/arcs-mini/services/service_v2_gait.c'
        run_c(COMMON + r'''
#include <errno.h>
#define GAIT_TICK_MS 20U
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP 9
#define SERVICE_V2_GAIT_ACTION_BACKFLIP 33
typedef int service_v2_servo_easing_t;
static bool s_background_command=true,s_wake_resetting,allowed=true;
static uint32_t s_background_generation=7,generation=7;
static void*s_queue;
static struct{bool busy;int current;}s_status;
static unsigned frames,elapsed;
static int mode;
static uint32_t service_offline_action_generation(void){return generation;}
static bool service_v2_background_motion_allowed(void){return allowed;}
static unsigned uxQueueMessagesWaiting(void*q){return 0;}
static bool gait_safety_gates_enabled(void){return false;}
static bool power_is_usb_plugged(void){return false;}
static int service_v2_actuators_servo_frame(const uint16_t*p,uint32_t ms,int easing){frames++;return 0;}
static void lisa_thread_mdelay(unsigned ms){elapsed+=ms;if(mode==1)allowed=false;if(mode==2)generation++;}
static int service_v2_actuators_servo_watchdog_kick(void){return 0;}
static bool cmd_pending(void)
''' + body(path, 'cmd_pending') + r'''
static int play_frame(const uint16_t p[4],uint32_t duration_ms,int easing)
''' .replace('p[4]', 'pulse_us[4]') + body(path, 'play_frame') + r'''
int main(void){
 const uint16_t p[4]={2100,900,2050,870};
 mode=1;assert(play_frame(p,400,0)==-ECANCELED);assert(elapsed==20 && frames==1);
 assert(play_frame(p,400,0)==-ECANCELED);assert(frames==1);
 allowed=true;mode=2;elapsed=0;assert(play_frame(p,400,0)==-ECANCELED);assert(elapsed==20);
 /* Cloud/user motion keeps its original semantics even during dialogue. */
 s_background_command=false;mode=0;allowed=false;elapsed=0;
 assert(play_frame(p,400,0)==0 && elapsed==400);
}
''')

    def test_idle_minute_restarts_after_dialogue_foreground_motion_and_sleep(self):
        run_c(COMMON + r'''
#define IDLE_INTERVAL_MS 60000U
static bool s_idle_valid;
static uint32_t s_idle_since,s_idle_generation;
static bool idle_due(uint32_t now, bool idle, uint32_t generation)
''' + body('apps/arcs-mini/services/service_v2_idle_action.c', 'idle_due') + r'''
int main(void){
 assert(!idle_due(1000,true,1));
 assert(!idle_due(60999,true,1));assert(idle_due(61000,true,1));
 assert(!idle_due(61001,true,1));assert(idle_due(121000,true,1));
 assert(!idle_due(122000,false,1));assert(!idle_due(130000,true,1));
 assert(!idle_due(189999,true,1));assert(idle_due(190000,true,1));
 assert(!idle_due(200000,true,2));assert(!idle_due(259999,true,2));assert(idle_due(260000,true,2));
 s_idle_valid=false;assert(!idle_due(UINT32_MAX-10000U,true,3));assert(idle_due(49999U,true,3));
}
''')
