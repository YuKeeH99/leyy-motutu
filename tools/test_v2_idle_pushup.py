"""Execute background pushup ownership/cancellation/recovery with production C."""
import re
import unittest

from test_lingxin_round_races import ROOT, COMMON, body, run_c

PATH = "apps/arcs-mini/services/service_v2_gait.c"
HEADER = "apps/arcs-mini/services/service_v2_gait.h"


def base():
    return COMMON + (ROOT / HEADER).read_text(encoding="utf-8") + r'''
#include <errno.h>
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_SERVO_EASING_SMOOTH 1
#define SERVICE_V2_SERVO_ALL_MASK 15U
#define LISA_WAIT_FOREVER 0
'''


def poses():
    source = (ROOT / PATH).read_text(encoding="utf-8")
    return "\n".join(re.findall(r"^#define GAIT_HOME_\w+[^\n]*", source, re.M)) + "\n" + "\n".join(
        re.findall(r"static const uint16_t (?:s_pushup_\w+|s_pose_stand)\[SERVICE_V2_SERVO_COUNT\] = \{[^}]+\};", source))


class IdlePushupTest(unittest.TestCase):
    def test_background_cancel_holds_current_frame_while_foreground_keeps_fail_off(self):
        run_c(base() + poses() + r'''
static bool s_background_command=true;
static unsigned frames,done,disabled,paused,fail_at,hold_calls,fail_hold;
static int last_error,frame_error,prep_error;
static uint16_t history[32][4];
static int prepare_normal_face_motion(const char*s,bool*p){*p=prep_error==0;return prep_error;}
static int play_frame_settled(const uint16_t*p,unsigned ms,int easing){
 assert(frames<32);memcpy(history[frames++],p,8);return frames==fail_at?frame_error:0;
}
static int hold_pose(unsigned ms){return ++hold_calls==fail_hold?-ECANCELED:0;}
static void background_motion_pause(void){paused++;}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void status_set(bool busy,int action,unsigned cycles,int error){assert(action==SERVICE_V2_GAIT_ACTION_PUSHUP);done=cycles;last_error=error;}
static int recover_pushup_home(void)
''' + body(PATH, "recover_pushup_home") +
              "\nstatic void run_pushup(uint32_t cycles)\n" + body(PATH, "run_pushup") + r'''
int main(void){
 for(unsigned interrupted=1;interrupted<=12;interrupted++){
  frames=done=disabled=paused=hold_calls=0;fail_at=interrupted;frame_error=-ECANCELED;
  run_pushup(4);
  assert(frames==interrupted && paused==1 && disabled==0 && !last_error);
 }
 frames=done=disabled=paused=hold_calls=fail_at=0;fail_hold=1;run_pushup(4);
 assert(frames==2 && paused==1 && !disabled && !done);fail_hold=0;
 /* Foreground pushups retain their established cancellation behavior. */
 frames=done=disabled=paused=0;s_background_command=false;fail_at=3;run_pushup(4);
 assert(frames==3 && paused==0 && disabled==1);
 frames=done=disabled=paused=0;s_background_command=true;frame_error=-EIO;run_pushup(4);
 assert(frames==3 && !paused && disabled==1 && last_error==-EIO);
 frames=disabled=paused=0;prep_error=-EPERM;run_pushup(4);
 assert(!frames && !paused && !disabled && last_error==-EPERM);
}
''')

    def test_background_pushup_uses_existing_cycles_validation_and_idle_submission(self):
        run_c(base() + r'''
static int normalize_command(service_v2_gait_action_t action,uint32_t *cycles)
''' + body(PATH, "normalize_command") + r'''
static unsigned submits,actual_cycles;
static int submit_command(int action,unsigned cycles,unsigned speed,bool idle,bool background){
 assert(action==SERVICE_V2_GAIT_ACTION_PUSHUP && speed==50 && idle && background);
 int ret=normalize_command(action,&cycles);if(ret)return ret;
 actual_cycles=cycles;submits++;return 0;
}
int service_v2_gait_submit_background(service_v2_gait_action_t action,uint32_t cycles)
''' + body(PATH, "service_v2_gait_submit_background") + r'''
int main(void){
 assert(service_v2_gait_submit_background(SERVICE_V2_GAIT_ACTION_PUSHUP,0)==0 && actual_cycles==4);
 for(unsigned cycles=3;cycles<=5;cycles++){
  assert(service_v2_gait_submit_background(SERVICE_V2_GAIT_ACTION_PUSHUP,cycles)==0);
  assert(actual_cycles==cycles);
 }
 assert(service_v2_gait_submit_background(SERVICE_V2_GAIT_ACTION_PUSHUP,2)==-EINVAL);
 assert(service_v2_gait_submit_background(SERVICE_V2_GAIT_ACTION_PUSHUP,6)==-EINVAL);
 assert(service_v2_gait_submit_background(SERVICE_V2_GAIT_ACTION_BACKFLIP,0)==-EINVAL);
 assert(submits==4);
}
''')

    def test_pushup_recovery_preserves_rear_support_and_origin_after_repeated_pause(self):
        source = (ROOT / PATH).read_text(encoding="utf-8")
        self.assertTrue("static int recover_pushup_home(void)" in source,
                        "background pushup requires its own support-first recovery")
        run_c(base() + poses() + r'''
typedef struct {unsigned sequence,enabled_mask,watchdog_ms;bool frame_active,sweep_active,watchdog_expired;uint16_t current_us[4],target_us[4];} service_v2_servo_status_t;
static service_v2_servo_status_t servo={.sequence=4,.enabled_mask=15};
static service_v2_servo_status_t s_background_home_snapshot;
static service_v2_gait_action_t s_background_home_action;
static service_v2_gait_status_t s_status;
static bool s_background_home_pending,allowed;
static unsigned frames,stops,disabled,submits,fail_at;
static unsigned durations[16];
static uint16_t history[16][4];
static int prepare_normal_face_motion(const char*s,bool*p){*p=true;return 0;}
static int play_frame_settled(const uint16_t*p,unsigned ms,int easing){durations[frames]=ms;memcpy(history[frames++],p,8);return frames==fail_at?-ECANCELED:0;}
static int service_v2_actuators_servo_frame_stop(void){stops++;servo.sequence++;return 0;}
static int service_v2_actuators_servo_get_status(service_v2_servo_status_t*p){*p=servo;return 0;}
static bool service_v2_background_motion_allowed(void){return allowed;}
int service_v2_gait_submit_background(service_v2_gait_action_t a,uint32_t c){assert(allowed && a==31);submits++;return 0;}
static int recover_idle_stretch_home(void){assert(0);return 0;}
static int recover_idle_scratch_home(void){assert(0);return 0;}
static int recover_codex_squat_home(void){assert(0);return 0;}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void status_set(bool busy,int action,unsigned cycles,int error){s_status.current=action;}
static int recover_pushup_home(void)
''' + body(PATH, "recover_pushup_home") +
              "\nstatic void background_motion_pause(void)\n" + body(PATH, "background_motion_pause") +
              "\nstatic bool background_home_owns_pose(const service_v2_servo_status_t *servo)\n" + body(PATH, "background_home_owns_pose") +
              "\nstatic void background_home_retry(void)\n" + body(PATH, "background_home_retry") +
              "\nstatic void run_background_home(void)\n" + body(PATH, "run_background_home") + r'''
int main(void){
 s_status.current=SERVICE_V2_GAIT_ACTION_PUSHUP;background_motion_pause();
 assert(s_background_home_pending && s_background_home_action==SERVICE_V2_GAIT_ACTION_PUSHUP);
 background_home_retry();assert(!submits && !frames);
 allowed=true;background_home_retry();assert(submits==1 && !s_background_home_pending);
 s_status.current=SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME;fail_at=1;run_background_home();
 assert(frames==1 && !disabled && s_background_home_pending && s_background_home_action==32);
 assert(!memcmp(history[0],s_pushup_rear,8) && durations[0]==700);
 fail_at=0;frames=0;background_home_retry();run_background_home();
 assert(frames==2 && !disabled && !s_background_home_pending);
 assert(!memcmp(history[0],s_pushup_rear,8) && durations[0]==700);
 assert(!memcmp(history[1],s_pose_stand,8) && durations[1]==1100);
}
''')

    def test_status_reports_owner_instead_of_inferring_it_from_pushup_action(self):
        header = (ROOT / HEADER).read_text(encoding="utf-8")
        self.assertRegex(header, r"bool background;[^}]*\} service_v2_gait_status_t;")
        run_c(base() + r'''
static bool s_ready=true,s_worker_active,s_background_command;
static bool s_wake_resetting,s_wake_standing,s_roll_reserved,s_normal_face_motion_reserved;
static unsigned s_pending_commands;
static void*s_lock;
static service_v2_gait_status_t s_status;
static void lisa_mutex_lock(void*p,int wait){}
static void lisa_mutex_unlock(void*p){}
int service_v2_gait_get_status(service_v2_gait_status_t *status)
''' + body(PATH, "service_v2_gait_get_status") + r'''
int main(void){
 service_v2_gait_status_t result;s_status.current=SERVICE_V2_GAIT_ACTION_PUSHUP;
 s_status.busy=true;s_worker_active=true;s_background_command=true;
 assert(!service_v2_gait_get_status(&result) && result.busy && result.background);
 s_background_command=false;assert(!service_v2_gait_get_status(&result) && result.busy && !result.background);
 s_background_command=true;s_pending_commands=1;
 assert(!service_v2_gait_get_status(&result) && result.busy && !result.background);
 s_pending_commands=0;s_wake_resetting=true;
 assert(!service_v2_gait_get_status(&result) && result.busy && !result.background);
 s_wake_resetting=false;s_wake_standing=true;
 assert(!service_v2_gait_get_status(&result) && result.busy && !result.background);
 s_wake_standing=false;s_worker_active=false;
 assert(!service_v2_gait_get_status(&result) && !result.background);
 s_status.busy=false;s_background_command=false;
 assert(!service_v2_gait_get_status(&result) && !result.busy && !result.background);
}
''')


if __name__ == "__main__":
    unittest.main()
