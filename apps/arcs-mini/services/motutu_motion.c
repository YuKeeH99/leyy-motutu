#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "FreeRTOS.h"
#include "task.h"

#define TAG "motutu"
#include "lisa_log.h"

#include "shell.h"

#include "motutu_motion.h"
#include "service_motutu_web.h"
#include "servo_bus.h"

#define MOTUTU_LF_ID 1U
#define MOTUTU_RF_ID 2U
#define MOTUTU_LR_ID 3U
#define MOTUTU_RR_ID 4U

#define MOTUTU_SERVO_ABS_MAX_DEG 270U
#define MOTUTU_DEFAULT_MIN_US    500U
#define MOTUTU_DEFAULT_MAX_US    2500U
#define MOTUTU_LF_CENTER_ABS_DEG 90U
#define MOTUTU_RF_CENTER_ABS_DEG 90U
#define MOTUTU_LR_CENTER_ABS_DEG 110U
#define MOTUTU_RR_CENTER_ABS_DEG 110U
#define MOTUTU_LF_CENTER_US      1500U
#define MOTUTU_RF_CENTER_US      1500U
#define MOTUTU_LR_CENTER_US      1315U
#define MOTUTU_RR_CENTER_US      1315U
#define MOTUTU_LF_STAND_US       1950U
#define MOTUTU_RF_STAND_US       1050U
#define MOTUTU_LR_STAND_US       950U
#define MOTUTU_RR_STAND_US       1750U
#define MOTUTU_LF_MIN_DEG        (-180)
#define MOTUTU_LF_MAX_DEG        120
#define MOTUTU_RF_MIN_DEG        (-100)
#define MOTUTU_RF_MAX_DEG        100
#define MOTUTU_LR_MIN_DEG        (-90)
#define MOTUTU_LR_MAX_DEG        180
#define MOTUTU_RR_MIN_DEG        (-180)
#define MOTUTU_RR_MAX_DEG        100
#define MOTUTU_SEQ_SLICE_MS      20U
#define MOTUTU_SEQ_JSON_MAX      768U
#define MOTUTU_GAIT_PHASE_MAX    12U
#define MOTUTU_CALIBRATE_TIME_MS 700U
#define MOTUTU_STAND_TIME_MS     520U
#define MOTUTU_HOME_TIME_MS      360U

typedef struct {
    uint8_t id;
    int8_t dir;
    uint16_t center_abs_deg;
    uint16_t pulse_center_us;
    uint16_t pulse_min_us;
    uint16_t pulse_max_us;
    int16_t angle_min_deg;
    int16_t angle_max_deg;
} motutu_joint_cfg_t;

static bool s_motutu_ready = false;
static motutu_pose_t s_current_pose = {0};
static uint16_t s_current_abs_deg[4] = {
    MOTUTU_LF_CENTER_ABS_DEG,
    MOTUTU_RF_CENTER_ABS_DEG,
    MOTUTU_LR_CENTER_ABS_DEG,
    MOTUTU_RR_CENTER_ABS_DEG,
};
static uint16_t s_current_pulse_us[4] = {
    MOTUTU_LF_CENTER_US,
    MOTUTU_RF_CENTER_US,
    MOTUTU_LR_CENTER_US,
    MOTUTU_RR_CENTER_US,
};
static motutu_joint_cfg_t s_joint_cfgs[4] = {
    {MOTUTU_LF_ID, +1, MOTUTU_LF_CENTER_ABS_DEG, MOTUTU_LF_CENTER_US, MOTUTU_DEFAULT_MIN_US, MOTUTU_DEFAULT_MAX_US,
     MOTUTU_LF_MIN_DEG, MOTUTU_LF_MAX_DEG},
    {MOTUTU_RF_ID, -1, MOTUTU_RF_CENTER_ABS_DEG, MOTUTU_RF_CENTER_US, MOTUTU_DEFAULT_MIN_US, MOTUTU_DEFAULT_MAX_US,
     MOTUTU_RF_MIN_DEG, MOTUTU_RF_MAX_DEG},
    {MOTUTU_LR_ID, +1, MOTUTU_LR_CENTER_ABS_DEG, MOTUTU_LR_CENTER_US, MOTUTU_DEFAULT_MIN_US, MOTUTU_DEFAULT_MAX_US,
     MOTUTU_LR_MIN_DEG, MOTUTU_LR_MAX_DEG},
    {MOTUTU_RR_ID, -1, MOTUTU_RR_CENTER_ABS_DEG, MOTUTU_RR_CENTER_US, MOTUTU_DEFAULT_MIN_US, MOTUTU_DEFAULT_MAX_US,
     MOTUTU_RR_MIN_DEG, MOTUTU_RR_MAX_DEG},
};

static const motutu_pose_t s_pose_home = {
    .lf_deg = 0, .rf_deg = 0, .lr_deg = 0, .rr_deg = 0, .hold_ms = 200,
};

static const motutu_pose_t s_pose_home_fast = {
    .lf_deg = 0, .rf_deg = 0, .lr_deg = 0, .rr_deg = 0, .hold_ms = 80,
};

static const motutu_pose_t s_pose_down = {
    .lf_deg = -36, .rf_deg = -36, .lr_deg = 36, .rr_deg = 36, .hold_ms = 340,
};

static const motutu_pose_t s_pose_flip_prepare = {
    .lf_deg = -45, .rf_deg = -45, .lr_deg = 40, .rr_deg = 40, .hold_ms = 220,
};

static const motutu_pose_t s_pose_flip_kick = {
    .lf_deg = 55, .rf_deg = 55, .lr_deg = -55, .rr_deg = -55, .hold_ms = 180,
};

typedef enum {
    MOTUTU_GAIT_FORWARD = 0,
    MOTUTU_GAIT_BACKWARD = 1,
    MOTUTU_GAIT_TURN_LEFT = 2,
    MOTUTU_GAIT_TURN_RIGHT = 3,
} motutu_gait_kind_t;

typedef struct {
    int16_t push;
    int16_t recover;
    uint8_t phase;
    int8_t sign;
} motutu_walk_leg_profile_t;

static int motutu_require_ready(void);
static int motutu_reset_all_servos_to_default_pose(void);
static void motutu_fill_calibrate_abs_pose(uint16_t abs_deg[4]);
static void motutu_fill_calibrate_pulse_pose(uint16_t pulse_us[4]);
static void motutu_fill_stand_pulse_pose(uint16_t pulse_us[4]);
static void motutu_fill_stand_pose(motutu_pose_t *pose, uint16_t hold_ms);
static void motutu_fill_stand_offset_pose(motutu_pose_t *pose,
                                          int16_t lf_deg,
                                          int16_t rf_deg,
                                          int16_t lr_deg,
                                          int16_t rr_deg,
                                          uint16_t hold_ms);
static void motutu_offset_sequence_from_stand(motutu_pose_t *seq, uint16_t count);
static int motutu_apply_abs_pose_smooth(const uint16_t abs_deg[4], uint16_t time_ms);
static int motutu_build_gait_sequence(motutu_gait_kind_t kind,
                                      motutu_speed_t speed,
                                      motutu_pose_t *gait,
                                      uint16_t max_count,
                                      uint16_t *out_count);
static int motutu_build_gait_pose(motutu_pose_t *pose,
                                  int16_t lf_deg,
                                  int16_t rf_deg,
                                  int16_t lr_deg,
                                  int16_t rr_deg,
                                  uint16_t hold_ms);

static int16_t motutu_forward_wave(uint8_t sample, uint8_t phase_offset, int16_t push, int16_t recover)
{
    uint8_t phase = (uint8_t)((sample + phase_offset) % MOTUTU_GAIT_PHASE_MAX);
    const uint8_t recover_samples = 3U;
    const uint8_t stance_samples = MOTUTU_GAIT_PHASE_MAX - recover_samples;

    if (phase < recover_samples) {
        return (int16_t)(push - (((int32_t)(push + recover) * phase) / (recover_samples - 1U)));
    }

    phase = (uint8_t)(phase - recover_samples);
    return (int16_t)(-recover + (((int32_t)(push + recover) * phase) / (stance_samples - 1U)));
}

static int16_t motutu_walk_wave_value(uint8_t sample, const motutu_walk_leg_profile_t *profile)
{
    int16_t value;

    if (profile == NULL) {
        return 0;
    }

    value = motutu_forward_wave(sample, profile->phase, profile->push, profile->recover);
    return profile->sign >= 0 ? value : (int16_t)(-value);
}

static int motutu_build_walk_wave_gait(motutu_pose_t *gait,
                                       uint16_t max_count,
                                       uint16_t phase_ms,
                                       const motutu_walk_leg_profile_t *lf,
                                       const motutu_walk_leg_profile_t *rf,
                                       const motutu_walk_leg_profile_t *lr,
                                       const motutu_walk_leg_profile_t *rr)
{
    uint16_t i;

    if (gait == NULL || max_count < MOTUTU_GAIT_PHASE_MAX) {
        return -1;
    }

    for (i = 0; i < MOTUTU_GAIT_PHASE_MAX; ++i) {
        motutu_build_gait_pose(&gait[i],
                               motutu_walk_wave_value((uint8_t)i, lf),
                               motutu_walk_wave_value((uint8_t)i, rf),
                               motutu_walk_wave_value((uint8_t)i, lr),
                               motutu_walk_wave_value((uint8_t)i, rr),
                               phase_ms);
    }

    return 0;
}

static int16_t motutu_speed_stride(motutu_speed_t speed)
{
    switch (speed) {
    case MOTUTU_SPEED_SLOW:
        return 12;
    case MOTUTU_SPEED_FAST:
        return 24;
    case MOTUTU_SPEED_NORMAL:
    default:
        return 18;
    }
}

static uint16_t motutu_speed_hold_ms(motutu_speed_t speed)
{
    switch (speed) {
    case MOTUTU_SPEED_SLOW:
        return 420U;
    case MOTUTU_SPEED_FAST:
        return 220U;
    case MOTUTU_SPEED_NORMAL:
    default:
        return 320U;
    }
}

