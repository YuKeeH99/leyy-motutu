#include "service_v2_touch_action.h"
#include "service_v2_idle_action.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "battery/battery.h"
#include "lisa_log.h"
#include "lisa_thread.h"
#include "power/power_manager.h"

#include "service_v2_actuators.h"
#include "service_v2_gait.h"
#include "service_v2_imu_interaction.h"
#include "service_v2_sensors.h"
#include "service_v2_touch_action_policy.h"

#define TAG "v2.touch_action"

#define TOUCH_ACTION_POLL_MS               50U
#define TOUCH_ACTION_STABLE_SAMPLES         3U
#define TOUCH_ACTION_ARMING_STABLE_MS     1000U
#define TOUCH_ACTION_COOLDOWN_MS         1000U
#define TOUCH_ACTION_MIN_BATTERY_MV      3300U
#define TOUCH_ACTION_ERROR_LOG_INTERVAL_MS 5000U
#define TOUCH_ACTION_TASK_STACK          2048U
#define TOUCH_ACTION_HOME_TOLERANCE_US      40U

/* 新触摸动作只从正常面 HOME 起步；反面 HOME 仍只属于 IMU 翻滚。 */
static const uint16_t s_touch_action_normal_home_us[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2050U, 870U};

static lisa_thread_t *s_touch_action_thread;
static bool s_touch_action_enabled = true;
static bool s_touch_action_reset_requested;
static bool s_error_log_valid;
static TickType_t s_last_error_log_tick;

typedef struct {
    const char *pin_name;
    service_v2_gait_action_t action;
    bool candidate_level;
    uint8_t candidate_samples;
    bool stable_level;
    bool stable_valid;
    bool armed;
    TickType_t arming_since_tick;
    bool cooldown_valid;
    TickType_t last_trigger_tick;
} touch_channel_state_t;

static touch_channel_state_t s_pa09 = {
    .pin_name = "PA09",
    .action = SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE,
};
static touch_channel_state_t s_pa13 = {
    .pin_name = "PA13",
    .action = SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK,
};

static void touch_action_reset_baselines(void)
{
    s_pa09.candidate_samples = 0U;
    s_pa09.stable_valid = false;
    s_pa09.armed = false;
    s_pa13.candidate_samples = 0U;
    s_pa13.stable_valid = false;
    s_pa13.armed = false;
}

static bool touch_action_error_log_due(void)
{
    TickType_t now = xTaskGetTickCount();

    if (!s_error_log_valid ||
        (now - s_last_error_log_tick) >= pdMS_TO_TICKS(TOUCH_ACTION_ERROR_LOG_INTERVAL_MS)) {
        s_error_log_valid = true;
        s_last_error_log_tick = now;
        return true;
    }

    return false;
}

static bool touch_action_pulse_near(uint16_t actual_us, uint16_t expected_us)
{
    uint16_t delta_us = actual_us > expected_us ? actual_us - expected_us : expected_us - actual_us;

    return delta_us <= TOUCH_ACTION_HOME_TOLERANCE_US;
}

static bool touch_action_servo_near_pose(const service_v2_servo_status_t *servo, const uint16_t *pose)
{
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if (!touch_action_pulse_near(servo->current_us[i], pose[i]) ||
            !touch_action_pulse_near(servo->target_us[i], pose[i])) {
            return false;
        }
    }
    return true;
}

static int touch_action_require_idle_home(void)
{
    service_v2_servo_status_t servo = {0};
    int ret = service_v2_actuators_servo_get_status(&servo);

    if (ret != 0) {
        return ret;
    }

    /* A non-zero watchdog belongs to host/direct control.  Reject even when
     * no interpolated frame is active so a touch cannot race Motion Studio. */
    if (servo.frame_active || servo.sweep_active || servo.watchdog_ms != 0U) {
        return -EBUSY;
    }
    if (servo.watchdog_expired) {
        return -EACCES;
    }

    /*
     * 只有真正冷启动（sequence=0、四路 1500 us），或上次卸力时已在
     * 正常 HOME，才允许全关态交给 gait 原子使能。中途 fail-off 会保留
     * 非 HOME 脉宽，必须拒绝下一次触摸，避免 0 ms 瞬间拉腿。部分通道
     * 使能同样拒绝，避免在未知的混合承重姿态启动。
     */
    if (servo.enabled_mask == 0U) {
        bool pristine = servo.sequence == 0U;
        for (uint32_t i = 0U; pristine && i < SERVICE_V2_SERVO_COUNT; ++i) {
            pristine = servo.current_us[i] == SERVICE_V2_SERVO_CENTER_PULSE_US &&
                       servo.target_us[i] == SERVICE_V2_SERVO_CENTER_PULSE_US;
        }
        return pristine || touch_action_servo_near_pose(&servo, s_touch_action_normal_home_us) ? 0 : -EAGAIN;
    }
    if (servo.enabled_mask != SERVICE_V2_SERVO_ALL_MASK) {
        return -EACCES;
    }

    if (!touch_action_servo_near_pose(&servo, s_touch_action_normal_home_us)) {
        return -EAGAIN;
    }

    return 0;
}

