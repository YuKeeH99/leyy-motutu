"""Execute the production roll sequence with recorded servo frames and IMU stubs."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_service_v2_gait_contract import GaitSource, function_body
from test_v2_rgb_effect_contract import CC, RUN


class RollLandingTest(unittest.TestCase):
    def test_final_reverse_landing_skips_support_but_continuation_keeps_it(self):
        gait = GaitSource()
        normal = function_body(gait.source, 'run_normal_half_roll', 'int')
        roll = function_body(gait.source, 'run_roll')
        signature = re.search(r'static int run_normal_half_roll\([^)]*\)', gait.source).group()
        names = set(re.findall(r'\bs_[a-zA-Z_]+\b', normal + roll))
        names.update(('s_pose_reverse_home', 's_roll_recover_reverse_front'))
        arrays = '\n'.join('static const uint16_t %s[4]={%s};' %
                           (name, ','.join(map(str, gait.frame(name)))) for name in sorted(names))
        defines = '\n'.join(f'#define {name} {value}U' for name, value in gait.defines.items())
        defines += '\n' + '\n'.join(re.findall(
            r'^#define GAIT_FLIP_AFTER_.*$', gait.source.replace('\\\n', ''), re.M))
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define SERVICE_V2_SERVO_EASING_SMOOTH 1
#define SERVICE_V2_SERVO_EASING_LINEAR 0
#define SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP 1
#define SERVICE_V2_GAIT_ACTION_BACKFLIP 33
#define play_roll_frame play_frame
typedef int service_v2_gait_action_t;
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
typedef enum {ROLL_FACE_NORMAL, ROLL_FACE_REVERSE} roll_face_t;
''' + defines + '\n' + arrays + r'''
static const uint16_t *frames[256];
static unsigned count, reverse_runs, landing_calls, disabled, completed;
static int landing_error, status_error;
static bool cancel_feet_up;
static roll_face_t initial_face;
static void status_set(bool active, int action, uint32_t done, int error) {
    if (!active) { completed=done; status_error=error; }
}
static int roll_power_check(const char *phase) {return 0;}
static int detect_roll_start_face(roll_face_t *face) {*face=initial_face;return 0;}
static int sync_roll_home_from_disabled(roll_face_t face) {return 0;}
static int hold_pose(unsigned ms) {return 0;}
static int service_v2_actuators_servo_disable_all(void) {++disabled;return 0;}
static int play_frame(const uint16_t *pose, unsigned ms, int easing) {
    assert(count<256); frames[count++]=pose;
    if (cancel_feet_up && landing_calls && pose==s_pose_stand) return -ECANCELED;
    return 0;
}
static int wait_for_roll_landing(bool reverse, const char *phase) {
    ++landing_calls; return landing_error;
}
static int run_reverse_half_roll(void) {++reverse_runs;return 0;}
static int run_backward_first_half(void) {assert(0);return -EINVAL;}
''' + signature + '{\n' + normal + '\n}\nstatic void run_roll(service_v2_gait_action_t action,uint32_t half_turns){\n' + roll + r'''
}
static unsigned occurrences(const uint16_t *pose) {
    unsigned n=0;for(unsigned i=0;i<count;++i)if(frames[i]==pose)++n;return n;
}
static void reset(roll_face_t face) {
    count=reverse_runs=landing_calls=disabled=completed=0;
    landing_error=status_error=0; cancel_feet_up=false; initial_face=face;
}
static void expect_final_feet_up(unsigned halves, roll_face_t start, unsigned intermediate) {
    reset(start); run_roll(SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,halves);
    assert(status_error==0 && disabled==0 && completed==halves);
    assert(occurrences(s_pose_reverse_home)==intermediate);
    assert(occurrences(s_roll_recover_reverse_front)==intermediate);
    assert(occurrences(s_upside_feedback_wiggle_a)==SERVICE_V2_GAIT_UPSIDE_WIGGLE_CYCLES);
    assert(occurrences(s_upside_feedback_wiggle_b)==SERVICE_V2_GAIT_UPSIDE_WIGGLE_CYCLES);
    unsigned last_cross=0;
    for(unsigned i=0;i<count;++i)if(frames[i]==s_roll_normal_cross)last_cross=i;
    assert(frames[last_cross+1]==s_pose_stand);
    assert(frames[count-1]==s_pose_stand);
    /* Each normal landing is checked; intermediate support is checked again. */
    assert(landing_calls==intermediate*2+1);
}
int main(void) {
    expect_final_feet_up(1,ROLL_FACE_NORMAL,0);
    expect_final_feet_up(3,ROLL_FACE_NORMAL,1);
    expect_final_feet_up(2,ROLL_FACE_REVERSE,0);
    reset(ROLL_FACE_NORMAL);run_roll(SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,2);
    assert(completed==2 && reverse_runs==1 && disabled==0);
    assert(occurrences(s_pose_reverse_home)==1);
    assert(occurrences(s_roll_recover_reverse_front)==1);
    assert(occurrences(s_upside_feedback_wiggle_a)==0);
    reset(ROLL_FACE_REVERSE);run_roll(SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,1);
    assert(completed==1 && reverse_runs==1 && count==0);
    reset(ROLL_FACE_NORMAL);landing_error=-ETIMEDOUT;run_roll(SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,1);
    assert(disabled==1 && completed==0 && status_error==-ETIMEDOUT);
    assert(frames[count-1]==s_roll_normal_cross);
    reset(ROLL_FACE_NORMAL);cancel_feet_up=true;run_roll(SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,1);
    assert(disabled==1 && occurrences(s_upside_feedback_wiggle_a)==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, elf = Path(tmp) / 'roll.c', Path(tmp) / 'roll.elf'
            source.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(source), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)


if __name__ == '__main__':
    unittest.main()