static uint16_t motutu_speed_phase_ms(motutu_speed_t speed)
{
    switch (speed) {
    case MOTUTU_SPEED_SLOW:
        return 90U;
    case MOTUTU_SPEED_FAST:
        return 45U;
    case MOTUTU_SPEED_NORMAL:
    default:
        return 65U;
    }
}

static int motutu_clamp_angle(const motutu_joint_cfg_t *cfg, int16_t angle_deg)
{
    if (angle_deg < cfg->angle_min_deg) {
        return cfg->angle_min_deg;
    }
    if (angle_deg > cfg->angle_max_deg) {
        return cfg->angle_max_deg;
    }
    return angle_deg;
}

static uint32_t motutu_joint_angle_to_pulse(const motutu_joint_cfg_t *cfg, int16_t logical_angle_deg)
{
    int16_t angle_deg = (int16_t)motutu_clamp_angle(cfg, logical_angle_deg);
    int32_t pulse;

    if (angle_deg == 0) {
        return cfg->pulse_center_us;
    }

    if (angle_deg > 0) {
        int32_t target = cfg->dir >= 0 ? cfg->pulse_max_us : cfg->pulse_min_us;
        int32_t span_deg = cfg->angle_max_deg > 0 ? cfg->angle_max_deg : 1;

        pulse = (int32_t)cfg->pulse_center_us +
                ((target - (int32_t)cfg->pulse_center_us) * angle_deg) / span_deg;
    } else {
        int32_t target = cfg->dir >= 0 ? cfg->pulse_min_us : cfg->pulse_max_us;
        int32_t span_deg = cfg->angle_min_deg < 0 ? -cfg->angle_min_deg : 1;

        pulse = (int32_t)cfg->pulse_center_us +
                ((target - (int32_t)cfg->pulse_center_us) * (-angle_deg)) / span_deg;
    }

    if (pulse < (int32_t)cfg->pulse_min_us) {
        pulse = cfg->pulse_min_us;
    } else if (pulse > (int32_t)cfg->pulse_max_us) {
        pulse = cfg->pulse_max_us;
    }

    return (uint32_t)pulse;
}

static uint16_t motutu_joint_logical_to_abs_deg(const motutu_joint_cfg_t *cfg, int16_t logical_angle_deg)
{
    int16_t angle_deg = (int16_t)motutu_clamp_angle(cfg, logical_angle_deg);
    int32_t abs_deg = (int32_t)cfg->center_abs_deg + (int32_t)(cfg->dir * angle_deg);

    if (abs_deg < 0) {
        abs_deg = 0;
    } else if (abs_deg > MOTUTU_SERVO_ABS_MAX_DEG) {
        abs_deg = MOTUTU_SERVO_ABS_MAX_DEG;
    }
    return (uint16_t)abs_deg;
}

static int16_t motutu_joint_abs_to_logical_deg(const motutu_joint_cfg_t *cfg, uint16_t abs_deg)
{
    int32_t logical = (int32_t)cfg->dir * ((int32_t)abs_deg - (int32_t)cfg->center_abs_deg);

    return (int16_t)motutu_clamp_angle(cfg, (int16_t)logical);
}

static uint16_t motutu_abs_to_servo_pulse(uint16_t abs_deg)
{
    if (abs_deg > MOTUTU_SERVO_ABS_MAX_DEG) {
        abs_deg = MOTUTU_SERVO_ABS_MAX_DEG;
    }

    return (uint16_t)(500U + ((uint32_t)abs_deg * 2000U) / MOTUTU_SERVO_ABS_MAX_DEG);
}

static int16_t motutu_joint_pulse_to_logical_deg(const motutu_joint_cfg_t *cfg, uint16_t pulse_us)
{
    int32_t pulse = pulse_us;

    if (pulse < (int32_t)cfg->pulse_min_us) {
        pulse = cfg->pulse_min_us;
    } else if (pulse > (int32_t)cfg->pulse_max_us) {
        pulse = cfg->pulse_max_us;
    }

    if (pulse == (int32_t)cfg->pulse_center_us) {
        return 0;
    }

    if (cfg->dir >= 0) {
        if (pulse > (int32_t)cfg->pulse_center_us) {
            int32_t span = (int32_t)cfg->pulse_max_us - (int32_t)cfg->pulse_center_us;

            return span > 0 ? (int16_t)(((pulse - (int32_t)cfg->pulse_center_us) * cfg->angle_max_deg) / span) : 0;
        }

        return cfg->pulse_center_us > cfg->pulse_min_us
                   ? (int16_t)(-(((int32_t)cfg->pulse_center_us - pulse) * (-cfg->angle_min_deg)) /
                               ((int32_t)cfg->pulse_center_us - (int32_t)cfg->pulse_min_us))
                   : 0;
    }

    if (pulse < (int32_t)cfg->pulse_center_us) {
        int32_t span = (int32_t)cfg->pulse_center_us - (int32_t)cfg->pulse_min_us;

        return span > 0 ? (int16_t)((((int32_t)cfg->pulse_center_us - pulse) * cfg->angle_max_deg) / span) : 0;
    }

    return cfg->pulse_max_us > cfg->pulse_center_us
               ? (int16_t)(-(((pulse - (int32_t)cfg->pulse_center_us) * (-cfg->angle_min_deg)) /
                           ((int32_t)cfg->pulse_max_us - (int32_t)cfg->pulse_center_us)))
               : 0;
}

static void motutu_store_abs_pose_cache(const uint16_t abs_deg[4])
{
    if (abs_deg == NULL) {
        return;
    }

    s_current_abs_deg[0] = abs_deg[0];
    s_current_abs_deg[1] = abs_deg[1];
    s_current_abs_deg[2] = abs_deg[2];
    s_current_abs_deg[3] = abs_deg[3];
    s_current_pulse_us[0] = motutu_abs_to_servo_pulse(abs_deg[0]);
    s_current_pulse_us[1] = motutu_abs_to_servo_pulse(abs_deg[1]);
    s_current_pulse_us[2] = motutu_abs_to_servo_pulse(abs_deg[2]);
    s_current_pulse_us[3] = motutu_abs_to_servo_pulse(abs_deg[3]);

    s_current_pose.lf_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[0], abs_deg[0]);
    s_current_pose.rf_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[1], abs_deg[1]);
    s_current_pose.lr_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[2], abs_deg[2]);
    s_current_pose.rr_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[3], abs_deg[3]);
}

static int motutu_apply_abs_pose(const uint16_t abs_deg[4])
{
    int ret = 0;
    uint8_t i = 0;

    if (motutu_require_ready() != 0 || abs_deg == NULL) {
        return -1;
    }

    for (i = 0; i < 4U; ++i) {
        if (servo_set_abs_angle_deg(s_joint_cfgs[i].id, abs_deg[i]) != 0) {
            ret = -1;
        }
    }

    if (ret == 0) {
        motutu_store_abs_pose_cache(abs_deg);
    }
    return ret;
}

static void motutu_fill_calibrate_pulse_pose(uint16_t pulse_us[4])
{
    if (pulse_us == NULL) {
        return;
    }

    pulse_us[0] = s_joint_cfgs[0].pulse_center_us;
    pulse_us[1] = s_joint_cfgs[1].pulse_center_us;
    pulse_us[2] = s_joint_cfgs[2].pulse_center_us;
    pulse_us[3] = s_joint_cfgs[3].pulse_center_us;
}

static void motutu_fill_stand_pulse_pose(uint16_t pulse_us[4])
{
    if (pulse_us == NULL) {
        return;
    }

    pulse_us[0] = MOTUTU_LF_STAND_US;
    pulse_us[1] = MOTUTU_RF_STAND_US;
    pulse_us[2] = MOTUTU_LR_STAND_US;
    pulse_us[3] = MOTUTU_RR_STAND_US;

    if (pulse_us[0] < s_joint_cfgs[0].pulse_min_us) {
        pulse_us[0] = s_joint_cfgs[0].pulse_min_us;
    } else if (pulse_us[0] > s_joint_cfgs[0].pulse_max_us) {
        pulse_us[0] = s_joint_cfgs[0].pulse_max_us;
    }
    if (pulse_us[1] < s_joint_cfgs[1].pulse_min_us) {
        pulse_us[1] = s_joint_cfgs[1].pulse_min_us;
    } else if (pulse_us[1] > s_joint_cfgs[1].pulse_max_us) {
        pulse_us[1] = s_joint_cfgs[1].pulse_max_us;
    }
    if (pulse_us[2] < s_joint_cfgs[2].pulse_min_us) {
        pulse_us[2] = s_joint_cfgs[2].pulse_min_us;
    } else if (pulse_us[2] > s_joint_cfgs[2].pulse_max_us) {
        pulse_us[2] = s_joint_cfgs[2].pulse_max_us;
    }
    if (pulse_us[3] < s_joint_cfgs[3].pulse_min_us) {
        pulse_us[3] = s_joint_cfgs[3].pulse_min_us;
    } else if (pulse_us[3] > s_joint_cfgs[3].pulse_max_us) {
        pulse_us[3] = s_joint_cfgs[3].pulse_max_us;
    }
}

static void motutu_fill_stand_pose(motutu_pose_t *pose, uint16_t hold_ms)
{
    uint16_t stand_pulse[4];

    if (pose == NULL) {
        return;
    }

    motutu_fill_stand_pulse_pose(stand_pulse);
    pose->lf_deg = motutu_joint_pulse_to_logical_deg(&s_joint_cfgs[0], stand_pulse[0]);
    pose->rf_deg = motutu_joint_pulse_to_logical_deg(&s_joint_cfgs[1], stand_pulse[1]);
    pose->lr_deg = motutu_joint_pulse_to_logical_deg(&s_joint_cfgs[2], stand_pulse[2]);
    pose->rr_deg = motutu_joint_pulse_to_logical_deg(&s_joint_cfgs[3], stand_pulse[3]);
    pose->hold_ms = hold_ms;
}