static void touch_action_handle_transition(touch_channel_state_t *state)
{
    service_v2_gait_status_t gait = {0};
    service_v2_imu_interaction_status_t imu = {0};
    TickType_t now = xTaskGetTickCount();

    if (!__atomic_load_n(&s_touch_action_enabled, __ATOMIC_ACQUIRE)) {
        return;
    }

    int imu_ret = service_v2_imu_interaction_get_status(&imu);
    if (imu_ret != 0 || !service_v2_touch_pose_allows_action(imu.pose)) {
        LISA_LOGI(TAG, "%s transition ignored for IMU pose=%s ready=%d", state->pin_name,
                  service_v2_imu_interaction_pose_name(imu.pose), imu.ready ? 1 : 0);
        return;
    }

    /* The stable baseline was updated before entering here.  Every ignored
     * transition is therefore consumed and cannot fire later when busy or
     * cooldown ends. */
    if (state->cooldown_valid &&
        (now - state->last_trigger_tick) < pdMS_TO_TICKS(TOUCH_ACTION_COOLDOWN_MS)) {
        LISA_LOGI(TAG, "%s transition ignored during cooldown", state->pin_name);
        return;
    }

    if (power_is_usb_plugged()) {
        if (touch_action_error_log_due()) {
            LISA_LOGD(TAG, "%s action blocked while Type-C is connected", state->pin_name);
        }
        return;
    }

    int ret = service_v2_gait_get_status(&gait);
    if (ret != 0 || !gait.ready) {
        if (touch_action_error_log_due()) {
            LISA_LOGW(TAG, "gait status unavailable: %d", ret);
        }
        return;
    }
    if (gait.busy) {
        LISA_LOGI(TAG, "%s transition ignored while gait is busy", state->pin_name);
        return;
    }

    ret = touch_action_require_idle_home();
    if (ret != 0) {
        if (touch_action_error_log_due()) {
            LISA_LOGW(TAG, "%s action blocked: actuator is not idle at normal HOME or cold-disabled (%d)",
                      state->pin_name, ret);
        }
        return;
    }

    uint16_t voltage_mv = battery_get_voltage_mv();
    if (voltage_mv < TOUCH_ACTION_MIN_BATTERY_MV) {
        if (touch_action_error_log_due()) {
            LISA_LOGW(TAG, "%s action blocked: battery=%umV minimum=%umV", state->pin_name,
                      (unsigned)voltage_mv,
                      (unsigned)TOUCH_ACTION_MIN_BATTERY_MV);
        }
        return;
    }

    ret = service_v2_gait_submit_if_idle(state->action, 0U);
    if (ret != 0) {
        if (touch_action_error_log_due()) {
            LISA_LOGW(TAG, "%s failed to submit %s: %d", state->pin_name,
                      service_v2_gait_action_name(state->action), ret);
        }
        return;
    }

    state->last_trigger_tick = now;
    state->cooldown_valid = true;
    LISA_LOGI(TAG, "%s transition -> %s then HOME (battery=%umV)", state->pin_name,
              service_v2_gait_action_name(state->action), (unsigned)voltage_mv);
}

static void touch_action_accept_sample(touch_channel_state_t *state, bool level)
{
    if (state->candidate_samples == 0U || level != state->candidate_level) {
        state->candidate_level = level;
        state->candidate_samples = 1U;
        if (state->stable_valid && !state->armed) {
            /* Any observed change breaks the continuous arming window, even
             * if it later proves to be a short bounce. */
            state->arming_since_tick = xTaskGetTickCount();
        }
        return;
    }

    if (state->candidate_samples < TOUCH_ACTION_STABLE_SAMPLES) {
        state->candidate_samples++;
    }
    if (state->candidate_samples < TOUCH_ACTION_STABLE_SAMPLES) {
        return;
    }

    if (!state->stable_valid) {
        state->stable_level = level;
        state->stable_valid = true;
        state->arming_since_tick = xTaskGetTickCount();
        LISA_LOGI(TAG, "%s baseline ready: level=%u; start arming window", state->pin_name,
                  level ? 1U : 0U);
        return;
    }

    if (!state->armed) {
        TickType_t now = xTaskGetTickCount();

        if (level != state->stable_level) {
            /* TTP223 is latched: accept both stable levels as the new baseline
             * during arming, but never turn them into an action. */
            state->stable_level = level;
            state->arming_since_tick = now;
            return;
        }
        if ((now - state->arming_since_tick) >= pdMS_TO_TICKS(TOUCH_ACTION_ARMING_STABLE_MS)) {
            state->armed = true;
            LISA_LOGI(TAG, "%s armed after %ums stable", state->pin_name,
                      (unsigned)TOUCH_ACTION_ARMING_STABLE_MS);
        }
        return;
    }
    if (level == state->stable_level) {
        return;
    }

    /* Synchronize the latched TTP223 baseline before any gating decision. */
    state->stable_level = level;
    touch_action_handle_transition(state);
}

