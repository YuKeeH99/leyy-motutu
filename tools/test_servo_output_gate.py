"""Execute wake cancellation and ordinary PWM control after latch rollback."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = Path(os.environ.get("NUCLEI_TOOLCHAIN_PATH", Path.home() / ".listenai/gcc")) / "bin"
SUFFIX = ".exe" if os.name == "nt" else ""


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;]+?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(f"missing production function: {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class ServoOutputGateTest(unittest.TestCase):
    def test_network_resume_wait_cannot_issue_new_wakeup(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby.c").read_text(encoding="utf-8")
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#define SERVICE_V2_VOICE_STANDBY_ACTIVE 0
#define STANDBY_POLL_MS 100
#define pdMS_TO_TICKS(x) (x)
static void *s_standby_task = (void *)1;
static unsigned wakes, now;
static int state;
static void service_v2_voice_standby_note_activity(void) {wakes++;}
static unsigned standby_now_ms(void) {return now;}
static int service_v2_voice_standby_get_state(void) {return state;}
static bool sys_wifi_radio_suspended(void) {return state != 0;}
static void vTaskDelay(unsigned ms) {now += ms;}
''' + function(source, "service_v2_voice_standby_resume_wait") + r'''
int main(void) {
    assert(service_v2_voice_standby_resume_wait(100) == 0);
    state = 1;
    assert(service_v2_voice_standby_resume_wait(200) == -ETIMEDOUT);
    assert(wakes == 0);
    return 0;
}
'''
        self.run_c(code)

    def test_raw_unvalidated_wake_event_cannot_resume_sleep(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby.c").read_text(encoding="utf-8")
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define VOICE_MSG_WAKEUP_KEYWORD 1
#define VOICE_MSG_WAKEUP_BUTTON_START 2
#define VOICE_MSG_PLAYER_MUSIC_PLAYING 3
#define VOICE_MSG_PLAYER_MUSIC_STOPPED 4
#define VOICE_MSG_V2_GAIT_ACTION_STARTED 5
#define VOICE_MSG_V2_GAIT_ACTION_FINISHED 6
#define STANDBY_NOTIFY_ACTIVITY 1
#define STANDBY_NOTIFY_WAKE 32
#define STANDBY_NOTIFY_MUSIC_START 4
#define STANDBY_NOTIFY_MUSIC_STOP 8
#define STANDBY_NOTIFY_GAIT 16
#define eSetBits 0
typedef struct {int action,result;_Bool background;} service_v2_gait_action_finished_t;
static void *s_standby_task = (void *)1;
static unsigned bits_sent;
static int xTaskNotify(void *t,unsigned bits,int mode) {bits_sent = bits;return 0;}
''' + function(source, "standby_on_voice") + r'''
int main(void) {
    standby_on_voice(NULL, VOICE_MSG_WAKEUP_KEYWORD, NULL, 0, NULL);
    assert((bits_sent & STANDBY_NOTIFY_WAKE) == 0);
    standby_on_voice(NULL, VOICE_MSG_WAKEUP_BUTTON_START, NULL, 0, NULL);
    assert((bits_sent & STANDBY_NOTIFY_WAKE) == 0);
    service_v2_gait_action_finished_t stretch = {.background=1};
    bits_sent=0;
    standby_on_voice(NULL, VOICE_MSG_V2_GAIT_ACTION_FINISHED, &stretch, sizeof(stretch), NULL);
    assert(bits_sent==0);
    return 0;
}
'''
        self.run_c(code)

    def test_late_music_and_action_notifications_do_not_wake_sleep(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby.c").read_text(encoding="utf-8")
        policy = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby_policy.c").as_posix()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
''' + '#include "' + policy + '"\n' + r'''
#define STANDBY_NOTIFY_ACTIVITY 1U
#define STANDBY_NOTIFY_MANUAL 2U
#define STANDBY_NOTIFY_MUSIC_START 4U
#define STANDBY_NOTIFY_MUSIC_STOP 8U
#define STANDBY_NOTIFY_GAIT 16U
#define STANDBY_NOTIFY_WAKE 32U
#define LISA_LOGI(...) ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static service_v2_voice_standby_policy_t s_policy;
static bool s_music_active, s_entry_gait_owned;
static bool s_manual_requested;
static bool s_wake_pending;
static uint32_t s_sleep_started_ms;
static unsigned resumes;
static uint32_t standby_now_ms(void) {return 1234;}
static void standby_publish_state(void) {}
static bool sys_wifi_radio_suspended(void) {return false;}
static int sys_wifi_resume_radio(void) {return 0;}
static void standby_finish_resume(bool pose) {
    resumes++;
    service_v2_voice_standby_policy_mark_resumed(&s_policy, 1234);
}
''' + function(source, "standby_process_notifications") + r'''
int main(void) {
    service_v2_voice_standby_policy_init(&s_policy, 0, 300000);
    s_policy.state = SERVICE_V2_VOICE_STANDBY_ASLEEP;
    standby_process_notifications(STANDBY_NOTIFY_MUSIC_STOP | STANDBY_NOTIFY_GAIT | STANDBY_NOTIFY_ACTIVITY);
    assert(resumes == 0 && s_policy.state == SERVICE_V2_VOICE_STANDBY_ASLEEP);
    standby_process_notifications(STANDBY_NOTIFY_WAKE);
    assert(resumes == 1 && s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE);
    s_policy.state = SERVICE_V2_VOICE_STANDBY_ENTERING;
    standby_process_notifications(STANDBY_NOTIFY_ACTIVITY);
    assert(!s_policy.resume_after_enter);
    standby_process_notifications(STANDBY_NOTIFY_WAKE);
    assert(s_policy.resume_after_enter);
    service_v2_voice_standby_policy_mark_resumed(&s_policy, 2000);
    service_v2_voice_standby_policy_request_manual(&s_policy);
    standby_process_notifications(STANDBY_NOTIFY_WAKE);
    assert(s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE);
    /* Notification bits can coalesce: last local command decides. */
    s_manual_requested = false;
    standby_process_notifications(STANDBY_NOTIFY_WAKE | STANDBY_NOTIFY_MANUAL);
    assert(s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE);
    s_manual_requested = true;
    standby_process_notifications(STANDBY_NOTIFY_WAKE | STANDBY_NOTIFY_MANUAL);
    assert(s_policy.state == SERVICE_V2_VOICE_STANDBY_ENTER_PENDING);
    return 0;
}
'''
        self.run_c(code)

    def test_resuming_peripherals_does_not_implicitly_move_legs(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_voice_standby.c").read_text(encoding="utf-8")
        code = r'''
#include <assert.h>
#include <stdbool.h>
#define CONFIG_APP_V2_NFC_PEER_ENABLE 0
#define SERVICE_V2_SENSORS_OK 0
#define SERVICE_V2_SENSORS_ERR_NOT_READY -1
#define STANDBY_MPU_WAKE_SETTLE_MS 200
#define SERVICE_V2_GAIT_ACTION_STAND 1
#define pdMS_TO_TICKS(x) (x)
#define LISA_LOGW(...) ((void)0)
static bool s_nfc_was_enabled, s_entry_gait_owned;
static unsigned step, motions;
static int service_v2_mpu6050_set_sleep(bool b) {assert(!b); assert(++step == 1);return 0;}
static void vTaskDelay(unsigned t) {assert(t == 200); assert(++step == 2);}
static int service_v2_touch_action_set_enabled(int n) {assert(n==1); assert(++step==3);return 0;}
static int service_v2_imu_interaction_set_enabled(int n) {assert(n==1); assert(++step==4);return 0;}
static int service_v2_rgb_effect_set_enabled(int n) {assert(n==1); assert(++step==5);return 0;}
static int service_v2_gait_submit(int a,unsigned n) {motions++;return 0;}
''' + function(source, "standby_resume_peripherals") + r'''
int main(void) {
    standby_resume_peripherals(true);
    assert(step == 5 && motions == 0);
    return 0;
}
'''
        self.run_c(code)

    def test_wake_drains_old_work_before_starting_new_pose(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_gait.c").read_text(encoding="utf-8")
        body = function(source, "service_v2_gait_wake_pose")
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define pdTRUE 1
#define LISA_WAIT_FOREVER 0
#define GAIT_TICK_MS 20
#define SERVICE_V2_GAIT_ACTION_STAND 1
#define LISA_LOGI(...) ((void)0)
#define SERVICE_V2_GAIT_ACTION_ASSEMBLY 2
typedef int service_v2_gait_action_t;
typedef struct {int action; unsigned cycles; unsigned speed_percent;} gait_cmd_t;
typedef struct {bool sweep_active;} service_v2_servo_status_t;
static bool s_ready = true, s_wake_resetting, s_wake_standing, s_worker_active;
static bool s_boot_pose_authorized;
static bool s_roll_reserved, s_normal_face_motion_reserved, s_normal_face_motion_pose_validated;
static unsigned s_pending_commands, s_normal_face_motion_terminal_pending;
static int s_lock, s_queue, queued, last_action;
static bool stuck;
static unsigned waited;
static unsigned disables;
static int service_v2_actuators_servo_sweep_stop(void) {return 0;}
static int service_v2_actuators_servo_disable_all(void) {disables++;return 0;}
static void lisa_mutex_lock(int l,int t) {}
static void lisa_mutex_unlock(int l) {}
static int service_v2_actuators_servo_get_status(service_v2_servo_status_t *s) {s->sweep_active=false; return 0;}
static int xQueueReceive(int q, gait_cmd_t *c,int t) {if(queued) {queued--; return 1;} return 0;}
static int xQueueSend(int q,gait_cmd_t *c,int t) {
    assert(!s_worker_active && s_pending_commands == 0);
    last_action=c->action; queued++; return 1;
}
static void lisa_thread_mdelay(unsigned ms) {
    assert(s_wake_resetting); waited += ms;
    if (!stuck) s_worker_active = false;
}
''' + body + r'''
int main(void) {
    queued = 2; s_pending_commands = 2; s_worker_active = true;
    assert(service_v2_gait_wake_pose(1, 100) == 0);
    assert(waited == 20 && disables == 1 && s_boot_pose_authorized);
    assert(queued == 1 && s_pending_commands == 1 && last_action == 1);
    assert(s_wake_standing && !s_wake_resetting);
    /* A new accepted wake replaces the pending stand, even when offline. */
    assert(service_v2_gait_wake_pose(2, 100) == 0);
    assert(queued == 1 && s_pending_commands == 1 && last_action == 2);
    /* A worker that cannot acknowledge cancellation never queues a new pose. */
    s_wake_standing=false; queued=0; s_pending_commands=0;
    s_worker_active=true; stuck=true; waited=0;
    assert(service_v2_gait_wake_pose(1, 100) == -ETIMEDOUT);
    assert(disables >= 3 && queued == 0 && !s_wake_resetting);
    return 0;
}
'''
        self.run_c(code)

    def run_c(self, code):
        with tempfile.TemporaryDirectory() as directory:
            test_c = Path(directory) / "gate.c"
            test_c.write_text(code, encoding="utf-8")
            elf = Path(directory) / "gate.elf"
            subprocess.run([str(TOOLCHAIN / f"riscv64-unknown-elf-gcc{SUFFIX}"),
                            "--specs=sim.specs", "-std=c11", str(test_c), "-o", str(elf)], check=True)
            result = subprocess.run([str(TOOLCHAIN / f"riscv64-unknown-elf-run{SUFFIX}"), str(elf)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_ordinary_pwm_enable_has_no_persistent_latch_or_pad_override(self):
        source = (ROOT / "apps/arcs-mini/services/service_v2_actuators.c").read_text(encoding="utf-8")
        body = "\n".join(function(source, name) for name in
                         ("servo_enable_mask_locked", "servo_enable_locked"))
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define SERVICE_V2_SERVO_COUNT 4
#define CSK_DRIVER_OK 0
#define CSK_GPT_PWM_MODE 0
#define CSK_GPT_PWM_CLKSRC_PCLK 0
#define CSK_GPT_PWM_OUTMODE_EDGE_ALIGNED 0
#define CSK_GPT_PWM_OUTPOLARITY_HIGH 0
#define CSK_GPT_PWM_OPERATION_MODE_PWM 0
#define ACTUATOR_PAD 0
#define ACTUATOR_PWM_FUNCTION 12
#define HAL_IOMUX_FORCE_OFF 0
typedef struct { bool enabled; uint8_t pin, channel; } servo_state_t;
static servo_state_t s_servos[4];
static unsigned enables, mux_writes, force_writes;
static int s_gpt, s_servo_divider_control;
static int HAL_GPT_PWMControl(int g, unsigned c, unsigned ch) {return 0;}
static int HAL_GPT_EnablePWM(int g, unsigned ch) {enables++; return 0;}
static int HAL_GPT_DisablePWM(int g, unsigned ch) {return 0;}
static int IOMuxManager_PinConfigure(int p, unsigned pin, int f) {mux_writes++; return 0;}
static int IOMuxManager_PinForce(int p, unsigned pin, int f) {force_writes++; return 0;}
static int actuator_pin_gpio_low(unsigned pin) {return 0;}
static int servo_apply_mask_locked(uint8_t m) {return 0;}
static int servo_apply_locked(servo_state_t *s) {return 0;}
''' + body + r'''
int main(void) {
    assert(servo_enable_locked(&s_servos[0]) == 0);
    assert(enables == 1);
    assert(servo_enable_mask_locked(15) == 0);
    assert(enables == 4);
    assert(servo_enable_locked(&s_servos[0]) == 0);
    assert(servo_enable_mask_locked(15) == 0);
    assert(enables == 4);
    assert(force_writes == 0);
    return 0;
}
'''
        self.run_c(code)


if __name__ == "__main__":
    unittest.main()