static void motutu_fill_stand_offset_pose(motutu_pose_t *pose,
                                          int16_t lf_deg,
                                          int16_t rf_deg,
                                          int16_t lr_deg,
                                          int16_t rr_deg,
                                          uint16_t hold_ms)
{
    if (pose == NULL) {
        return;
    }

    motutu_fill_stand_pose(pose, hold_ms);
    pose->lf_deg = (int16_t)motutu_clamp_angle(&s_joint_cfgs[0], pose->lf_deg + lf_deg);
    pose->rf_deg = (int16_t)motutu_clamp_angle(&s_joint_cfgs[1], pose->rf_deg + rf_deg);
    pose->lr_deg = (int16_t)motutu_clamp_angle(&s_joint_cfgs[2], pose->lr_deg + lr_deg);
    pose->rr_deg = (int16_t)motutu_clamp_angle(&s_joint_cfgs[3], pose->rr_deg + rr_deg);
    pose->hold_ms = hold_ms;
}

static void motutu_offset_sequence_from_stand(motutu_pose_t *seq, uint16_t count)
{
    uint16_t i = 0;

    if (seq == NULL || count == 0U) {
        return;
    }

    for (i = 0; i < count; ++i) {
        motutu_fill_stand_offset_pose(&seq[i],
                                      seq[i].lf_deg,
                                      seq[i].rf_deg,
                                      seq[i].lr_deg,
                                      seq[i].rr_deg,
                                      seq[i].hold_ms);
    }
}

static int motutu_set_joint_angle(uint8_t joint_index, int16_t logical_angle_deg)
{
    uint16_t abs_deg;
    uint16_t pulse_us;
    int16_t clamped_deg;

    if (joint_index >= 4U) {
        return -1;
    }

    clamped_deg = (int16_t)motutu_clamp_angle(&s_joint_cfgs[joint_index], logical_angle_deg);
    abs_deg = motutu_joint_logical_to_abs_deg(&s_joint_cfgs[joint_index], clamped_deg);
    pulse_us = (uint16_t)motutu_joint_angle_to_pulse(&s_joint_cfgs[joint_index], clamped_deg);
    if (servo_set_pulse_us(s_joint_cfgs[joint_index].id, pulse_us) != 0) {
        return -1;
    }

    s_current_abs_deg[joint_index] = abs_deg;
    s_current_pulse_us[joint_index] = pulse_us;
    switch (joint_index) {
    case 0:
        s_current_pose.lf_deg = clamped_deg;
        break;
    case 1:
        s_current_pose.rf_deg = clamped_deg;
        break;
    case 2:
        s_current_pose.lr_deg = clamped_deg;
        break;
    case 3:
        s_current_pose.rr_deg = clamped_deg;
        break;
    default:
        break;
    }
    return 0;
}

static int motutu_set_joint_pulse(uint8_t joint_index, uint16_t pulse_us)
{
    int16_t logical_deg;
    uint16_t abs_deg;

    if (joint_index >= 4U) {
        return -1;
    }

    if (pulse_us < s_joint_cfgs[joint_index].pulse_min_us) {
        pulse_us = s_joint_cfgs[joint_index].pulse_min_us;
    } else if (pulse_us > s_joint_cfgs[joint_index].pulse_max_us) {
        pulse_us = s_joint_cfgs[joint_index].pulse_max_us;
    }

    if (servo_set_pulse_us(s_joint_cfgs[joint_index].id, pulse_us) != 0) {
        return -1;
    }

    logical_deg = motutu_joint_pulse_to_logical_deg(&s_joint_cfgs[joint_index], pulse_us);
    abs_deg = motutu_joint_logical_to_abs_deg(&s_joint_cfgs[joint_index], logical_deg);
    s_current_abs_deg[joint_index] = abs_deg;
    s_current_pulse_us[joint_index] = pulse_us;
    switch (joint_index) {
    case 0:
        s_current_pose.lf_deg = logical_deg;
        break;
    case 1:
        s_current_pose.rf_deg = logical_deg;
        break;
    case 2:
        s_current_pose.lr_deg = logical_deg;
        break;
    case 3:
        s_current_pose.rr_deg = logical_deg;
        break;
    default:
        break;
    }
    return 0;
}

static int motutu_set_joint_abs_angle(uint8_t joint_index, uint16_t angle_deg)
{
    if (joint_index >= 4U) {
        return -1;
    }

    if (servo_set_abs_angle_deg(s_joint_cfgs[joint_index].id, angle_deg) != 0) {
        return -1;
    }

    s_current_abs_deg[joint_index] = angle_deg;
    s_current_pulse_us[joint_index] = motutu_abs_to_servo_pulse(angle_deg);
    switch (joint_index) {
    case 0:
        s_current_pose.lf_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[joint_index], angle_deg);
        break;
    case 1:
        s_current_pose.rf_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[joint_index], angle_deg);
        break;
    case 2:
        s_current_pose.lr_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[joint_index], angle_deg);
        break;
    case 3:
        s_current_pose.rr_deg = motutu_joint_abs_to_logical_deg(&s_joint_cfgs[joint_index], angle_deg);
        break;
    default:
        break;
    }
    return 0;
}

static int motutu_require_ready(void)
{
    if (!s_motutu_ready) {
        int ret = servo_bus_init();

        if (ret != 0) {
            LOGE("motutu motion is not ready");
            return -1;
        }

        s_motutu_ready = true;
        if (motutu_reset_all_servos_to_default_pose() != 0) {
            LOGE("motutu late init reset failed");
            s_motutu_ready = false;
            return -1;
        }
        LOGW("motutu late init ok");
    }
    return 0;
}

static void motutu_fill_calibrate_abs_pose(uint16_t abs_deg[4])
{
    if (abs_deg == NULL) {
        return;
    }

    abs_deg[0] = s_joint_cfgs[0].center_abs_deg;
    abs_deg[1] = s_joint_cfgs[1].center_abs_deg;
    abs_deg[2] = s_joint_cfgs[2].center_abs_deg;
    abs_deg[3] = s_joint_cfgs[3].center_abs_deg;
}

static int motutu_build_gait_pose(motutu_pose_t *pose,
                                  int16_t lf,
                                  int16_t rf,
                                  int16_t lr,
                                  int16_t rr,
                                  uint16_t hold_ms)
{
    if (pose == NULL) {
        return -1;
    }

    motutu_fill_stand_offset_pose(pose, lf, rf, lr, rr, hold_ms);
    return 0;
}

static const char *motutu_gait_name(motutu_gait_kind_t kind)
{
    switch (kind) {
    case MOTUTU_GAIT_FORWARD:
        return "walk";
    case MOTUTU_GAIT_BACKWARD:
        return "back";
    case MOTUTU_GAIT_TURN_LEFT:
        return "left";
    case MOTUTU_GAIT_TURN_RIGHT:
        return "right";
    default:
        return "?";
    }
}

static bool motutu_parse_gait_kind(const char *text, motutu_gait_kind_t *out_kind)
{
    if (text == NULL || out_kind == NULL) {
        return false;
    }
    if (strcmp(text, "walk") == 0 || strcmp(text, "forward") == 0) {
        *out_kind = MOTUTU_GAIT_FORWARD;
        return true;
    }
    if (strcmp(text, "back") == 0 || strcmp(text, "backward") == 0) {
        *out_kind = MOTUTU_GAIT_BACKWARD;
        return true;
    }
    if (strcmp(text, "left") == 0) {
        *out_kind = MOTUTU_GAIT_TURN_LEFT;
        return true;
    }
    if (strcmp(text, "right") == 0) {
        *out_kind = MOTUTU_GAIT_TURN_RIGHT;
        return true;
    }
    return false;
}

static int16_t motutu_lerp_i16(int16_t from, int16_t to, uint8_t step, uint8_t total)
{
    int32_t delta;

    if (total == 0U) {
        return to;
    }

    delta = (int32_t)to - (int32_t)from;
    return (int16_t)((int32_t)from + (delta * (int32_t)step) / (int32_t)total);
}

static const char *motutu_joint_name(uint8_t joint_index)
{
    switch (joint_index) {
    case 0:
        return "lf";
    case 1:
        return "rf";
    case 2:
        return "lr";
    case 3:
        return "rr";
    default:
        return "?";
    }
}

static int motutu_parse_joint_index(const char *text)
{
    if (text == NULL) {
        return -1;
    }
    if (strcmp(text, "1") == 0 || strcmp(text, "lf") == 0) {
        return 0;
    }
    if (strcmp(text, "2") == 0 || strcmp(text, "rf") == 0) {
        return 1;
    }
    if (strcmp(text, "3") == 0 || strcmp(text, "lr") == 0) {
        return 2;
    }
    if (strcmp(text, "4") == 0 || strcmp(text, "rr") == 0) {
        return 3;
    }
    return -1;
}

