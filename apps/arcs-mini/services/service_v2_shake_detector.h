#ifndef SERVICE_V2_SHAKE_DETECTOR_H
#define SERVICE_V2_SHAKE_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MPU6050 gyro full scale is configured to +/-250 dps. */
#define SERVICE_V2_SHAKE_GYRO_LSB_PER_DPS       131U
#define SERVICE_V2_SHAKE_GYRO_DEADBAND_LSB     3930U /* 30 dps */
#define SERVICE_V2_SHAKE_WINDOW_MS              5000U /* minimum continuous duration */
#define SERVICE_V2_SHAKE_MAX_QUIET_MS             200U
#define SERVICE_V2_SHAKE_ROTATION_DEGREES        360U
#define SERVICE_V2_SHAKE_REQUIRED_ROTATIONS        1U

typedef struct {
    bool timestamp_valid;
    bool window_active;
    uint32_t last_sample_ms;
    uint32_t window_start_ms;
    uint32_t last_motion_ms;
    uint64_t angle_lsb_ms;
    uint16_t peak_gyro_lsb;
    uint8_t rotation_count;
} service_v2_shake_detector_t;

void service_v2_shake_detector_reset(service_v2_shake_detector_t *detector,
                                     bool reset_baseline);

/**
 * Require at least five seconds of continuous significant motion and at least
 * 360 degrees of accumulated rotation. Quiet gaps over 200 ms restart both
 * conditions; brief direction reversals are tolerated.
 *
 * X/Y/Z rotations are all eligible. For diagonal rotation only the dominant
 * axis is integrated, so one physical movement cannot count twice.
 */
bool service_v2_shake_detector_update(service_v2_shake_detector_t *detector,
                                      uint32_t now_ms,
                                      int16_t gyro_x,
                                      int16_t gyro_y,
                                      int16_t gyro_z,
                                      uint32_t *trigger_peak_gyro_lsb);

#ifdef __cplusplus
}
#endif

#endif
