"""Exercise production front-paw idle frames and cancellation on the C simulator."""
import re
import unittest

from test_lingxin_round_races import ROOT, COMMON, body, run_c


PATH = "apps/arcs-mini/services/service_v2_gait.c"
HEADER = "apps/arcs-mini/services/service_v2_gait.h"


def production_header():
    return (ROOT / HEADER).read_text(encoding="utf-8")


def production_poses():
    source = (ROOT / PATH).read_text(encoding="utf-8")
    defines = "\n".join(re.findall(r"^#define GAIT_HOME_\w+[^\n]*", source, re.M))
    arrays = "\n".join(re.search(
        r"static const uint16_t " + name + r"\[SERVICE_V2_SERVO_COUNT\] = \{[^}]+\};",
        source).group() for name in ("s_pose_sit", "s_pose_stand"))
    return defines + "\n" + arrays


class IdleScratchTest(unittest.TestCase):
    def motion_source(self):
        source = (ROOT / PATH).read_text(encoding="utf-8")
        self.assertTrue("static void run_idle_scratch(void)" in source,
                        "idle scratch runner is not implemented")
        return COMMON + production_header() + r'''
#include <errno.h>
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_SERVO_EASING_SMOOTH 1
static unsigned frames, paused, disabled, done, random_reads, hold_total;
static unsigned fail_at, durations[32];
static int frame_error, prepare_error, last_error;
static bool prepared = true;
static uint16_t history[32][4];
static uint32_t s_idle_scratch_random_state;
static uint32_t lisa_rand32(void){random_reads++;return 1;}
static int prepare_normal_face_motion(const char *s, bool *p){*p=prepared;return prepare_error;}
static int play_frame_settled(const uint16_t *p, unsigned ms, int easing){
 assert(easing==SERVICE_V2_SERVO_EASING_SMOOTH);
 for(unsigned i=0;i<4;i++)assert(p[i]>=500 && p[i]<=2500);
 durations[frames]=ms;memcpy(history[frames++],p,8);
 return frames==fail_at ? frame_error : 0;
}
static int hold_pose(unsigned ms){hold_total+=ms;return 0;}
static void background_motion_pause(void){paused++;}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void status_set(bool busy,int action,unsigned cycles,int error){
 assert(!busy && action==SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH);done=cycles;last_error=error;
}
''' + production_poses() + "\nstatic uint32_t idle_scratch_channel(void)\n" + body(
            PATH, "idle_scratch_channel") + "\nstatic int recover_idle_scratch_home(void)\n" + body(
            PATH, "recover_idle_scratch_home") + "\nstatic void run_idle_scratch(void)\n" + body(
            PATH, "run_idle_scratch")

    def test_both_physical_front_legs_reach_all_ten_endpoints_then_sit_and_stand(self):
        run_c(self.motion_source() + r'''
static uint32_t next_random(uint32_t value){value^=value<<13;value^=value>>17;value^=value<<5;return value;}
int main(void){
 for(uint32_t seed=1;seed<=2;seed++){
  frames=done=0;s_idle_scratch_random_state=seed;
  run_idle_scratch();
  /* One random decision per motion, never switching front legs halfway. */
  assert(s_idle_scratch_random_state==next_random(seed));
  assert(frames==14 && done==5 && !paused && !disabled && !last_error);
  unsigned ch=seed==1 ? 1 : 0;
  assert(!memcmp(history[0],s_pose_sit,8));
  assert(history[1][ch]==(ch==0 ? 800 : 2200) && durations[1]==600);
  for(unsigned i=0;i<5;i++){
   assert(history[2+i*2][ch]==(ch==0 ? 1100 : 1900));
   assert(history[3+i*2][ch]==(ch==0 ? 800 : 2200));
   assert(durations[2+i*2]==200);
   assert(durations[3+i*2]==200);
  }
  for(unsigned i=0;i<13;i++)for(unsigned leg=0;leg<4;leg++)
   if(leg!=ch)assert(history[i][leg]==s_pose_sit[leg]);
  assert(!memcmp(history[12],s_pose_sit,8));
  assert(!memcmp(history[13],s_pose_stand,8));
 }
 s_idle_scratch_random_state=0;assert(idle_scratch_channel()==1 && random_reads==1);
 idle_scratch_channel();assert(random_reads==1);
}
''')

    def test_interruption_at_every_frame_holds_pose_without_forced_recovery(self):
        run_c(self.motion_source() + r'''
int main(void){
 for(unsigned interrupted=1;interrupted<=14;interrupted++){
  frames=paused=disabled=done=0;fail_at=interrupted;frame_error=-ECANCELED;
  run_idle_scratch();
  assert(frames==interrupted && paused==1 && !disabled && !last_error);
  unsigned completed=interrupted<=2 ? 0 : (interrupted-3)/2;
  if(completed>5)completed=5;assert(done==completed);
 }
 /* Hardware failure disables PWM; a rejected precondition never takes ownership. */
 frames=paused=disabled=0;fail_at=4;frame_error=-EIO;run_idle_scratch();
 assert(frames==4 && !paused && disabled==1 && last_error==-EIO);
 frames=paused=disabled=0;prepared=false;prepare_error=-EPERM;run_idle_scratch();
 assert(!frames && !paused && !disabled && !done && last_error==-EPERM);
}
''')

    def test_parser_normalization_and_background_submission_use_appended_action(self):
        header = production_header()
        # Before the enum is added, still execute the existing routing functions.
        if "SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH" not in header:
            header += "\n#define SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH 35\n"
        run_c(COMMON + header + r'''
#include <errno.h>
static unsigned submitted;
static int submit_command(int action,unsigned cycles,unsigned speed,bool idle,bool background){
 assert(action==SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH && speed==50 && idle && background);
 submitted++;return 7;
}
static int normalize_command(service_v2_gait_action_t action,uint32_t *cycles)
''' + body(PATH, "normalize_command") +
              "\nstatic bool action_is_normal_face_motion(service_v2_gait_action_t action)\n" +
              body(PATH, "action_is_normal_face_motion") +
              "\nbool service_v2_gait_action_from_name(const char *name,service_v2_gait_action_t *action)\n" +
              body(PATH, "service_v2_gait_action_from_name") +
              "\nconst char *service_v2_gait_action_name(service_v2_gait_action_t action)\n" +
              body(PATH, "service_v2_gait_action_name") +
              "\nint service_v2_gait_submit_background(service_v2_gait_action_t action,uint32_t cycles)\n" +
              body(PATH, "service_v2_gait_submit_background") + r'''
int main(void){
 assert(SERVICE_V2_GAIT_ACTION_MUSIC_BEAT==34 && SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH==35);
 service_v2_gait_action_t action=SERVICE_V2_GAIT_ACTION_STOP;
 assert(service_v2_gait_action_from_name("idle_scratch",&action));
 assert(action==SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH);
 assert(service_v2_gait_action_from_name("idle-scratch",&action));
 assert(!service_v2_gait_action_from_name("idle_rear_swing",&action));
 assert(!service_v2_gait_action_from_name("idle-rear-swing",&action));
 assert(!strcmp(service_v2_gait_action_name(action),"idle_scratch"));
 assert(action_is_normal_face_motion(action));
 uint32_t cycles=0;assert(normalize_command(action,&cycles)==0 && cycles==0);
 cycles=99;assert(normalize_command(action,&cycles)==0 && cycles==0);
 assert(service_v2_gait_submit_background(action,0)==7 && submitted==1);
 assert(service_v2_gait_submit_background(SERVICE_V2_GAIT_ACTION_BACKFLIP,0)==-EINVAL && submitted==1);
 assert(service_v2_gait_action_from_name("scratch",&action));
 assert(action==SERVICE_V2_GAIT_ACTION_SCRATCH && action==19);
}
''')
        self.assertRegex(body(PATH, "gait_task"),
                         r"case SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH:\s*run_idle_scratch\(\);")

    def test_paused_idle_scratch_keeps_its_own_recovery_across_second_interruption(self):
        source = (ROOT / PATH).read_text(encoding="utf-8")
        self.assertTrue("s_background_home_action" in source,
                        "background recovery does not retain its action source")
        run_c(COMMON + production_header() + r'''
#include <errno.h>
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_SERVO_ALL_MASK 15U
#define LISA_WAIT_FOREVER 0
typedef struct {unsigned sequence,enabled_mask,watchdog_ms;bool frame_active,sweep_active,watchdog_expired;uint16_t current_us[4],target_us[4];} service_v2_servo_status_t;
typedef struct {bool background;unsigned generation;int action;} gait_cmd_t;
static bool s_background_home_pending,s_background_command,s_worker_active,allowed;
static service_v2_gait_action_t s_background_home_action;
static service_v2_servo_status_t s_background_home_snapshot;
static unsigned s_background_generation,s_pending_commands;
static struct {bool busy;int current,cycles_done,last_error;} s_status;
static void *s_lock;
static service_v2_servo_status_t servo={.sequence=4,.enabled_mask=15};
static unsigned submits,stops,scratch_recovered,stretch_recovered,disabled;
static int recovery_error;
static int prepare_normal_face_motion(const char*s,bool*p){*p=true;return 0;}
static int recover_idle_stretch_home(void){stretch_recovered++;return recovery_error;}
static int recover_idle_scratch_home(void){scratch_recovered++;return recovery_error;}
static int recover_pushup_home(void){assert(0);return 0;}
static int recover_codex_squat_home(void){assert(0);return 0;}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void status_set(bool busy,int action,unsigned cycles,int error){s_status.current=action;}
static int service_v2_actuators_servo_frame_stop(void){stops++;servo.sequence++;servo.frame_active=false;return 0;}
static int service_v2_actuators_servo_get_status(service_v2_servo_status_t*p){*p=servo;return 0;}
static bool service_v2_background_motion_allowed(void){return allowed;}
int service_v2_gait_submit_background(service_v2_gait_action_t action,uint32_t cycles){assert(allowed && action==31);submits++;return 0;}
static void lisa_mutex_lock(void*p,int wait){}
static void lisa_mutex_unlock(void*p){}
static void background_motion_pause(void)
''' + body(PATH, "background_motion_pause") +
              "\nstatic bool background_home_owns_pose(const service_v2_servo_status_t *servo)\n" +
              body(PATH, "background_home_owns_pose") +
              "\nstatic void background_home_retry(void)\n" + body(PATH, "background_home_retry") +
              "\nstatic void command_begin(const gait_cmd_t *cmd)\n" + body(PATH, "command_begin") +
              "\nstatic void run_background_home(void)\n" + body(PATH, "run_background_home") + r'''
int main(void){
 s_status.current=SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH;background_motion_pause();
 assert(s_background_home_pending && stops==1 && s_background_home_action==35);
 for(unsigned i=0;i<10;i++)background_home_retry();
 assert(!submits && !scratch_recovered && !stretch_recovered);
 allowed=true;background_home_retry();assert(submits==1 && !s_background_home_pending);
 gait_cmd_t cmd={.background=true,.action=31};command_begin(&cmd);
 recovery_error=-ECANCELED;run_background_home();
 assert(scratch_recovered==1 && !stretch_recovered && !disabled);
 assert(s_background_home_pending && s_background_home_action==35);
 recovery_error=0;background_home_retry();command_begin(&cmd);run_background_home();
 assert(scratch_recovered==2 && !stretch_recovered && !disabled);
 /* The existing stretch/touch path keeps its original recovery. */
 s_status.current=30;background_motion_pause();command_begin(&cmd);run_background_home();
 assert(scratch_recovered==2 && stretch_recovered==1);
 /* Another owner cancels the deferred path; no deferred frame is submitted. */
 s_status.current=35;background_motion_pause();unsigned before=submits;
 cmd.background=false;command_begin(&cmd);background_home_retry();assert(submits==before);
 s_status.current=35;background_motion_pause();servo.current_us[0]++;
 background_home_retry();assert(submits==before && !s_background_home_pending);
}
''')


if __name__ == "__main__":
    unittest.main()