static int motutu_build_gait_sequence(motutu_gait_kind_t kind,
                                      motutu_speed_t speed,
                                      motutu_pose_t *gait,
                                      uint16_t max_count,
                                      uint16_t *out_count)
{
    int16_t stride;
    uint16_t hold_ms;

    if (gait == NULL || out_count == NULL) {
        return -1;
    }

    stride = motutu_speed_stride(speed);
    hold_ms = motutu_speed_hold_ms(speed);

    switch (kind) {
    case MOTUTU_GAIT_FORWARD: {
        uint16_t phase_ms = (uint16_t)(motutu_speed_phase_ms(speed) * 2U);
        int16_t front_push = stride - 4;
        int16_t front_recover;
        int16_t rear_push = stride + 5;
        int16_t rear_recover;
        motutu_walk_leg_profile_t lf;
        motutu_walk_leg_profile_t rf;
        motutu_walk_leg_profile_t lr;
        motutu_walk_leg_profile_t rr;

        if (max_count < MOTUTU_GAIT_PHASE_MAX) {
            return -1;
        }
        if (front_push < 8) {
            front_push = 8;
        }
        if (rear_push < 10) {
            rear_push = 10;
        }
        front_recover = front_push / 2;
        rear_recover = rear_push / 4;
        if (front_recover < 4) {
            front_recover = 4;
        }
        if (rear_recover < 4) {
            rear_recover = 4;
        }

        /* 前进改成“后腿主驱动、前腿导向”的 12 相波形，并把每相时间拉长到原来的两倍。
         * 这样既能和后退明显区分，也能把整体节奏降到用户要求的“慢一半”。 */
        lf = (motutu_walk_leg_profile_t){.push = front_push, .recover = front_recover, .phase = 0U, .sign = -1};
        rf = (motutu_walk_leg_profile_t){.push = front_push, .recover = front_recover, .phase = 6U, .sign = +1};
        lr = (motutu_walk_leg_profile_t){.push = rear_push, .recover = rear_recover, .phase = 3U, .sign = -1};
        rr = (motutu_walk_leg_profile_t){.push = rear_push, .recover = rear_recover, .phase = 9U, .sign = +1};
        if (motutu_build_walk_wave_gait(gait, max_count, phase_ms, &lf, &rf, &lr, &rr) != 0) {
            return -1;
        }
        *out_count = MOTUTU_GAIT_PHASE_MAX;
        return 0;
    }
    case MOTUTU_GAIT_BACKWARD: {
        uint16_t phase_ms = motutu_speed_phase_ms(speed);
        int16_t front_push = stride + 2;
        int16_t front_recover;
        int16_t rear_push = stride - 4;
        int16_t rear_recover;
        motutu_walk_leg_profile_t lf;
        motutu_walk_leg_profile_t rf;
        motutu_walk_leg_profile_t lr;
        motutu_walk_leg_profile_t rr;

        if (max_count < MOTUTU_GAIT_PHASE_MAX) {
            return -1;
        }
        if (front_push < 8) {
            front_push = 8;
        }
        if (rear_push < 6) {
            rear_push = 6;
        }
        front_recover = front_push / 3;
        rear_recover = rear_push / 2;
        if (front_recover < 4) {
            front_recover = 4;
        }
        if (rear_recover < 4) {
            rear_recover = 4;
        }

        /* 后退保留连续波形，但改成“前腿主拉、后腿短行程跟随”的相位分布，
         * 不再只是把前进整套动作做一个正负镜像。 */
        lf = (motutu_walk_leg_profile_t){.push = front_push, .recover = front_recover, .phase = 3U, .sign = +1};
        rf = (motutu_walk_leg_profile_t){.push = front_push, .recover = front_recover, .phase = 9U, .sign = -1};
        lr = (motutu_walk_leg_profile_t){.push = rear_push, .recover = rear_recover, .phase = 0U, .sign = +1};
        rr = (motutu_walk_leg_profile_t){.push = rear_push, .recover = rear_recover, .phase = 6U, .sign = -1};
        if (motutu_build_walk_wave_gait(gait, max_count, phase_ms, &lf, &rf, &lr, &rr) != 0) {
            return -1;
        }
        *out_count = MOTUTU_GAIT_PHASE_MAX;
        return 0;
    }
    case MOTUTU_GAIT_TURN_LEFT: {
        int16_t assist_front = stride / 4;
        int16_t assist_rear = stride / 5;
        int16_t outer = stride - 2;
        uint16_t turn_hold_ms = (uint16_t)(hold_ms + 120U);

        if (max_count < 2U) {
            return -1;
        }
        if (outer < 6) {
            outer = 6;
        }
        if (assist_front < 2) {
            assist_front = 2;
        }
        if (assist_rear < 0) {
            assist_rear = 0;
        }
        /* 左转改为“右侧主驱动 + 左前轻微辅助 + 左后弱跟随”，
         * 避免四腿大幅对拉带来的瞬时扭矩和机械卡滞。 */
        motutu_build_gait_pose(&gait[0], -assist_front, -outer, assist_rear, outer, turn_hold_ms);
        motutu_build_gait_pose(&gait[1], assist_front, outer, -assist_rear, -outer, turn_hold_ms);
        *out_count = 2U;
        return 0;
    }
    case MOTUTU_GAIT_TURN_RIGHT: {
        int16_t assist_front = stride / 4;
        int16_t assist_rear = stride / 5;
        int16_t outer = stride - 2;
        uint16_t turn_hold_ms = (uint16_t)(hold_ms + 120U);

        if (max_count < 2U) {
            return -1;
        }
        if (outer < 6) {
            outer = 6;
        }
        if (assist_front < 2) {
            assist_front = 2;
        }
        if (assist_rear < 0) {
            assist_rear = 0;
        }
        motutu_build_gait_pose(&gait[0], outer, assist_front, -outer, -assist_rear, turn_hold_ms);
        motutu_build_gait_pose(&gait[1], -outer, -assist_front, outer, assist_rear, turn_hold_ms);
        *out_count = 2U;
        return 0;
    }
    default:
        return -1;
    }
}

int motutu_motion_is_ready(void)
{
    return s_motutu_ready ? 1 : 0;
}

int motutu_get_current_pose(motutu_pose_t *out_pose)
{
    if (out_pose == NULL) {
        return -1;
    }

    *out_pose = s_current_pose;
    return 0;
}

int motutu_get_current_abs_pose(uint16_t out_abs_deg[4])
{
    if (out_abs_deg == NULL) {
        return -1;
    }

    out_abs_deg[0] = s_current_abs_deg[0];
    out_abs_deg[1] = s_current_abs_deg[1];
    out_abs_deg[2] = s_current_abs_deg[2];
    out_abs_deg[3] = s_current_abs_deg[3];
    return 0;
}

int motutu_get_joint_info(uint8_t joint_index, motutu_joint_info_t *out_info)
{
    if (out_info == NULL || joint_index >= 4U) {
        return -1;
    }

    out_info->id = s_joint_cfgs[joint_index].id;
    out_info->dir = s_joint_cfgs[joint_index].dir;
    out_info->center_abs_deg = s_joint_cfgs[joint_index].center_abs_deg;
    out_info->pulse_center_us = s_joint_cfgs[joint_index].pulse_center_us;
    out_info->pulse_min_us = s_joint_cfgs[joint_index].pulse_min_us;
    out_info->pulse_max_us = s_joint_cfgs[joint_index].pulse_max_us;
    out_info->angle_min_deg = s_joint_cfgs[joint_index].angle_min_deg;
    out_info->angle_max_deg = s_joint_cfgs[joint_index].angle_max_deg;
    return 0;
}

int motutu_apply_pose(const motutu_pose_t *pose)
{
    int ret = 0;
    uint16_t lf_abs;
    uint16_t rf_abs;
    uint16_t lr_abs;
    uint16_t rr_abs;

    if (motutu_require_ready() != 0 || pose == NULL) {
        return -1;
    }

    lf_abs = motutu_joint_logical_to_abs_deg(&s_joint_cfgs[0], pose->lf_deg);
    rf_abs = motutu_joint_logical_to_abs_deg(&s_joint_cfgs[1], pose->rf_deg);
    lr_abs = motutu_joint_logical_to_abs_deg(&s_joint_cfgs[2], pose->lr_deg);
    rr_abs = motutu_joint_logical_to_abs_deg(&s_joint_cfgs[3], pose->rr_deg);

    if (motutu_set_joint_angle(0, pose->lf_deg) != 0) {
        ret = -1;
    }
    if (motutu_set_joint_angle(1, pose->rf_deg) != 0) {
        ret = -1;
    }
    if (motutu_set_joint_angle(2, pose->lr_deg) != 0) {
        ret = -1;
    }
    if (motutu_set_joint_angle(3, pose->rr_deg) != 0) {
        ret = -1;
    }

    if (ret == 0) {
        s_current_pose = *pose;
    }
    return ret;
}

