"""Execute gait C against servo/IMU boundaries; no device motion or recording."""
import re
import unittest
from test_lingxin_round_races import ROOT, COMMON, body, run_c

PATH = 'apps/arcs-mini/services/service_v2_gait.c'
HEADER = (ROOT / 'apps/arcs-mini/services/service_v2_gait.h').as_posix()
BASE = COMMON + '\n#include <errno.h>\n#include "' + HEADER + '"\n' + r'''
#define GAIT_HOME_FL_US 2100U
#define GAIT_HOME_FR_US 900U
#define GAIT_HOME_RL_US 2050U
#define GAIT_HOME_RR_US 870U
#define SERVICE_V2_SERVO_COUNT 4U
#define SERVICE_V2_SERVO_EASING_SMOOTH 1
#define SERVICE_V2_SERVO_EASING_LINEAR 0
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
typedef int service_v2_servo_easing_t;
'''


class NewTricksTest(unittest.TestCase):
    def test_reverse_handoff_rejects_spikes_but_sensor_error_cannot_block_motion(self):
        run_c(BASE + r'''
#define GAIT_FLIP_FACE_REVERSE_Z_MIN 12000
#define GAIT_FLIP_ACCEL_NORM_MIN 12000
#define GAIT_FLIP_ACCEL_NORM_MAX 22000
#define GAIT_FLIP_IMU_SAMPLE_INTERVAL_MS 10
typedef struct {int16_t accel_x,accel_y,accel_z,temperature,gyro_x,gyro_y,gyro_z;} service_v2_mpu6050_raw_t;
static service_v2_mpu6050_raw_t samples[3];
static unsigned reads,delays,kicks,cancel_at;
static int sensor_error;
static bool cmd_pending(void){return cancel_at && reads>=cancel_at;}
static uint64_t square_i32(int32_t v){return (int64_t)v*v;}
static int service_v2_mpu6050_read_raw(service_v2_mpu6050_raw_t*p){assert(reads<3);*p=samples[reads++];return sensor_error;}
static int service_v2_actuators_servo_watchdog_kick(void){kicks++;return 0;}
static void lisa_thread_mdelay(unsigned ms){assert(ms==10);delays++;}
static int backward_reverse_seen(void)
''' + body(PATH, 'backward_reverse_seen') + r'''
static void reset(void){reads=delays=kicks=cancel_at=0;sensor_error=0;
 for(unsigned i=0;i<3;i++)samples[i]=(service_v2_mpu6050_raw_t){.accel_z=16384};}
int main(void){
 reset();assert(backward_reverse_seen()==1 && reads==3 && delays==2 && kicks==2);
 reset();samples[0].accel_z=-16384;assert(!backward_reverse_seen() && reads==1 && !delays);
 reset();samples[1].accel_z=-16384;assert(!backward_reverse_seen() && reads==2);
 reset();samples[0].accel_x=25000;assert(!backward_reverse_seen() && reads==1);
 reset();samples[0].accel_z=5000;assert(!backward_reverse_seen() && reads==1);
 reset();sensor_error=-EIO;assert(backward_reverse_seen()==0 && reads==1);
 reset();cancel_at=1;assert(backward_reverse_seen()==-ECANCELED && reads==1);
}
''')

    def test_backward_frame_waits_for_pwm_endpoint_and_propagates_failure(self):
        run_c(BASE + r'''
#define GAIT_FRAME_SETTLE_TIMEOUT_MS 60U
#define GAIT_TICK_MS 20U
#define SERVICE_V2_SERVO_ALL_MASK 15U
typedef struct {bool frame_active,watchdog_expired;unsigned enabled_mask;
 uint16_t current_us[4],target_us[4];} service_v2_servo_status_t;
static unsigned submitted,reads,delays;
static bool cancel,stalled,disabled;
static int frame_error;
static const uint16_t target[4]={2100,900,2200,750};
static bool cmd_pending(void){return cancel;}
static int play_frame(const uint16_t*p,unsigned ms,int easing){submitted++;return frame_error;}
static int service_v2_actuators_servo_get_status(service_v2_servo_status_t*s){
 reads++;s->frame_active=stalled || reads<3;s->enabled_mask=disabled?0:15;
 memcpy(s->target_us,target,8);memcpy(s->current_us,target,8);
 if(s->frame_active)s->current_us[2]-=10;return 0;
}
static void lisa_thread_mdelay(unsigned ms){delays++;}
static int service_v2_actuators_servo_watchdog_kick(void){return 0;}
''' + '\nstatic int wait_frame_target(const uint16_t *target_us)\n' + body(PATH, 'wait_frame_target') +
              '\nstatic int play_backward_first_frame(const uint16_t *pose,uint32_t ms,service_v2_servo_easing_t easing)\n' + body(PATH, 'play_backward_first_frame') + r'''
int main(void){
 assert(!play_backward_first_frame(target,180,0));assert(submitted==1 && reads==3 && delays==2);
 reads=delays=0;stalled=true;assert(play_backward_first_frame(target,180,0)==-ETIMEDOUT);
 assert(reads==4 && delays==3);
 stalled=false;disabled=true;assert(play_backward_first_frame(target,180,0)==-EIO);
 disabled=false;reads=0;frame_error=-EIO;assert(play_backward_first_frame(target,180,0)==-EIO && !reads);
 unsigned before=submitted;cancel=true;assert(play_backward_first_frame(target,180,0)==-ECANCELED && submitted==before);
}
''')

    def test_backflip_landing_ignores_imu_but_retains_power_and_cancel_guards(self):
        source = (ROOT / PATH).read_text(encoding='utf-8')
        defines = '\n'.join(re.findall(r'^#define GAIT_[^\n]+', source.replace('\\\n', ''), re.M))
        funcs = [('bool','gait_safety_gates_enabled','void'),
                 ('bool','cmd_pending','void'),
                 ('uint64_t','square_i32','int32_t value'),
                 ('int32_t','abs_i32','int32_t value'),
                 ('bool','flip_pose_vector_is_safe','int32_t accel_x,int32_t accel_y,int32_t accel_z,bool reverse_face'),
                 ('int','hold_pose','uint32_t duration_ms'),
                 ('int','motion_battery_check','const char *phase'),
                 ('int','roll_power_check','const char *phase'),
                 ('int','wait_for_roll_landing','bool reverse_face,const char *phase')]
        actual = '\n'.join('static %s %s(%s)\n%s' % (kind,name,args,body(PATH,name)) for kind,name,args in funcs)
        run_c(BASE + defines + r'''
#define LISA_LOGW(...) ((void)0)
typedef enum {ROLL_FACE_NORMAL,ROLL_FACE_REVERSE} roll_face_t;
typedef struct {int16_t accel_x,accel_y,accel_z,temperature,gyro_x,gyro_y,gyro_z;} service_v2_mpu6050_raw_t;
static struct {bool busy;int current;} s_status={true,SERVICE_V2_GAIT_ACTION_BACKFLIP};
static bool s_background_command,s_wake_resetting,usb;
static unsigned s_background_generation,reads,inits,ticks,plug_at;
static uint16_t voltage_mv=4000;
static int sensor_error;
static void *s_queue;
static service_v2_mpu6050_raw_t sample={.accel_z=-16384};
static unsigned service_offline_action_generation(void){return 0;}
static bool service_v2_background_motion_allowed(void){return true;}
static unsigned uxQueueMessagesWaiting(void*q){return 0;}
static bool power_is_usb_plugged(void){return usb;}
static uint16_t battery_get_voltage_mv(void){return voltage_mv;}
static int service_v2_mpu6050_init(bool force){inits++;return sensor_error;}
static int service_v2_mpu6050_read_raw(service_v2_mpu6050_raw_t*p){*p=sample;reads++;return sensor_error;}
static int service_v2_actuators_servo_watchdog_kick(void){return 0;}
static void lisa_thread_mdelay(unsigned ms){ticks++;if(plug_at && ticks==plug_at)usb=true;}
''' + actual + r'''
int main(void){
 assert(gait_safety_gates_enabled());
 /* Real board gravity is below the old absolute threshold; an absent IMU
  * must also leave the timed sequence executable. Neither path reads IMU. */
 sample=(service_v2_mpu6050_raw_t){.accel_x=972,.accel_y=-64,.accel_z=-11712};
 assert(!wait_for_roll_landing(false,"weak gravity") && !reads && !inits && ticks==5);
 sensor_error=-EIO;ticks=0;
 assert(!wait_for_roll_landing(true,"missing IMU") && !reads && !inits && ticks==5);
 assert(!roll_power_check("charged"));
 voltage_mv=3299;assert(roll_power_check("low")==-EAGAIN);
 voltage_mv=3300;assert(!roll_power_check("boundary"));
 usb=true;assert(roll_power_check("plugged")==-EBUSY);
 usb=false;reads=ticks=0;plug_at=2;
 assert(wait_for_roll_landing(false,"USB inserted")==-ECANCELED && ticks==2 && !reads);
 usb=false;plug_at=0;s_wake_resetting=true;assert(cmd_pending());
 assert(wait_for_roll_landing(false,"wake cancelled")==-ECANCELED);
 s_wake_resetting=false;usb=true;assert(cmd_pending());
 /* Ordinary rolls retain their existing IMU landing policy. */
 s_status.current=SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP;
 assert(!gait_safety_gates_enabled() && !cmd_pending());
 usb=false;sensor_error=0;sample=(service_v2_mpu6050_raw_t){.accel_z=-16384};reads=0;
 assert(!wait_for_roll_landing(false,"normal roll") && reads==5);
 assert(wait_for_roll_landing(true,"wrong face") == -ETIMEDOUT);
 sensor_error=-EIO;assert(wait_for_roll_landing(false,"ordinary IMU failure")==-EIO);
}
''')

    def source(self):
        source = (ROOT / PATH).read_text(encoding='utf-8')
        self.assertTrue('static void run_pushup(' in source, 'pushup runner is missing')
        self.assertTrue('static int backflip_frame(' in source, 'backward trajectory is missing')
        return source

    def test_pushup_limits_and_slow_support_then_home(self):
        source = self.source()
        arrays = '\n'.join(re.findall(r'static const uint16_t (?:s_pushup_\w+|s_pose_stand)\[SERVICE_V2_SERVO_COUNT\] = \{[^}]+\};', source))
        run_c(BASE + arrays + r'''
static unsigned n,done,disabled,fail_at;
static bool s_background_command;
static int last_error,frame_error,prep_error;
static uint16_t frames[32][4];
static int prepare_normal_face_motion(const char*p,bool*started){*started=prep_error==0;return prep_error;}
static int hold_pose(unsigned ms){return 0;}
static int play_frame_settled(const uint16_t*p,unsigned ms,int easing){
 assert(n<32 && ms>=500);memcpy(frames[n++],p,8);return n==fail_at?frame_error:0;
}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void background_motion_pause(void){assert(0);}
static void status_set(bool active,int action,unsigned count,int error){assert(action==SERVICE_V2_GAIT_ACTION_PUSHUP);done=count;last_error=error;}
static int normalize_command(service_v2_gait_action_t action,uint32_t *cycles)
''' + body(PATH, 'normalize_command') + '\nstatic int recover_pushup_home(void)\n' + body(PATH, 'recover_pushup_home') + '\nstatic void run_pushup(uint32_t cycles)\n' + body(PATH, 'run_pushup') + r'''
int main(void){
 for(unsigned reps=3;reps<=5;reps++){
  n=done=disabled=0;unsigned c=reps;assert(normalize_command(SERVICE_V2_GAIT_ACTION_PUSHUP,&c)==0);
  run_pushup(c);assert(done==reps && n==4+2*reps && !disabled && !last_error);
  assert(frames[0][0]==2100 && frames[0][1]==900);
  for(unsigned i=0;i<n-1;i++)assert(frames[i][2]==1615 && frames[i][3]==1315);
  assert(frames[1][0]>2100 && frames[1][1]<900);
  for(unsigned i=2;i<2+reps*2;i+=2){
   assert(frames[i][0]>frames[i+1][0] && frames[i][1]<frames[i+1][1]);
  }
  assert(frames[n-2][0]==2100 && frames[n-2][1]==900);
  assert(!memcmp(frames[n-1],s_pose_stand,8));
 }
 unsigned c=0;assert(!normalize_command(SERVICE_V2_GAIT_ACTION_PUSHUP,&c) && c==4);
 c=2;assert(normalize_command(SERVICE_V2_GAIT_ACTION_PUSHUP,&c)==-EINVAL);
 c=6;assert(normalize_command(SERVICE_V2_GAIT_ACTION_PUSHUP,&c)==-EINVAL);
 c=0;assert(!normalize_command(SERVICE_V2_GAIT_ACTION_BACKFLIP,&c) && c==1);
 c=2;assert(normalize_command(SERVICE_V2_GAIT_ACTION_BACKFLIP,&c)==-EINVAL);
 n=done=disabled=0;fail_at=3;frame_error=-ECANCELED;run_pushup(4);
 assert(n==3 && done==0 && disabled==1);
 n=disabled=0;frame_error=-EIO;run_pushup(4);assert(n==3 && disabled==1 && last_error==-EIO);
 n=disabled=0;prep_error=-EAGAIN;run_pushup(4);assert(!n && !disabled && last_error==-EAGAIN);
}
''')

    def test_backflip_mirrors_about_calibrated_home_and_checks_range(self):
        source = self.source()
        arrays = '\n'.join(re.findall(r'static const uint16_t (?:s_pose_stand|s_flip_front_retract)\[SERVICE_V2_SERVO_COUNT\] = \{[^}]+\};', source))
        run_c(BASE + arrays + '\nstatic int backflip_frame(const uint16_t *source,uint16_t *target)\n' + body(PATH, 'backflip_frame') + r'''
int main(void){
 uint16_t actual[4],back[4];
 assert(!backflip_frame(s_pose_stand,actual) && !memcmp(actual,s_pose_stand,8));
 assert(!backflip_frame(s_flip_front_retract,actual));
 const uint16_t expected[4]={2100,900,2250,670};
 assert(!memcmp(actual,expected,8));
 assert(!backflip_frame(actual,back) && !memcmp(back,s_flip_front_retract,8));
 const uint16_t invalid[4]={4000,900,2050,870};
 assert(backflip_frame(invalid,actual)==-ERANGE);
}
''')

    def test_full_backward_roll_keeps_trajectory_without_start_imu_dependency(self):
        from test_service_v2_gait_contract import GaitSource, function_body
        gait = GaitSource()
        funcs = [('int', 'backflip_frame', 'const uint16_t *source,uint16_t *target'),
                 ('int', 'play_roll_frame', 'const uint16_t *pulse_us,uint32_t duration_ms,service_v2_servo_easing_t easing'),
                 ('int', 'run_reverse_half_roll', 'void'),
                 ('int', 'play_backward_first_frame', 'const uint16_t *pose,uint32_t ms,service_v2_servo_easing_t easing'),
                 ('int', 'run_backward_first_half', 'void'),
                 ('int', 'run_normal_half_roll', 'bool prepare_reverse_home'),
                 ('void', 'run_roll', 'service_v2_gait_action_t action,uint32_t half_turns')]
        bodies = '\n'.join('static %s %s(%s){\n%s\n}' %
                           (kind, name, args, function_body(gait.source, name, kind))
                           for kind, name, args in funcs)
        names = set(re.findall(r'\bs_\w+\b', bodies)) - {'s_status'}
        arrays = '\n'.join('static const uint16_t %s[4]={%s};' %
                           (name, ','.join(map(str, gait.frame(name)))) for name in sorted(names))
        defines = '\n'.join(f'#define {name} {value}U' for name, value in gait.defines.items()
                            if name.startswith('GAIT_') and not name.startswith('GAIT_HOME_'))
        defines += '\n' + '\n'.join(re.findall(r'^#define GAIT_FLIP_AFTER_.*$', gait.source.replace('\\\n', ''), re.M))
        run_c(BASE + defines + '\n' + arrays + r'''
typedef enum {ROLL_FACE_NORMAL,ROLL_FACE_REVERSE} roll_face_t;
static struct {bool busy;service_v2_gait_action_t current;} s_status;
static bool usb,cancel;
static int battery_error,landing_error,frame_error,last_error;
static unsigned frames,landings,completed,disabled,synced,fail_at,face_reads;
static int start_sensor_error;
static uint16_t final_pose[4], first_frames[8][4];
static unsigned first_durations[8];
static unsigned reverse_checks,reverse_after,first_landing_frames;
static unsigned kick_holds;
static unsigned cancel_kick_hold;
static int reverse_error;
static int backward_reverse_seen(void){reverse_checks++;return reverse_error?reverse_error:(reverse_after && frames>=reverse_after);}
static roll_face_t initial_face;
static bool cmd_pending(void){return cancel || usb;}
static bool power_is_usb_plugged(void){return usb;}
static int motion_battery_check(const char*p){return battery_error;}
#define LISA_LOGW(...) ((void)0)
static bool gait_safety_gates_enabled(void)
''' + body(PATH, 'gait_safety_gates_enabled') + '\nstatic int roll_power_check(const char *phase)\n' + body(PATH, 'roll_power_check') + r'''
static int detect_roll_start_face(roll_face_t*f){face_reads++;*f=initial_face;return start_sensor_error;}
static int sync_roll_home_from_disabled(roll_face_t f){synced++;return 0;}
static int hold_pose(unsigned ms){
 if(ms==240){kick_holds++;if(cancel_kick_hold==kick_holds)cancel=true;}
 return cancel?-ECANCELED:0;
}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static int play_frame(const uint16_t*p,unsigned ms,int easing){
 for(int i=0;i<4;i++)assert(p[i]>=650 && p[i]<=2350);
 if(frames<8){memcpy(first_frames[frames],p,8);first_durations[frames]=ms;}
 memcpy(final_pose,p,8);frames++;return frames==fail_at?frame_error:0;
}
static int wait_frame_target(const uint16_t*p){return 0;}
static void status_set(bool busy,service_v2_gait_action_t action,unsigned done,int error){
 s_status.busy=busy;s_status.current=action;completed=done;last_error=error;
}
static int wait_for_roll_landing(bool reverse,const char*p){
 /* Two settled checks per half: landing, then restored supporting legs. */
 assert(reverse==(landings<2));if(!landings)first_landing_frames=frames;landings++;return landing_error;
}
''' + bodies + r'''
static void reset(void){frames=landings=completed=disabled=synced=fail_at=face_reads=0;start_sensor_error=0;
 usb=cancel=false;battery_error=landing_error=frame_error=last_error=0;initial_face=ROLL_FACE_NORMAL;
 reverse_checks=reverse_after=first_landing_frames=kick_holds=0;reverse_error=0;cancel_kick_hold=0;}
int main(void){
 reset();cancel=true;assert(run_backward_first_half()==-ECANCELED && frames==0);
 reset();run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(completed==2 && landings==4 && !disabled && !last_error && frames>=14 && !face_reads);
 /* Use the bench-confirmed head-up support BEFORE moving the front pair. */
 const uint16_t supported[4]={2100,900,2200,750};
 const uint16_t front_lift[4]={2200,800,2200,750};
 assert(!memcmp(first_frames[0],supported,8));
 assert(!memcmp(first_frames[1],front_lift,8));
 /* A complete bounded drive sequence must precede a blind landing wait. */
 assert(first_landing_frames==8 && reverse_checks==9 && kick_holds==3);
 /* Restore the field-confirmed hop-then-flip trajectory, including timing. */
 for(unsigned i=2;i<8;i+=2){
  assert(first_frames[i][0]==2200 && first_frames[i][1]==800);
  assert(first_frames[i+1][0]==2200 && first_frames[i+1][1]==800);
  assert(first_frames[i][2]==2250 && first_frames[i][3]==670);
  assert(first_frames[i+1][2]==1615 && first_frames[i+1][3]==1315);
  assert(first_durations[i]==320 && first_durations[i+1]==100);
 }
 assert(first_durations[0]>=700);
 assert(!memcmp(final_pose,s_pose_stand,8));
 reset();reverse_after=4;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(first_landing_frames==4 && reverse_checks==3 && completed==2 && landings==4 && !last_error);
 reset();reverse_after=5;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(first_landing_frames==5 && reverse_checks==5 && completed==2 && !last_error);
 /* Orientation can change during preparation/preload, not just the kick. */
 reset();reverse_after=2;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(first_landing_frames==2 && completed==2 && !last_error);
 reset();reverse_after=3;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(first_landing_frames==3 && completed==2 && !last_error);
 reset();reverse_after=6;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(first_landing_frames==6 && completed==2 && !last_error);
 reset();reverse_error=-ECANCELED;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(!landings && !completed && disabled==1 && !last_error);
 reset();fail_at=4;frame_error=-ECANCELED;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(frames==4 && !landings && !completed && disabled==1);
 reset();fail_at=5;frame_error=-EIO;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(frames==5 && !landings && !completed && disabled==1 && last_error==-EIO);
 reset();cancel_kick_hold=1;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(frames==4 && kick_holds==1 && !landings && !completed && disabled==1 && !last_error);
 reset();cancel_kick_hold=2;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(frames==6 && kick_holds==2 && !landings && !completed && disabled==1 && !last_error);
 reset();start_sensor_error=-EIO;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(completed==2 && synced==1 && !last_error && !face_reads);
 reset();usb=true;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(!frames && !synced && last_error==-EBUSY);
 reset();battery_error=-EAGAIN;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(!frames && !synced && last_error==-EAGAIN);
 reset();landing_error=-ECANCELED;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(landings==1 && completed==0 && disabled==1 && !last_error);
 reset();fail_at=3;frame_error=-ECANCELED;run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,2);
 assert(frames==3 && !landings && !completed && disabled==1);
 reset();run_roll(SERVICE_V2_GAIT_ACTION_BACKFLIP,1);assert(!frames && !synced && last_error==-EINVAL);
}
''')


if __name__ == '__main__':
    unittest.main()
