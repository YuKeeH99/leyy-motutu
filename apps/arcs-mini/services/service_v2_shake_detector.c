#include "service_v2_shake_detector.h"

#include <stddef.h>
#include <string.h>

#define SERVICE_V2_SHAKE_MAX_SAMPLE_GAP_MS 200U
#define SERVICE_V2_SHAKE_ROTATION_LSB_MS \
    ((uint64_t)SERVICE_V2_SHAKE_ROTATION_DEGREES * \
     SERVICE_V2_SHAKE_GYRO_LSB_PER_DPS * 1000U)

static uint32_t shake_abs_i16(int16_t value)
{
    int32_t wide = value;
    return (uint32_t)(wide < 0 ? -wide : wide);
}

static void shake_clear_window(service_v2_shake_detector_t *detector)
{
    detector->window_active = false;
    detector->window_start_ms = 0U;
    detector->last_motion_ms = 0U;
    detector->angle_lsb_ms = 0U;
    detector->peak_gyro_lsb = 0U;
    detector->rotation_count = 0U;
}

void service_v2_shake_detector_reset(service_v2_shake_detector_t *detector,
                                     bool reset_baseline)
{
    (void)reset_baseline;

    if (detector == NULL) {
        return;
    }
    memset(detector, 0, sizeof(*detector));
}

bool service_v2_shake_detector_update(service_v2_shake_detector_t *detector,
                                      uint32_t now_ms,
                                      int16_t gyro_x,
                                      int16_t gyro_y,
                                      int16_t gyro_z,
                                      uint32_t *trigger_peak_gyro_lsb)
{
    if (detector == NULL || trigger_peak_gyro_lsb == NULL) {
        return false;
    }

    *trigger_peak_gyro_lsb = 0U;
    if (!detector->timestamp_valid) {
        detector->timestamp_valid = true;
        detector->last_sample_ms = now_ms;
        return false;
    }

    uint32_t elapsed_ms = (uint32_t)(now_ms - detector->last_sample_ms);
    detector->last_sample_ms = now_ms;
    if (elapsed_ms == 0U) {
        return false;
    }
    if (elapsed_ms > SERVICE_V2_SHAKE_MAX_SAMPLE_GAP_MS) {
        shake_clear_window(detector);
        return false;
    }

    uint32_t roll_lsb = shake_abs_i16(gyro_x);
    uint32_t pitch_lsb = shake_abs_i16(gyro_y);
    uint32_t yaw_lsb = shake_abs_i16(gyro_z);
    uint32_t dominant_lsb = roll_lsb > pitch_lsb ? roll_lsb : pitch_lsb;
    if (yaw_lsb > dominant_lsb) {
        dominant_lsb = yaw_lsb;
    }

    if (detector->window_active &&
        (uint32_t)(now_ms - detector->last_motion_ms) > SERVICE_V2_SHAKE_MAX_QUIET_MS) {
        shake_clear_window(detector);
    }
    if (dominant_lsb <= SERVICE_V2_SHAKE_GYRO_DEADBAND_LSB) {
        return false;
    }

    if (!detector->window_active) {
        detector->window_active = true;
        detector->window_start_ms = now_ms;
        /* The previous sample may have been still; do not credit that gap. */
        elapsed_ms = 0U;
    }
    detector->last_motion_ms = now_ms;

    /* Keep the angle qualification latched while waiting for the minimum
     * duration. Saturation avoids wrapping during a long motion episode. */
    if (detector->rotation_count < SERVICE_V2_SHAKE_REQUIRED_ROTATIONS) {
        detector->angle_lsb_ms += (uint64_t)dominant_lsb * elapsed_ms;
        if (detector->angle_lsb_ms >= SERVICE_V2_SHAKE_ROTATION_LSB_MS) {
            detector->angle_lsb_ms -= SERVICE_V2_SHAKE_ROTATION_LSB_MS;
            detector->rotation_count++;
        }
    }
    if (dominant_lsb > detector->peak_gyro_lsb) {
        detector->peak_gyro_lsb = (uint16_t)dominant_lsb;
    }

    if (detector->rotation_count >= SERVICE_V2_SHAKE_REQUIRED_ROTATIONS &&
        (uint32_t)(now_ms - detector->window_start_ms) >= SERVICE_V2_SHAKE_WINDOW_MS) {
        *trigger_peak_gyro_lsb = detector->peak_gyro_lsb;
        shake_clear_window(detector);
        return true;
    }

    return false;
}