static int motutu_apply_pose_smooth(const motutu_pose_t *pose, uint8_t slices, uint16_t slice_ms)
{
    motutu_pose_t start_pose;
    uint8_t i = 0;

    if (pose == NULL) {
        return -1;
    }
    if (slices == 0U || slice_ms == 0U) {
        return motutu_apply_pose(pose);
    }

    start_pose = s_current_pose;
    for (i = 1; i <= slices; ++i) {
        motutu_pose_t mid = {
            .lf_deg = motutu_lerp_i16(start_pose.lf_deg, pose->lf_deg, i, slices),
            .rf_deg = motutu_lerp_i16(start_pose.rf_deg, pose->rf_deg, i, slices),
            .lr_deg = motutu_lerp_i16(start_pose.lr_deg, pose->lr_deg, i, slices),
            .rr_deg = motutu_lerp_i16(start_pose.rr_deg, pose->rr_deg, i, slices),
            .hold_ms = 0,
        };

        if (motutu_apply_pose(&mid) != 0) {
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
    }

    return 0;
}

static int motutu_apply_pose_timed(const motutu_pose_t *pose, uint16_t time_ms)
{
    uint16_t slices;
    uint16_t slice_ms;

    if (pose == NULL) {
        return -1;
    }
    if (time_ms <= MOTUTU_SEQ_SLICE_MS) {
        return motutu_apply_pose(pose);
    }

    slices = (uint16_t)(time_ms / MOTUTU_SEQ_SLICE_MS);
    if (slices < 1U) {
        slices = 1U;
    }
    if (slices > 255U) {
        slices = 255U;
    }
    slice_ms = (uint16_t)(time_ms / slices);
    if (slice_ms == 0U) {
        slice_ms = 1U;
    }

    return motutu_apply_pose_smooth(pose, (uint8_t)slices, slice_ms);
}

static int motutu_apply_abs_pose_smooth(const uint16_t abs_deg[4], uint16_t time_ms)
{
    uint16_t start_abs[4];
    uint16_t slices;
    uint16_t slice_ms;
    uint16_t i;

    if (abs_deg == NULL) {
        return -1;
    }

    if (time_ms <= MOTUTU_SEQ_SLICE_MS) {
        return motutu_apply_abs_pose(abs_deg);
    }

    start_abs[0] = s_current_abs_deg[0];
    start_abs[1] = s_current_abs_deg[1];
    start_abs[2] = s_current_abs_deg[2];
    start_abs[3] = s_current_abs_deg[3];
    slices = (uint16_t)(time_ms / MOTUTU_SEQ_SLICE_MS);
    if (slices < 1U) {
        slices = 1U;
    }
    slice_ms = (uint16_t)(time_ms / slices);
    if (slice_ms == 0U) {
        slice_ms = 1U;
    }

    for (i = 1; i <= slices; ++i) {
        uint16_t mid_abs[4] = {
            (uint16_t)motutu_lerp_i16((int16_t)start_abs[0], (int16_t)abs_deg[0], (uint8_t)i, (uint8_t)slices),
            (uint16_t)motutu_lerp_i16((int16_t)start_abs[1], (int16_t)abs_deg[1], (uint8_t)i, (uint8_t)slices),
            (uint16_t)motutu_lerp_i16((int16_t)start_abs[2], (int16_t)abs_deg[2], (uint8_t)i, (uint8_t)slices),
            (uint16_t)motutu_lerp_i16((int16_t)start_abs[3], (int16_t)abs_deg[3], (uint8_t)i, (uint8_t)slices),
        };

        if (motutu_apply_abs_pose(mid_abs) != 0) {
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
    }

    return 0;
}

static int motutu_apply_pulse_pose_smooth(const uint16_t pulse_us[4], uint16_t time_ms)
{
    uint16_t start_pulse[4];
    uint16_t slices;
    uint16_t slice_ms;
    uint16_t i;
    uint8_t joint = 0;

    if (pulse_us == NULL) {
        return -1;
    }

    if (time_ms <= MOTUTU_SEQ_SLICE_MS) {
        for (joint = 0; joint < 4U; ++joint) {
            if (motutu_set_joint_pulse(joint, pulse_us[joint]) != 0) {
                return -1;
            }
        }
        return 0;
    }

    start_pulse[0] = s_current_pulse_us[0];
    start_pulse[1] = s_current_pulse_us[1];
    start_pulse[2] = s_current_pulse_us[2];
    start_pulse[3] = s_current_pulse_us[3];
    slices = (uint16_t)(time_ms / MOTUTU_SEQ_SLICE_MS);
    if (slices < 1U) {
        slices = 1U;
    }
    slice_ms = (uint16_t)(time_ms / slices);
    if (slice_ms == 0U) {
        slice_ms = 1U;
    }

    for (i = 1; i <= slices; ++i) {
        uint16_t mid_pulse[4] = {
            (uint16_t)motutu_lerp_i16((int16_t)start_pulse[0], (int16_t)pulse_us[0], (uint8_t)i, (uint8_t)slices),
            (uint16_t)motutu_lerp_i16((int16_t)start_pulse[1], (int16_t)pulse_us[1], (uint8_t)i, (uint8_t)slices),
            (uint16_t)motutu_lerp_i16((int16_t)start_pulse[2], (int16_t)pulse_us[2], (uint8_t)i, (uint8_t)slices),
            (uint16_t)motutu_lerp_i16((int16_t)start_pulse[3], (int16_t)pulse_us[3], (uint8_t)i, (uint8_t)slices),
        };

        for (joint = 0; joint < 4U; ++joint) {
            if (motutu_set_joint_pulse(joint, mid_pulse[joint]) != 0) {
                return -1;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(slice_ms));
    }

    return 0;
}

int motutu_play_sequence(const motutu_pose_t *seq, uint16_t count, bool loop, uint16_t loop_count)
{
    uint16_t pass = 0;
    uint16_t target_loops = loop ? (loop_count == 0U ? 1U : loop_count) : 1U;

    if (motutu_require_ready() != 0 || seq == NULL || count == 0U) {
        return -1;
    }

    while (pass < target_loops) {
        uint16_t i = 0;

        for (i = 0; i < count; ++i) {
            /* 用小步插值把关键帧过渡拉平，避免“机械舞”式的硬切。 */
            if (motutu_apply_pose_smooth(&seq[i], 6U, 35U) != 0) {
                return -1;
            }
            if (seq[i].hold_ms > 0U) {
                vTaskDelay(pdMS_TO_TICKS(seq[i].hold_ms));
            }
        }
        ++pass;
    }

    return 0;
}

int motutu_home(void)
{
    uint16_t pulse_us[4];

    if (motutu_require_ready() != 0) {
        return -1;
    }

    motutu_fill_stand_pulse_pose(pulse_us);
    return motutu_apply_pulse_pose_smooth(pulse_us, MOTUTU_HOME_TIME_MS);
}

static int motutu_reset_all_servos_to_default_pose(void)
{
    uint16_t abs_deg[4];

    motutu_fill_calibrate_abs_pose(abs_deg);

    if (motutu_apply_abs_pose(abs_deg) != 0) {
        return -1;
    }
    s_current_pose = s_pose_home;
    return 0;
}

int motutu_calibrate_pose(void)
{
    uint16_t pulse_us[4];

    if (motutu_require_ready() != 0) {
        return -1;
    }

    motutu_fill_calibrate_pulse_pose(pulse_us);
    return motutu_apply_pulse_pose_smooth(pulse_us, MOTUTU_CALIBRATE_TIME_MS);
}

static int motutu_finish_motion(void)
{
    int ret = motutu_stand();

    /* 动作结束统一回站立位，避免又回到装配校正姿态。 */
    if (ret == 0) {
        vTaskDelay(pdMS_TO_TICKS(180));
    }
    return ret;
}

static int motutu_play_gait_and_finish(const motutu_pose_t *seq, uint16_t count, uint16_t steps)
{
    uint16_t pass = 0;
    uint16_t target_loops = steps == 0U ? 1U : steps;
    int ret = 0;

    if (motutu_require_ready() != 0 || seq == NULL || count == 0U) {
        return -1;
    }

    /* 行走/转向类步态统一先回到站立位，再围绕站立位做动态偏移。
     * 否则如果上一个姿态还停在校正位，视觉上就会像“围着校正位来回摆动”。 */
    ret = motutu_stand();
    if (ret != 0) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    while (pass < target_loops) {
        uint16_t i = 0;

        for (i = 0; i < count; ++i) {
            uint16_t phase_ms = seq[i].hold_ms;

            if (phase_ms == 0U) {
                phase_ms = MOTUTU_SEQ_SLICE_MS;
            }
            ret = motutu_apply_pose_timed(&seq[i], phase_ms);
            if (ret != 0) {
                return ret;
            }
        }
        ++pass;
    }

    return motutu_finish_motion();
}

int motutu_stand(void)
{
    uint16_t pulse_us[4];

    if (motutu_require_ready() != 0) {
        return -1;
    }

    motutu_fill_stand_pulse_pose(pulse_us);
    return motutu_apply_pulse_pose_smooth(pulse_us, MOTUTU_STAND_TIME_MS);
}

int motutu_down(void)
{
    motutu_pose_t pose;

    if (motutu_require_ready() != 0) {
        return -1;
    }

    motutu_fill_stand_offset_pose(&pose,
                                  s_pose_down.lf_deg,
                                  s_pose_down.rf_deg,
                                  s_pose_down.lr_deg,
                                  s_pose_down.rr_deg,
                                  s_pose_down.hold_ms);
    return motutu_apply_pose_timed(&pose, s_pose_down.hold_ms);
}

int motutu_forward(uint8_t steps, motutu_speed_t speed)
{
    motutu_pose_t gait[MOTUTU_GAIT_PHASE_MAX];
    uint16_t count = 0U;

    if (steps == 0U) {
        steps = 1U;
    }
    if (motutu_build_gait_sequence(MOTUTU_GAIT_FORWARD, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
        return -1;
    }
    return motutu_play_gait_and_finish(gait, count, steps);
}

int motutu_backward(uint8_t steps, motutu_speed_t speed)
{
    motutu_pose_t gait[MOTUTU_GAIT_PHASE_MAX];
    uint16_t count = 0U;

    if (steps == 0U) {
        steps = 1U;
    }
    if (motutu_build_gait_sequence(MOTUTU_GAIT_BACKWARD, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
        return -1;
    }
    return motutu_play_gait_and_finish(gait, count, steps);
}

int motutu_turn_left(uint8_t steps, motutu_speed_t speed)
{
    motutu_pose_t gait[MOTUTU_GAIT_PHASE_MAX];
    uint16_t count = 0U;

    if (steps == 0U) {
        steps = 1U;
    }
    if (motutu_build_gait_sequence(MOTUTU_GAIT_TURN_LEFT, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
        return -1;
    }
    return motutu_play_gait_and_finish(gait, count, steps);
}

int motutu_turn_right(uint8_t steps, motutu_speed_t speed)
{
    motutu_pose_t gait[MOTUTU_GAIT_PHASE_MAX];
    uint16_t count = 0U;

    if (steps == 0U) {
        steps = 1U;
    }
    if (motutu_build_gait_sequence(MOTUTU_GAIT_TURN_RIGHT, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
        return -1;
    }
    return motutu_play_gait_and_finish(gait, count, steps);
}

int motutu_flip(void)
{
    motutu_pose_t seq[] = {
        {.lf_deg = 10, .rf_deg = 10, .lr_deg = -12, .rr_deg = -12, .hold_ms = 140},
        {.lf_deg = 18, .rf_deg = 18, .lr_deg = -28, .rr_deg = -28, .hold_ms = 160},
        {.lf_deg = 24, .rf_deg = 24, .lr_deg = -44, .rr_deg = -44, .hold_ms = 160},
        {.lf_deg = 28, .rf_deg = 28, .lr_deg = -60, .rr_deg = -60, .hold_ms = 120},
        {.lf_deg = 20, .rf_deg = 20, .lr_deg = -54, .rr_deg = -54, .hold_ms = 100},
        {.lf_deg = 28, .rf_deg = 28, .lr_deg = -60, .rr_deg = -60, .hold_ms = 120},
        {.lf_deg = 55, .rf_deg = 55, .lr_deg = 55, .rr_deg = 55, .hold_ms = 220},
        {.lf_deg = 48, .rf_deg = 48, .lr_deg = 48, .rr_deg = 48, .hold_ms = 120},
        {.lf_deg = 58, .rf_deg = 58, .lr_deg = 58, .rr_deg = 58, .hold_ms = 120},
        {.lf_deg = 50, .rf_deg = 50, .lr_deg = 50, .rr_deg = 50, .hold_ms = 120},
        {.lf_deg = -24, .rf_deg = -24, .lr_deg = 24, .rr_deg = 24, .hold_ms = 140},
        {.lf_deg = -52, .rf_deg = -52, .lr_deg = 52, .rr_deg = 52, .hold_ms = 160},
        {.lf_deg = -60, .rf_deg = -60, .lr_deg = 48, .rr_deg = 48, .hold_ms = 140},
        {.lf_deg = 18, .rf_deg = 18, .lr_deg = -16, .rr_deg = -16, .hold_ms = 120},
        {.lf_deg = -8, .rf_deg = -8, .lr_deg = 0, .rr_deg = 0, .hold_ms = 100},
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -12, .rr_deg = -12, .hold_ms = 80},
        s_pose_home_fast,
    };

    LOGW("motutu flip is experimental; success depends on center of mass and floor friction");
    motutu_offset_sequence_from_stand(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])));
    return motutu_play_sequence(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])), false, 1);
}