static void touch_action_task(void *arg)
{
    (void)arg;

    for (;;) {
        bool enabled = __atomic_load_n(&s_touch_action_enabled, __ATOMIC_ACQUIRE);
        service_v2_idle_action_poll(enabled);
        if (!enabled) {
            lisa_thread_mdelay(TOUCH_ACTION_POLL_MS);
            continue;
        }
        if (__atomic_exchange_n(&s_touch_action_reset_requested, false, __ATOMIC_ACQ_REL)) {
            touch_action_reset_baselines();
        }

        service_v2_imu_interaction_status_t imu = {0};
        if (service_v2_imu_interaction_get_status(&imu) != 0 ||
            !service_v2_touch_pose_allows_action(imu.pose)) {
            /* Consume no touch edge while the dog is upside down or moving
             * through an unclassified pose.  A fresh one-second stable
             * baseline is required after it is upright again. */
            touch_action_reset_baselines();
            lisa_thread_mdelay(TOUCH_ACTION_POLL_MS);
            continue;
        }

        bool pa09_level = false;
        bool pa13_level = false;
        int pa09_ret = service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, &pa09_level);
        int pa13_ret = service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA13, &pa13_level);

        if (pa09_ret == SERVICE_V2_SENSORS_OK) {
            touch_action_accept_sample(&s_pa09, pa09_level);
        } else {
            s_pa09.candidate_samples = 0U;
            if (s_pa09.stable_valid && !s_pa09.armed) s_pa09.arming_since_tick = xTaskGetTickCount();
            if (touch_action_error_log_due()) {
                LISA_LOGW(TAG, "PA09 read failed: %d", pa09_ret);
            }
        }

        if (pa13_ret == SERVICE_V2_SENSORS_OK) {
            touch_action_accept_sample(&s_pa13, pa13_level);
        } else {
            /* A failed poll breaks the consecutive-sample debounce window. */
            s_pa13.candidate_samples = 0U;
            if (s_pa13.stable_valid && !s_pa13.armed) {
                s_pa13.arming_since_tick = xTaskGetTickCount();
            }
            if (touch_action_error_log_due()) {
                LISA_LOGW(TAG, "PA13 read failed: %d", pa13_ret);
            }
        }

        lisa_thread_mdelay(TOUCH_ACTION_POLL_MS);
    }
}

int service_v2_touch_action_init(void)
{
    service_v2_gait_status_t gait = {0};

    if (s_touch_action_thread != NULL) {
        return 0;
    }

    int ret = service_v2_gait_get_status(&gait);
    if (ret != 0 || !gait.ready) {
        LISA_LOGE(TAG, "gait service is not ready: %d", ret);
        return ret != 0 ? ret : -ENODEV;
    }

    ret = service_v2_touch_init();
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGE(TAG, "touch service is not ready: %d", ret);
        return ret;
    }

    /* Establishing the baseline needs three equal worker samples.  Do not
     * turn the power-on level into an action. */
    touch_action_reset_baselines();
    s_pa09.cooldown_valid = false;
    s_pa13.cooldown_valid = false;
    s_error_log_valid = false;

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"v2_touch_action",
        .stack_size = TOUCH_ACTION_TASK_STACK,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    s_touch_action_thread = lisa_thread_create(&attr, touch_action_task, NULL);
    if (s_touch_action_thread == NULL) {
        LISA_LOGE(TAG, "failed to create dual touch worker");
        return -ENOMEM;
    }

    LISA_LOGI(TAG, "touch ready: PA09 left handshake + PA13 right action (poll=%ums debounce=%u arm=%ums cooldown=%ums min_battery=%umV)",
              (unsigned)TOUCH_ACTION_POLL_MS, (unsigned)TOUCH_ACTION_STABLE_SAMPLES,
              (unsigned)TOUCH_ACTION_ARMING_STABLE_MS, (unsigned)TOUCH_ACTION_COOLDOWN_MS,
              (unsigned)TOUCH_ACTION_MIN_BATTERY_MV);
    return 0;
}

int service_v2_touch_action_set_enabled(int enabled)
{
    if (s_touch_action_thread == NULL) {
        return -ENODEV;
    }

    if (!enabled) {
        __atomic_store_n(&s_touch_action_enabled, false, __ATOMIC_RELEASE);
        return 0;
    }

    __atomic_store_n(&s_touch_action_reset_requested, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_touch_action_enabled, true, __ATOMIC_RELEASE);
    return 0;
}
