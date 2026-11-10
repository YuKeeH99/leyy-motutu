"""Execute charging idle/PWM release against real gait/actuator functions.

Only scheduler, mutex and physical PWM boundaries are stubbed. Interleavings
at each mutex acquisition exercise stale idle snapshots and manual takeovers.
"""
import re
import unittest

from test_lingxin_round_races import COMMON, body, run_c

IDLE = 'apps/arcs-mini/services/service_v2_idle_action.c'
GAIT = 'apps/arcs-mini/services/service_v2_gait.c'
ACT = 'apps/arcs-mini/services/service_v2_actuators.c'

BASE = COMMON + r'''
#include <errno.h>
#define LISA_WAIT_FOREVER -1
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_SERVO_ALL_MASK 15U
#define SERVICE_V2_SERVO_FRAME_MAX_DURATION_MS 60000U
#define SERVICE_V2_SERVO_MIN_PULSE_US 500U
#define SERVICE_V2_SERVO_MAX_PULSE_US 2500U
#define SERVICE_V2_SERVO_EASING_LINEAR 0
typedef int service_v2_servo_easing_t;
typedef struct {uint16_t pulse_us;bool enabled;} servo_state_t;
typedef struct {uint16_t start_us[4],target_us[4];unsigned sequence,duration_ms,elapsed_ms,
 watchdog_ms,watchdog_remaining_ms;int easing;bool active,watchdog_expired;} servo_frame_state_t;
static servo_state_t s_servos[4];
static servo_frame_state_t s_servo_frame;
static bool s_servo_sweep_task_alive;
static bool s_ready=true,act_ready=true,usb=true;
static bool s_worker_active,s_wake_resetting,s_wake_standing,s_roll_reserved,
 s_normal_face_motion_reserved,s_background_home_pending;
static unsigned s_pending_commands,s_normal_face_motion_terminal_pending;
static struct {bool ready,busy;int current;bool background;} s_status;
static void *s_lock=(void*)1,*act_lock=(void*)2,*s_queue=(void*)3;
static unsigned generation=7,queue_depth,gait_locked,act_locked,off_calls,apply_calls;
static unsigned inject_gait,inject_act;
static int disable_error;
static bool power_is_usb_plugged(void){return usb;}
static unsigned service_offline_action_generation(void){return generation;}
static unsigned uxQueueMessagesWaiting(void*q){assert(q==s_queue);return queue_depth;}
static void lisa_mutex_lock(void*p,int timeout){
 if(p==s_lock){assert(!gait_locked && !act_locked);
  if(inject_gait==1)generation++;if(inject_gait==2)s_pending_commands++;
  if(inject_gait==3)s_wake_standing=true;if(inject_gait==4)usb=false;
  gait_locked=1;
 }else{assert(p==act_lock && !act_locked);
  if(inject_act==1)s_servo_frame.active=true;if(inject_act==2)s_servo_sweep_task_alive=true;
  if(inject_act==3)s_servo_frame.watchdog_ms=5000;if(inject_act==4)usb=false;
  act_locked=1;
 }
}
static void lisa_mutex_unlock(void*p){
 if(p==s_lock){assert(gait_locked && !act_locked);gait_locked=0;}
 else{assert(p==act_lock && act_locked);act_locked=0;}
}
static uint8_t servo_enabled_mask_locked(void){assert(act_locked);unsigned mask=0;
 for(unsigned i=0;i<4;i++)if(s_servos[i].enabled)mask|=1U<<i;return mask;}
static bool s_usb_idle_released;
static int servo_disable_all_locked(void){assert(act_locked);s_usb_idle_released=false;off_calls++;
 for(unsigned i=0;i<4;i++)s_servos[i].enabled=false;return disable_error;}
static bool servo_easing_valid(int easing){return easing>=0 && easing<=3;}
static int servo_enable_mask_locked(unsigned mask){assert(act_locked);
 for(unsigned i=0;i<4;i++)if(mask&(1U<<i))s_servos[i].enabled=true;return 0;}
static int servo_apply_mask_locked(unsigned mask){assert(act_locked);apply_calls++;return 0;}
static void reset(void){
 memset(&s_servo_frame,0,sizeof(s_servo_frame));memset(&s_status,0,sizeof(s_status));
 const unsigned home[4]={2100,900,2050,870};for(unsigned i=0;i<4;i++){
  s_servos[i]=(servo_state_t){.pulse_us=home[i],.enabled=true};
  s_servo_frame.start_us[i]=s_servo_frame.target_us[i]=home[i];}
 s_servo_frame.sequence=9;s_servo_frame.duration_ms=s_servo_frame.elapsed_ms=400;
 s_ready=act_ready=usb=true;s_worker_active=s_wake_resetting=s_wake_standing=s_roll_reserved=false;
 s_normal_face_motion_reserved=s_background_home_pending=s_servo_sweep_task_alive=false;
 s_pending_commands=s_normal_face_motion_terminal_pending=queue_depth=0;generation=7;
 gait_locked=act_locked=off_calls=apply_calls=inject_gait=inject_act=0;disable_error=0;
}
'''