int motutu_wave_left(void)
{
    motutu_pose_t seq[] = {
        /* 单脚招手先让两条后腿同向轻微预压，再慢抬前腿，避免左前抬脚时朝左前方倾倒。 */
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 200},
        {.lf_deg = -12, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 160},
        {.lf_deg = -28, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = -58, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = -36, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 140},
        {.lf_deg = -60, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = -22, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 140},
        s_pose_home_fast,
    };

    motutu_offset_sequence_from_stand(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])));
    return motutu_play_sequence(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])), false, 1);
}

int motutu_wave_right(void)
{
    motutu_pose_t seq[] = {
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 200},
        {.lf_deg = 0, .rf_deg = -12, .lr_deg = -10, .rr_deg = -10, .hold_ms = 160},
        {.lf_deg = 0, .rf_deg = -28, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = -58, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = -36, .lr_deg = -10, .rr_deg = -10, .hold_ms = 140},
        {.lf_deg = 0, .rf_deg = -60, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = -22, .lr_deg = -6, .rr_deg = -6, .hold_ms = 140},
        s_pose_home_fast,
    };

    motutu_offset_sequence_from_stand(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])));
    return motutu_play_sequence(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])), false, 1);
}

int motutu_shake_left(void)
{
    motutu_pose_t seq[] = {
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 200},
        {.lf_deg = -20, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 140},
        {.lf_deg = -42, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 160},
        {.lf_deg = -58, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = -46, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = -58, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = -44, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = -58, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = -24, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 120},
        s_pose_home_fast,
    };

    motutu_offset_sequence_from_stand(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])));
    return motutu_play_sequence(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])), false, 1);
}

int motutu_shake_right(void)
{
    motutu_pose_t seq[] = {
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 200},
        {.lf_deg = 0, .rf_deg = -20, .lr_deg = -10, .rr_deg = -10, .hold_ms = 140},
        {.lf_deg = 0, .rf_deg = -42, .lr_deg = -10, .rr_deg = -10, .hold_ms = 160},
        {.lf_deg = 0, .rf_deg = -58, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = -46, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = 0, .rf_deg = -58, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = 0, .rf_deg = -44, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = 0, .rf_deg = -58, .lr_deg = -10, .rr_deg = -10, .hold_ms = 100},
        {.lf_deg = 0, .rf_deg = -24, .lr_deg = -6, .rr_deg = -6, .hold_ms = 120},
        s_pose_home_fast,
    };

    motutu_offset_sequence_from_stand(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])));
    return motutu_play_sequence(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])), false, 1);
}

int motutu_spin_left(uint8_t steps, motutu_speed_t speed)
{
    motutu_pose_t gait[MOTUTU_GAIT_PHASE_MAX];
    uint16_t count = 0U;
    uint16_t loops;

    if (steps == 0U) {
        steps = 1U;
    }
    if (motutu_build_gait_sequence(MOTUTU_GAIT_TURN_LEFT, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
        return -1;
    }
    loops = (uint16_t)steps * 5U;
    if (loops == 0U) {
        loops = 5U;
    }
    return motutu_play_gait_and_finish(gait, count, loops);
}

int motutu_spin_right(uint8_t steps, motutu_speed_t speed)
{
    motutu_pose_t gait[MOTUTU_GAIT_PHASE_MAX];
    uint16_t count = 0U;
    uint16_t loops;

    if (steps == 0U) {
        steps = 1U;
    }
    if (motutu_build_gait_sequence(MOTUTU_GAIT_TURN_RIGHT, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
        return -1;
    }
    loops = (uint16_t)steps * 5U;
    if (loops == 0U) {
        loops = 5U;
    }
    return motutu_play_gait_and_finish(gait, count, loops);
}

int motutu_serve_left(void)
{
    motutu_pose_t seq[] = {
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 180},
        {.lf_deg = -18, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = -38, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 160},
        {.lf_deg = -56, .rf_deg = 0, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = 10, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 120},
        {.lf_deg = -2, .rf_deg = 0, .lr_deg = -2, .rr_deg = -2, .hold_ms = 80},
        s_pose_home_fast,
    };

    motutu_offset_sequence_from_stand(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])));
    return motutu_play_sequence(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])), false, 1);
}

int motutu_serve_right(void)
{
    motutu_pose_t seq[] = {
        {.lf_deg = 0, .rf_deg = 0, .lr_deg = -6, .rr_deg = -6, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = -18, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = -38, .lr_deg = -10, .rr_deg = -10, .hold_ms = 160},
        {.lf_deg = 0, .rf_deg = -56, .lr_deg = -10, .rr_deg = -10, .hold_ms = 180},
        {.lf_deg = 0, .rf_deg = 10, .lr_deg = -6, .rr_deg = -6, .hold_ms = 120},
        {.lf_deg = 0, .rf_deg = -2, .lr_deg = -2, .rr_deg = -2, .hold_ms = 80},
        s_pose_home_fast,
    };

    motutu_offset_sequence_from_stand(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])));
    return motutu_play_sequence(seq, (uint16_t)(sizeof(seq) / sizeof(seq[0])), false, 1);
}

int motutu_motion_init(void)
{
    int ret = servo_bus_init();

    if (ret != 0) {
        s_motutu_ready = false;
        return ret;
    }

    s_motutu_ready = true;
    s_current_pose = s_pose_home;
    ret = motutu_reset_all_servos_to_default_pose();
    LOGW("motutu: ready (PA04-07 -> lf/rf/lr/rr, init abs=%u/%u/%u/%u)",
         (unsigned int)MOTUTU_LF_CENTER_ABS_DEG,
         (unsigned int)MOTUTU_RF_CENTER_ABS_DEG,
         (unsigned int)MOTUTU_LR_CENTER_ABS_DEG,
         (unsigned int)MOTUTU_RR_CENTER_ABS_DEG);
    return ret;
}

