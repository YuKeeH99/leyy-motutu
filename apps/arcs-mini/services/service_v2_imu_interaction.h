#ifndef SERVICE_V2_IMU_INTERACTION_H
#define SERVICE_V2_IMU_INTERACTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVICE_V2_IMU_POSE_UNKNOWN = 0,
    SERVICE_V2_IMU_POSE_NORMAL,
    SERVICE_V2_IMU_POSE_UPSIDE_DOWN,
    SERVICE_V2_IMU_POSE_OTHER,
} service_v2_imu_pose_t;

typedef enum {
    SERVICE_V2_IMU_EVENT_NONE = 0,
    SERVICE_V2_IMU_EVENT_UPSIDE_DOWN,
    SERVICE_V2_IMU_EVENT_TABLE_SHAKE,
} service_v2_imu_event_t;

typedef struct {
    bool ready;
    bool inhibited;
    service_v2_imu_pose_t pose;
    service_v2_imu_event_t last_event;
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    uint32_t sample_count;
    uint32_t imu_error_count;
    uint32_t upside_down_count;
    uint32_t reverse_home_suppressed_count;
    uint32_t table_shake_count;
    uint32_t feedback_played_count;
    uint32_t feedback_suppressed_count;
    uint32_t feedback_missing_count;
    uint32_t shake_peak_gyro_lsb;
    uint32_t quiet_remaining_ms;
    uint32_t cooldown_remaining_ms;
} service_v2_imu_interaction_status_t;

/**
 * Start the 20 ms MPU6050 interaction worker.
 *
 * The worker emits local voice feedback and queues the bounded upside-down
 * leg gesture. It never writes servo frames directly, and suppresses
 * detection while either executor is busy.
 */
int service_v2_imu_interaction_init(void);

/** Pause or resume MPU6050 sampling and local IMU reactions. */
int service_v2_imu_interaction_set_enabled(int enabled);

/** Copy the latest detector state and counters. */
int service_v2_imu_interaction_get_status(service_v2_imu_interaction_status_t *status);

const char *service_v2_imu_interaction_pose_name(service_v2_imu_pose_t pose);
const char *service_v2_imu_interaction_event_name(service_v2_imu_event_t event);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_V2_IMU_INTERACTION_H */
