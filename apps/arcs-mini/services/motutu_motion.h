#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOTUTU_SPEED_SLOW = 0,
    MOTUTU_SPEED_NORMAL = 1,
    MOTUTU_SPEED_FAST = 2,
} motutu_speed_t;

typedef struct {
    int16_t lf_deg;
    int16_t rf_deg;
    int16_t lr_deg;
    int16_t rr_deg;
    uint16_t hold_ms;
} motutu_pose_t;

typedef struct {
    uint8_t id;
    int8_t dir;
    uint16_t center_abs_deg;
    uint16_t pulse_center_us;
    uint16_t pulse_min_us;
    uint16_t pulse_max_us;
    int16_t angle_min_deg;
    int16_t angle_max_deg;
} motutu_joint_info_t;

int motutu_motion_init(void);
int motutu_motion_is_ready(void);
int motutu_get_current_pose(motutu_pose_t *out_pose);
int motutu_get_current_abs_pose(uint16_t out_abs_deg[4]);
int motutu_get_joint_info(uint8_t joint_index, motutu_joint_info_t *out_info);

int motutu_apply_pose(const motutu_pose_t *pose);
int motutu_play_sequence(const motutu_pose_t *seq, uint16_t count, bool loop, uint16_t loop_count);
int motutu_execute_sequence_json(const char *json_str);

int motutu_calibrate_pose(void);
int motutu_home(void);
int motutu_stand(void);
int motutu_down(void);
int motutu_forward(uint8_t steps, motutu_speed_t speed);
int motutu_backward(uint8_t steps, motutu_speed_t speed);
int motutu_turn_left(uint8_t steps, motutu_speed_t speed);
int motutu_turn_right(uint8_t steps, motutu_speed_t speed);
int motutu_flip(void);
int motutu_wave_left(void);
int motutu_wave_right(void);
int motutu_shake_left(void);
int motutu_shake_right(void);
int motutu_spin_left(uint8_t steps, motutu_speed_t speed);
int motutu_spin_right(uint8_t steps, motutu_speed_t speed);
int motutu_serve_left(void);
int motutu_serve_right(void);

#ifdef __cplusplus
}
#endif