def release_functions():
    # Separate translation-unit statics while executing unchanged function bodies.
    actuator = body(ACT, 'service_v2_actuators_servo_release_usb_idle')
    actuator = re.sub(r'\bs_ready\b', 'act_ready', actuator)
    actuator = re.sub(r'\bs_lock\b', 'act_lock', actuator)
    return ('\nint service_v2_actuators_servo_release_usb_idle(void)\n' + actuator +
            '\nint service_v2_gait_release_idle_pwm(uint32_t expected_generation)\n' +
            body(GAIT, 'service_v2_gait_release_idle_pwm'))


class ChargingIdlePwmTest(unittest.TestCase):
    def test_idle_release_never_cancels_gait_or_manual_owner(self):
        run_c(BASE + release_functions() + r'''
int main(void){
 reset();assert(!service_v2_gait_release_idle_pwm(7) && off_calls==1 && !apply_calls);
 assert(!gait_locked && !act_locked && s_servo_frame.sequence==9);
 assert(s_servo_frame.target_us[2]==2050 && s_servos[2].pulse_us==2050);
 assert(!service_v2_gait_release_idle_pwm(7) && off_calls==1);
 bool *owners[]={&s_status.busy,&s_worker_active,&s_wake_resetting,&s_wake_standing,
  &s_roll_reserved,&s_normal_face_motion_reserved};
 for(unsigned i=0;i<sizeof(owners)/sizeof(owners[0]);i++){
  reset();*owners[i]=true;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls);
 }
 unsigned *pending[]={&s_pending_commands,&s_normal_face_motion_terminal_pending,&queue_depth};
 for(unsigned i=0;i<3;i++){
  reset();*pending[i]=1;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls);
 }
 reset();s_servo_frame.active=true;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls && s_servo_frame.active);
 reset();s_servo_sweep_task_alive=true;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls);
 reset();s_servo_frame.watchdog_ms=5000;s_servo_frame.watchdog_remaining_ms=4000;
 assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls && s_servo_frame.watchdog_remaining_ms==4000);
 reset();disable_error=-EIO;assert(service_v2_gait_release_idle_pwm(7)==-EIO && off_calls==1);
 reset();s_ready=false;assert(service_v2_gait_release_idle_pwm(7)==-ENODEV && !off_calls);
 reset();act_ready=false;assert(service_v2_gait_release_idle_pwm(7)==-ENODEV && !off_calls);
}
''')

    def test_new_wake_unplug_and_raw_owner_win_at_lock_boundaries(self):
        run_c(BASE + release_functions() + r'''
int main(void){
 for(unsigned i=1;i<=4;i++){
  reset();inject_gait=i;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls);
  assert(!gait_locked && !act_locked);
 }
 for(unsigned i=1;i<=4;i++){
  reset();inject_act=i;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls);
  assert(!gait_locked && !act_locked);
 }
 reset();generation++;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls);
 reset();usb=false;assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && !off_calls);
}
''')

    def test_release_does_not_latch_off_or_change_pose_before_next_explicit_frame(self):
        run_c(BASE + release_functions() +
              '\nstatic int servo_frame_start_locked(const uint16_t *pulse_us,uint32_t duration_ms,service_v2_servo_easing_t easing)\n' +
              body(ACT, 'servo_frame_start_locked') + r'''
int main(void){
 reset();assert(!service_v2_gait_release_idle_pwm(7) && s_usb_idle_released);
 assert(!service_v2_gait_release_idle_pwm(7) && s_usb_idle_released);
 usb=false;const uint16_t home[4]={2100,900,2050,870};
 lisa_mutex_lock(act_lock,-1);assert(!servo_frame_start_locked(home,400,0));
 assert(servo_enabled_mask_locked()==15 && s_servo_frame.active && s_servo_frame.sequence==10);
 assert(!s_usb_idle_released);
 lisa_mutex_unlock(act_lock);
 assert(service_v2_gait_release_idle_pwm(7)==-EBUSY && off_calls==1);
}
''')

    def test_usb_poll_releases_only_business_idle_and_restarts_unplug_minute(self):
        common = '\nstatic bool business_idle(bool audio_idle)\n' + body(IDLE, 'business_idle')
        run_c(BASE + release_functions() + r'''
#define IDLE_INTERVAL_MS 60000U
#define portTICK_PERIOD_MS 1U
#define SERVICE_V2_IMU_POSE_NORMAL 0
enum {SERVICE_V2_GAIT_ACTION_IDLE_STRETCH=30,SERVICE_V2_GAIT_ACTION_PUSHUP=32,SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH=35};
enum {LINGXIN_DIALOG_PHASE_UNAVAILABLE=-1,LINGXIN_DIALOG_PHASE_IDLE=0,SERVICE_V2_VOICE_STANDBY_ACTIVE=0};
enum {OTA_STATE_CHECKING=1,OTA_STATE_PACKAGE_INFO=2,OTA_STATE_UPDATING=3};
typedef int service_v2_gait_action_t,lingxin_dialog_phase_t,ota_state_e;
typedef struct {bool ready,busy;int current;bool background;} service_v2_gait_status_t;
typedef struct {bool ready,action_active;unsigned queued_count;} service_voice_action_status_t;
typedef struct {bool ready;int pose;} service_v2_imu_interaction_status_t;
typedef struct {bool frame_active,sweep_active,watchdog_expired;unsigned watchdog_ms,enabled_mask;
 uint16_t current_us[4],target_us[4];} service_v2_servo_status_t;
static bool s_idle_valid,owned,chat,query,tts,audio,alarm,portal;
static uint32_t s_idle_since,s_idle_generation,now;
static unsigned submitted;
static int phase,ota,sleeping,voice_error;
static service_voice_action_status_t voice={.ready=true};
static unsigned xTaskGetTickCount(void){return now;}
static unsigned lisa_rand32(void){return 5;}
static unsigned battery_get_voltage_mv(void){return 3900;}
static int lingxin_server_dialog_phase(void){return phase;}
static int ota_manager_get_state(void){return ota;}
static int service_v2_voice_standby_get_state(void){return sleeping;}
static bool service_offline_action_background_allowed(void){return !owned;}
static bool lingxin_server_chat_is_active(void){return chat;}
static bool service_voice_action_query_idle(void){return !query;}
static int service_voice_action_get_status(service_voice_action_status_t*p){*p=voice;return voice_error;}
static bool voice_player_tts_is_active(void){return tts;}
static bool voice_player_is_audio_active(void){return audio;}
static bool alarm_ring_is_active(void){return alarm;}
static bool app_netcfg_portal_is_active(void){return portal;}
static int service_v2_gait_get_status(service_v2_gait_status_t*p){p->ready=true;p->busy=s_status.busy;p->background=s_status.background;return 0;}
static int service_v2_imu_interaction_get_status(service_v2_imu_interaction_status_t*p){p->ready=true;p->pose=0;return 0;}
static int service_v2_actuators_servo_get_status(service_v2_servo_status_t*p){
 memset(p,0,sizeof(*p));for(unsigned i=0;i<4;i++)if(s_servos[i].enabled)p->enabled_mask|=1U<<i;
 const uint16_t home[4]={2100,900,2050,870};
 memcpy(p->current_us,home,8);memcpy(p->target_us,home,8);return 0;}
static int service_v2_gait_submit_background(int action,unsigned cycles){submitted++;return 0;}
''' + common + '\nbool service_v2_background_motion_allowed(void)\n' +
              body(IDLE, 'service_v2_background_motion_allowed') +
              '\nstatic service_v2_gait_action_t idle_random_action(void)\n' + body(IDLE, 'idle_random_action') +
              '\nstatic bool idle_due(uint32_t now,bool idle,uint32_t generation)\n' + body(IDLE, 'idle_due') +
              '\nvoid service_v2_idle_action_poll(bool enabled)\n' + body(IDLE, 'service_v2_idle_action_poll') + r'''
int main(void){
 reset();service_v2_idle_action_poll(true);assert(off_calls==1 && !submitted && !s_idle_valid);
 now=300000;service_v2_idle_action_poll(true);assert(off_calls==1 && !submitted);
 /* USB present is sufficient: no charging-current/battery-full dependency. */
 assert(!service_v2_background_motion_allowed());
 bool *owners[]={&owned,&chat,&query,&tts,&audio,&alarm,&portal,&voice.action_active};
 for(unsigned i=0;i<sizeof(owners)/sizeof(owners[0]);i++){
  reset();*owners[i]=true;service_v2_idle_action_poll(true);assert(!off_calls);*owners[i]=false;
 }
 for(phase=1;phase<=3;phase++){reset();service_v2_idle_action_poll(true);assert(!off_calls);}phase=0;
 for(sleeping=1;sleeping<=4;sleeping++){reset();service_v2_idle_action_poll(true);assert(!off_calls);}sleeping=0;
 for(ota=1;ota<=3;ota++){reset();service_v2_idle_action_poll(true);assert(!off_calls);}ota=0;
 reset();voice.queued_count=1;service_v2_idle_action_poll(true);assert(!off_calls);voice.queued_count=0;
 reset();voice_error=-EIO;service_v2_idle_action_poll(true);assert(!off_calls);voice_error=0;
 reset();s_status.busy=s_worker_active=true;service_v2_idle_action_poll(true);assert(!off_calls);
 /* A canceled background pose is released without a HOME frame. */
 s_status.busy=s_worker_active=false;s_background_home_pending=true;
 service_v2_idle_action_poll(true);assert(off_calls==1 && !apply_calls && !submitted);
 reset();inject_gait=1;service_v2_idle_action_poll(true);assert(!off_calls);
 reset();usb=false;now=500000;service_v2_idle_action_poll(true);
 now+=59999;service_v2_idle_action_poll(true);assert(!submitted && !off_calls);
 usb=true;service_v2_idle_action_poll(true);assert(off_calls==1 && !s_idle_valid);
 usb=false;now++;service_v2_idle_action_poll(true);now+=59999;service_v2_idle_action_poll(true);assert(!submitted);
 now++;service_v2_idle_action_poll(true);assert(!submitted); /* Unplug alone does not re-enable PWM. */
 /* A later accepted wake/explicit pose re-enables outputs, with a fresh generation. */
 generation++;for(unsigned i=0;i<4;i++)s_servos[i].enabled=true;
 service_v2_idle_action_poll(true);now+=59999;service_v2_idle_action_poll(true);assert(!submitted);
 now++;service_v2_idle_action_poll(true);assert(submitted==1);
}
''')


if __name__ == '__main__':
    unittest.main()