static bool motutu_seq_get_joint_index_from_key(const char *key, uint8_t *out_index)
{
    static const char *const names[4] = {"lf", "rf", "lr", "rr"};
    uint8_t i = 0;

    if (key == NULL || out_index == NULL) {
        return false;
    }

    for (i = 0; i < 4U; ++i) {
        if (strcmp(key, names[i]) == 0) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

int motutu_execute_sequence_json(const char *json_str)
{
    cJSON *root = NULL;
    cJSON *actions = NULL;
    cJSON *top_delay_item = NULL;
    int top_delay_ms = 0;
    int i = 0;

    if (motutu_require_ready() != 0 || json_str == NULL || json_str[0] == '\0') {
        return -1;
    }

    root = cJSON_Parse(json_str);
    if (root == NULL) {
        LOGE("motutu seq json parse failed: %s", cJSON_GetErrorPtr());
        return -1;
    }

    actions = cJSON_GetObjectItem(root, "a");
    if (!cJSON_IsArray(actions)) {
        LOGE("motutu seq invalid: top-level 'a' is not array");
        cJSON_Delete(root);
        return -1;
    }

    top_delay_item = cJSON_GetObjectItem(root, "d");
    if (cJSON_IsNumber(top_delay_item)) {
        top_delay_ms = top_delay_item->valueint;
        if (top_delay_ms < 0) {
            top_delay_ms = 0;
        }
    }

    for (i = 0; i < cJSON_GetArraySize(actions); ++i) {
        cJSON *item = cJSON_GetArrayItem(actions, i);
        cJSON *servos_item;
        cJSON *pulse_item;
        cJSON *move_item;
        cJSON *delay_item;
        uint16_t target_abs[4];
        uint16_t target_pulse[4];
        int move_ms = 300;
        int delay_ms = 0;
        cJSON *servo_value = NULL;
        bool has_abs_cmd = false;
        bool has_pulse_cmd = false;

        if (!cJSON_IsObject(item)) {
            continue;
        }

        target_abs[0] = s_current_abs_deg[0];
        target_abs[1] = s_current_abs_deg[1];
        target_abs[2] = s_current_abs_deg[2];
        target_abs[3] = s_current_abs_deg[3];
        target_pulse[0] = s_current_pulse_us[0];
        target_pulse[1] = s_current_pulse_us[1];
        target_pulse[2] = s_current_pulse_us[2];
        target_pulse[3] = s_current_pulse_us[3];

        servos_item = cJSON_GetObjectItem(item, "s");
        pulse_item = cJSON_GetObjectItem(item, "p");
        if (!cJSON_IsObject(servos_item) && !cJSON_IsObject(pulse_item)) {
            LOGE("motutu seq item[%d] invalid: 's' missing", i);
            cJSON_Delete(root);
            return -1;
        }

        if (cJSON_IsObject(servos_item)) {
            for (servo_value = servos_item->child; servo_value != NULL; servo_value = servo_value->next) {
                uint8_t joint_index = 0;

                if (!cJSON_IsNumber(servo_value)) {
                    continue;
                }
                if (!motutu_seq_get_joint_index_from_key(servo_value->string, &joint_index)) {
                    continue;
                }

                has_abs_cmd = true;
                if (servo_value->valueint < 0) {
                    target_abs[joint_index] = 0U;
                } else if (servo_value->valueint > MOTUTU_SERVO_ABS_MAX_DEG) {
                    target_abs[joint_index] = MOTUTU_SERVO_ABS_MAX_DEG;
                } else {
                    target_abs[joint_index] = (uint16_t)servo_value->valueint;
                }
                target_pulse[joint_index] = motutu_abs_to_servo_pulse(target_abs[joint_index]);
            }
        }

        if (cJSON_IsObject(pulse_item)) {
            for (servo_value = pulse_item->child; servo_value != NULL; servo_value = servo_value->next) {
                uint8_t joint_index = 0;

                if (!cJSON_IsNumber(servo_value)) {
                    continue;
                }
                if (!motutu_seq_get_joint_index_from_key(servo_value->string, &joint_index)) {
                    continue;
                }

                has_pulse_cmd = true;
                if (servo_value->valueint < s_joint_cfgs[joint_index].pulse_min_us) {
                    target_pulse[joint_index] = s_joint_cfgs[joint_index].pulse_min_us;
                } else if (servo_value->valueint > s_joint_cfgs[joint_index].pulse_max_us) {
                    target_pulse[joint_index] = s_joint_cfgs[joint_index].pulse_max_us;
                } else {
                    target_pulse[joint_index] = (uint16_t)servo_value->valueint;
                }
            }
        }

        move_item = cJSON_GetObjectItem(item, "v");
        if (cJSON_IsNumber(move_item)) {
            move_ms = move_item->valueint;
            if (move_ms < 20) {
                move_ms = 20;
            } else if (move_ms > 5000) {
                move_ms = 5000;
            }
        }

        delay_item = cJSON_GetObjectItem(item, "d");
        if (cJSON_IsNumber(delay_item)) {
            delay_ms = delay_item->valueint;
            if (delay_ms < 0) {
                delay_ms = 0;
            } else if (delay_ms > 10000) {
                delay_ms = 10000;
            }
        }

        if (has_pulse_cmd) {
            if (motutu_apply_pulse_pose_smooth(target_pulse, (uint16_t)move_ms) != 0) {
                LOGE("motutu seq item[%d] apply pulse failed", i);
                cJSON_Delete(root);
                return -1;
            }
        } else if (has_abs_cmd && motutu_apply_abs_pose_smooth(target_abs, (uint16_t)move_ms) != 0) {
            LOGE("motutu seq item[%d] apply failed", i);
            cJSON_Delete(root);
            return -1;
        }

        if (delay_ms > 0 && i < cJSON_GetArraySize(actions) - 1) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)delay_ms));
        }
    }

    if (top_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)top_delay_ms));
    }

    cJSON_Delete(root);
    return 0;
}

static motutu_speed_t motutu_parse_speed(const char *text)
{
    if (text == NULL) {
        return MOTUTU_SPEED_NORMAL;
    }
    if (strcmp(text, "slow") == 0 || strcmp(text, "low") == 0) {
        return MOTUTU_SPEED_SLOW;
    }
    if (strcmp(text, "fast") == 0 || strcmp(text, "high") == 0) {
        return MOTUTU_SPEED_FAST;
    }
    return MOTUTU_SPEED_NORMAL;
}

static int dog_cmd_handler(int argc, char *argv[])
{
    Shell *sh = shellGetCurrent();
    int ret = 0;

    if (argc < 2) {
        shellPrint(sh, "dog: ready=%d\n", motutu_motion_is_ready());
        shellPrint(sh, "usage: dog home|stand|down|init|calibrate|url|page|pose <current|lf rf lr rr>|joint <lf|rf|lr|rr|1-4> <deg>|pulse <lf|rf|lr|rr|1-4> <us>|servo <lf|rf|lr|rr|1-4> <0-270>|test <lf|rf|lr|rr|1-4>|gait show <walk|back|left|right> [slow|normal|fast]|gait <walk|back|left|right> <phase> [slow|normal|fast]|seq <json>|calib show|calib dir <joint> <1|-1>|calib center <joint> <us>|calib limit <joint> <min_us> <max_us>|calib angle <joint> <min_deg> <max_deg>|walk [steps] [slow|normal|fast]|back [steps] [spd]|left [steps] [spd]|right [steps] [spd]|spin_left [steps] [spd]|spin_right [steps] [spd]|wave_left|wave_right|shake_left|shake_right|serve_left|serve_right|flip\n");
        return 0;
    }

    if (strcmp(argv[1], "home") == 0) {
        ret = motutu_home();
    } else if (strcmp(argv[1], "stand") == 0) {
        ret = motutu_stand();
    } else if (strcmp(argv[1], "down") == 0) {
        ret = motutu_down();
    } else if (strcmp(argv[1], "init") == 0 || strcmp(argv[1], "calibrate") == 0) {
        ret = motutu_calibrate_pose();
    } else if (strcmp(argv[1], "flip") == 0) {
        ret = motutu_flip();
    } else if (strcmp(argv[1], "wave_left") == 0) {
        ret = motutu_wave_left();
    } else if (strcmp(argv[1], "wave_right") == 0) {
        ret = motutu_wave_right();
    } else if (strcmp(argv[1], "shake_left") == 0) {
        ret = motutu_shake_left();
    } else if (strcmp(argv[1], "shake_right") == 0) {
        ret = motutu_shake_right();
    } else if (strcmp(argv[1], "serve_left") == 0) {
        ret = motutu_serve_left();
    } else if (strcmp(argv[1], "serve_right") == 0) {
        ret = motutu_serve_right();
    } else if (strcmp(argv[1], "url") == 0) {
        char url_buf[48];

        if (!service_motutu_web_get_control_url(url_buf, sizeof(url_buf))) {
            shellPrint(sh, "dog url: unavailable (wifi not ready)\n");
            return -1;
        }
        shellPrint(sh, "dog url: %s\n", url_buf);
        return 0;
    } else if (strcmp(argv[1], "page") == 0) {
        ret = service_motutu_web_show_control_page();
        shellPrint(sh, "dog page ret=%d\n", ret);
        return ret;
    } else if (strcmp(argv[1], "pose") == 0 && argc >= 3 && strcmp(argv[2], "current") == 0) {
        shellPrint(sh, "dog pose current: logical lf=%d rf=%d lr=%d rr=%d | abs lf=%u rf=%u lr=%u rr=%u | pulse lf=%u rf=%u lr=%u rr=%u\n",
                   (int)s_current_pose.lf_deg,
                   (int)s_current_pose.rf_deg,
                   (int)s_current_pose.lr_deg,
                   (int)s_current_pose.rr_deg,
                   (unsigned int)s_current_abs_deg[0],
                   (unsigned int)s_current_abs_deg[1],
                   (unsigned int)s_current_abs_deg[2],
                   (unsigned int)s_current_abs_deg[3],
                   (unsigned int)s_current_pulse_us[0],
                   (unsigned int)s_current_pulse_us[1],
                   (unsigned int)s_current_pulse_us[2],
                   (unsigned int)s_current_pulse_us[3]);
        return 0;
    } else if (strcmp(argv[1], "pose") == 0 && argc >= 6) {
        motutu_pose_t pose = {
            .lf_deg = (int16_t)atoi(argv[2]),
            .rf_deg = (int16_t)atoi(argv[3]),
            .lr_deg = (int16_t)atoi(argv[4]),
            .rr_deg = (int16_t)atoi(argv[5]),
            .hold_ms = 0,
        };
        ret = motutu_apply_pose(&pose);
    } else if (strcmp(argv[1], "joint") == 0 && argc >= 4) {
        int joint_index = motutu_parse_joint_index(argv[2]);
        int deg = atoi(argv[3]);

        if (joint_index < 0) {
            shellPrint(sh, "dog: invalid joint %s\n", argv[2]);
            return -1;
        }
        ret = motutu_set_joint_angle((uint8_t)joint_index, (int16_t)deg);
        shellPrint(sh, "dog: joint %s deg=%d ret=%d\n", motutu_joint_name((uint8_t)joint_index), deg, ret);
        return 0;
    } else if (strcmp(argv[1], "pulse") == 0 && argc >= 4) {
        int joint_index = motutu_parse_joint_index(argv[2]);
        int pulse_us = atoi(argv[3]);

        if (joint_index < 0) {
            shellPrint(sh, "dog: invalid joint %s\n", argv[2]);
            return -1;
        }
        ret = motutu_set_joint_pulse((uint8_t)joint_index, (uint16_t)pulse_us);
        shellPrint(sh, "dog: pulse %s us=%d ret=%d\n", motutu_joint_name((uint8_t)joint_index), pulse_us, ret);
        return 0;
    } else if (strcmp(argv[1], "servo") == 0 && argc >= 4) {
        int joint_index = motutu_parse_joint_index(argv[2]);
        int deg = atoi(argv[3]);

        if (joint_index < 0) {
            shellPrint(sh, "dog: invalid joint %s\n", argv[2]);
            return -1;
        }
        ret = motutu_set_joint_abs_angle((uint8_t)joint_index, (uint16_t)deg);
        shellPrint(sh, "dog: servo %s abs=%d ret=%d\n", motutu_joint_name((uint8_t)joint_index), deg, ret);
        return 0;
    } else if (strcmp(argv[1], "test") == 0 && argc >= 3) {
        int joint_index = motutu_parse_joint_index(argv[2]);
        static const uint16_t seq[] = {90U, 60U, 120U, 90U};
        size_t i = 0;

        if (joint_index < 0) {
            shellPrint(sh, "dog: invalid joint %s\n", argv[2]);
            return -1;
        }

        for (i = 0; i < sizeof(seq) / sizeof(seq[0]); ++i) {
            ret = motutu_set_joint_abs_angle((uint8_t)joint_index, seq[i]);
            if (ret != 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        shellPrint(sh, "dog: test %s ret=%d\n", motutu_joint_name((uint8_t)joint_index), ret);
        return 0;
    } else if (strcmp(argv[1], "seq") == 0 && argc >= 3) {
        char json_buf[MOTUTU_SEQ_JSON_MAX];
        size_t used = 0;
        int i = 0;

        json_buf[0] = '\0';
        for (i = 2; i < argc; ++i) {
            size_t part_len = strlen(argv[i]);
            size_t extra = part_len + ((i > 2) ? 1U : 0U);

            if (used + extra >= sizeof(json_buf)) {
                shellPrint(sh, "dog: seq json too long (max %u)\n", (unsigned int)(sizeof(json_buf) - 1U));
                return -1;
            }

            if (i > 2) {
                json_buf[used++] = ' ';
                json_buf[used] = '\0';
            }
            memcpy(&json_buf[used], argv[i], part_len);
            used += part_len;
            json_buf[used] = '\0';
        }

        ret = motutu_execute_sequence_json(json_buf);
        shellPrint(sh, "dog: seq ret=%d\n", ret);
        return 0;
    } else if (strcmp(argv[1], "gait") == 0 && argc >= 4) {
        motutu_pose_t gait[MOTUTU_GAIT_PHASE_MAX];
        uint16_t count = 0U;
        motutu_speed_t speed = MOTUTU_SPEED_NORMAL;
        motutu_gait_kind_t kind;

        if (strcmp(argv[2], "show") == 0) {
            uint16_t i;

            if (!motutu_parse_gait_kind(argv[3], &kind)) {
                shellPrint(sh, "dog: invalid gait %s\n", argv[3]);
                return -1;
            }
            if (argc >= 5) {
                speed = motutu_parse_speed(argv[4]);
            }
            if (motutu_build_gait_sequence(kind, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
                shellPrint(sh, "dog: gait build failed\n");
                return -1;
            }
            shellPrint(sh, "dog gait %s speed=%s phases=%u\n",
                       motutu_gait_name(kind),
                       (speed == MOTUTU_SPEED_SLOW) ? "slow" :
                       (speed == MOTUTU_SPEED_FAST) ? "fast" : "normal",
                       (unsigned int)count);
            for (i = 0; i < count; ++i) {
                shellPrint(sh, "  phase %u: lf=%d rf=%d lr=%d rr=%d hold=%u\n",
                           (unsigned int)(i + 1U),
                           (int)gait[i].lf_deg,
                           (int)gait[i].rf_deg,
                           (int)gait[i].lr_deg,
                           (int)gait[i].rr_deg,
                           (unsigned int)gait[i].hold_ms);
            }
            return 0;
        }

        if (!motutu_parse_gait_kind(argv[2], &kind)) {
            shellPrint(sh, "dog: invalid gait %s\n", argv[2]);
            return -1;
        }
        if (argc >= 5) {
            speed = motutu_parse_speed(argv[4]);
        }
        if (motutu_build_gait_sequence(kind, speed, gait, MOTUTU_GAIT_PHASE_MAX, &count) != 0) {
            shellPrint(sh, "dog: gait build failed\n");
            return -1;
        }

        {
            int phase = atoi(argv[3]);
            const motutu_pose_t *pose;

            if (phase <= 0 || (uint16_t)phase > count) {
                shellPrint(sh, "dog: invalid phase %d, expect 1..%u\n", phase, (unsigned int)count);
                return -1;
            }

            pose = &gait[(uint16_t)phase - 1U];
            ret = motutu_apply_pose_smooth(pose, 6U, 35U);
            shellPrint(sh, "dog gait %s phase=%d lf=%d rf=%d lr=%d rr=%d hold=%u ret=%d\n",
                       motutu_gait_name(kind),
                       phase,
                       (int)pose->lf_deg,
                       (int)pose->rf_deg,
                       (int)pose->lr_deg,
                       (int)pose->rr_deg,
                       (unsigned int)pose->hold_ms,
                       ret);
            return ret;
        }
    } else if (strcmp(argv[1], "calib") == 0 && argc >= 3) {
        if (strcmp(argv[2], "show") == 0) {
            uint8_t i = 0;
            for (i = 0; i < 4U; ++i) {
                shellPrint(sh,
                           "dog calib %s(id=%u): dir=%d center=%u min_us=%u max_us=%u min_deg=%d max_deg=%d\n",
                           motutu_joint_name(i),
                           (unsigned int)s_joint_cfgs[i].id,
                           (int)s_joint_cfgs[i].dir,
                           (unsigned int)s_joint_cfgs[i].pulse_center_us,
                           (unsigned int)s_joint_cfgs[i].pulse_min_us,
                           (unsigned int)s_joint_cfgs[i].pulse_max_us,
                           (int)s_joint_cfgs[i].angle_min_deg,
                           (int)s_joint_cfgs[i].angle_max_deg);
            }
            shellPrint(sh, "dog pose current: lf=%d rf=%d lr=%d rr=%d\n",
                       (int)s_current_pose.lf_deg,
                       (int)s_current_pose.rf_deg,
                       (int)s_current_pose.lr_deg,
                       (int)s_current_pose.rr_deg);
            return 0;
        }

        if (argc < 5) {
            shellPrint(sh, "dog: calib args not enough\n");
            return -1;
        }

        {
            int joint_index = motutu_parse_joint_index(argv[3]);
            if (joint_index < 0) {
                shellPrint(sh, "dog: invalid joint %s\n", argv[3]);
                return -1;
            }

            if (strcmp(argv[2], "dir") == 0) {
                int dir = atoi(argv[4]);
                s_joint_cfgs[joint_index].dir = (dir >= 0) ? 1 : -1;
                shellPrint(sh, "dog calib %s dir=%d\n", motutu_joint_name((uint8_t)joint_index), (int)s_joint_cfgs[joint_index].dir);
                return 0;
            }
            if (strcmp(argv[2], "center") == 0) {
                s_joint_cfgs[joint_index].pulse_center_us = (uint16_t)atoi(argv[4]);
                shellPrint(sh, "dog calib %s center=%u\n", motutu_joint_name((uint8_t)joint_index),
                           (unsigned int)s_joint_cfgs[joint_index].pulse_center_us);
                return 0;
            }
            if (strcmp(argv[2], "limit") == 0 && argc >= 6) {
                s_joint_cfgs[joint_index].pulse_min_us = (uint16_t)atoi(argv[4]);
                s_joint_cfgs[joint_index].pulse_max_us = (uint16_t)atoi(argv[5]);
                shellPrint(sh, "dog calib %s limit=%u..%u\n", motutu_joint_name((uint8_t)joint_index),
                           (unsigned int)s_joint_cfgs[joint_index].pulse_min_us,
                           (unsigned int)s_joint_cfgs[joint_index].pulse_max_us);
                return 0;
            }
            if (strcmp(argv[2], "angle") == 0 && argc >= 6) {
                s_joint_cfgs[joint_index].angle_min_deg = (int16_t)atoi(argv[4]);
                s_joint_cfgs[joint_index].angle_max_deg = (int16_t)atoi(argv[5]);
                shellPrint(sh, "dog calib %s angle=%d..%d\n", motutu_joint_name((uint8_t)joint_index),
                           (int)s_joint_cfgs[joint_index].angle_min_deg,
                           (int)s_joint_cfgs[joint_index].angle_max_deg);
                return 0;
            }
        }

        shellPrint(sh, "dog: invalid calib args\n");
        return -1;
    } else if (strcmp(argv[1], "walk") == 0 || strcmp(argv[1], "back") == 0 ||
               strcmp(argv[1], "left") == 0 || strcmp(argv[1], "right") == 0 ||
               strcmp(argv[1], "spin_left") == 0 || strcmp(argv[1], "spin_right") == 0) {
        uint8_t steps = 1U;
        motutu_speed_t speed = MOTUTU_SPEED_NORMAL;

        if (argc >= 3) {
            steps = (uint8_t)atoi(argv[2]);
            if (steps == 0U) {
                steps = 1U;
            }
        }
        if (argc >= 4) {
            speed = motutu_parse_speed(argv[3]);
        }

        if (strcmp(argv[1], "walk") == 0) {
            ret = motutu_forward(steps, speed);
        } else if (strcmp(argv[1], "back") == 0) {
            ret = motutu_backward(steps, speed);
        } else if (strcmp(argv[1], "left") == 0) {
            ret = motutu_turn_left(steps, speed);
        } else if (strcmp(argv[1], "spin_left") == 0) {
            ret = motutu_spin_left(steps, speed);
        } else if (strcmp(argv[1], "spin_right") == 0) {
            ret = motutu_spin_right(steps, speed);
        } else {
            ret = motutu_turn_right(steps, speed);
        }
    } else {
        shellPrint(sh, "dog: invalid args\n");
        return -1;
    }

    shellPrint(sh, "dog: %s ret=%d\n", argv[1], ret);
    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 dog, dog_cmd_handler, motutu dog motion debug);
