#include "service_v2_imu_interaction.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "app_tone.h"
#include "lisa_log.h"
#include "lisa_mutex.h"
#include "lisa_thread.h"
#include "tone.h"
#include "voice_msg.h"
#include "voice_player.h"

#include "service_v2_actuators.h"
#include "service_v2_gait.h"
#include "service_v2_rgb_effect.h"
#include "service_v2_sensors.h"
#include "service_v2_shake_detector.h"

#define TAG "v2.imu_interaction"

/* MPU6050 is configured by service_v2_sensors for +/-2 g: 1 g = 16384 LSB. */
#define IMU_INTERACTION_POLL_MS                    20U
#define IMU_INTERACTION_TASK_STACK               3072U
#define IMU_INTERACTION_FEEDBACK_TASK_STACK      2048U
#define IMU_INTERACTION_FEEDBACK_QUEUE_DEPTH        2U
#define IMU_INTERACTION_FEEDBACK_MAX_AGE_MS       1000U
#define IMU_INTERACTION_POST_MOTION_QUIET_MS     1500U
#define IMU_INTERACTION_EVENT_COOLDOWN_MS        5000U
#define IMU_INTERACTION_UPSIDE_STABLE_MS          300U
#define IMU_INTERACTION_UPSIDE_REARM_MS          1000U
#define IMU_INTERACTION_ERROR_LOG_INTERVAL_MS      5000U

#define IMU_INTERACTION_POSE_ENTER_Z_LSB          12000
#define IMU_INTERACTION_POSE_STAY_Z_LSB            8000
#define IMU_INTERACTION_ACCEL_ENTER_MIN_LSB       12000
#define IMU_INTERACTION_ACCEL_ENTER_MAX_LSB       22000
#define IMU_INTERACTION_ACCEL_STAY_MIN_LSB         9000
#define IMU_INTERACTION_ACCEL_STAY_MAX_LSB        28000
#define IMU_INTERACTION_STATIONARY_GYRO_LSB        1500
#define IMU_INTERACTION_REVERSE_HOME_TOLERANCE_US    80U

#define IMU_INTERACTION_QUIET_SAMPLES \
    (IMU_INTERACTION_POST_MOTION_QUIET_MS / IMU_INTERACTION_POLL_MS)
#define IMU_INTERACTION_COOLDOWN_SAMPLES \
    (IMU_INTERACTION_EVENT_COOLDOWN_MS / IMU_INTERACTION_POLL_MS)
#define IMU_INTERACTION_UPSIDE_SAMPLES \
    (IMU_INTERACTION_UPSIDE_STABLE_MS / IMU_INTERACTION_POLL_MS)
#define IMU_INTERACTION_UPSIDE_REARM_SAMPLES \
    (IMU_INTERACTION_UPSIDE_REARM_MS / IMU_INTERACTION_POLL_MS)
_Static_assert((IMU_INTERACTION_POST_MOTION_QUIET_MS % IMU_INTERACTION_POLL_MS) == 0U,
               "post-motion quiet must align to the IMU poll period");
_Static_assert((IMU_INTERACTION_EVENT_COOLDOWN_MS % IMU_INTERACTION_POLL_MS) == 0U,
               "event cooldown must align to the IMU poll period");
_Static_assert((IMU_INTERACTION_UPSIDE_STABLE_MS % IMU_INTERACTION_POLL_MS) == 0U,
               "upside debounce must align to the IMU poll period");
_Static_assert((IMU_INTERACTION_UPSIDE_REARM_MS % IMU_INTERACTION_POLL_MS) == 0U,
               "upside rearm must align to the IMU poll period");
typedef struct {
    service_v2_imu_pose_t pose;
    bool upside_latched;
    bool servo_sequence_valid;
    uint16_t quiet_samples;
    uint16_t cooldown_samples;
    uint16_t upside_samples;
    uint16_t upside_exit_samples;
    uint32_t servo_sequence;
    service_v2_shake_detector_t shake;
} imu_detector_t;

typedef struct {
    service_v2_imu_event_t event;
    TickType_t emitted_tick;
} imu_feedback_request_t;

static lisa_thread_t *s_thread;
static lisa_thread_t *s_feedback_thread;
static lisa_mutex_t *s_lock;
static QueueHandle_t s_feedback_queue;
static service_v2_imu_interaction_status_t s_status;
static imu_detector_t s_detector;
static bool s_error_log_valid;
static TickType_t s_last_error_log_tick;
static bool s_interaction_enabled = true;
static bool s_detector_reset_requested;

/* A completed 180-degree roll deliberately holds this supporting pose.  Its
 * IMU Z sign is positive, but it is not the "four feet in the air" scene. */
static const uint16_t s_reverse_home_us[SERVICE_V2_SERVO_COUNT] = {1100U, 1900U, 1150U, 1900U};

static uint64_t imu_square_i32(int32_t value)
{
    int64_t wide = value;

    return (uint64_t)(wide * wide);
}

static uint32_t imu_abs_i16(int16_t value)
{
    int32_t wide = value;

    return (uint32_t)(wide < 0 ? -wide : wide);
}

static bool imu_error_log_due(void)
{
    TickType_t now = xTaskGetTickCount();

    if (!s_error_log_valid ||
        (TickType_t)(now - s_last_error_log_tick) >= pdMS_TO_TICKS(IMU_INTERACTION_ERROR_LOG_INTERVAL_MS)) {
        s_error_log_valid = true;
        s_last_error_log_tick = now;
        return true;
    }

    return false;
}

static bool imu_orientation_quality(const service_v2_mpu6050_raw_t *sample, bool entering)
{
    uint64_t ax2 = imu_square_i32(sample->accel_x);
    uint64_t ay2 = imu_square_i32(sample->accel_y);
    uint64_t az2 = imu_square_i32(sample->accel_z);
    uint64_t norm2 = ax2 + ay2 + az2;
    uint32_t min_lsb = entering ? IMU_INTERACTION_ACCEL_ENTER_MIN_LSB : IMU_INTERACTION_ACCEL_STAY_MIN_LSB;
    uint32_t max_lsb = entering ? IMU_INTERACTION_ACCEL_ENTER_MAX_LSB : IMU_INTERACTION_ACCEL_STAY_MAX_LSB;

    if (norm2 < imu_square_i32((int32_t)min_lsb) || norm2 > imu_square_i32((int32_t)max_lsb)) {
        return false;
    }

    /* Entry is within about 30 degrees of flat; hysteresis allows about 45 degrees. */
    return entering ? ((ax2 + ay2) * 3U <= az2) : ((ax2 + ay2) <= az2);
}

static bool imu_gyro_stationary(const service_v2_mpu6050_raw_t *sample)
{
    return imu_abs_i16(sample->gyro_x) <= IMU_INTERACTION_STATIONARY_GYRO_LSB &&
           imu_abs_i16(sample->gyro_y) <= IMU_INTERACTION_STATIONARY_GYRO_LSB &&
           imu_abs_i16(sample->gyro_z) <= IMU_INTERACTION_STATIONARY_GYRO_LSB;
}

static service_v2_imu_pose_t imu_classify_pose(const service_v2_mpu6050_raw_t *sample,
                                               service_v2_imu_pose_t previous)
{
    if (previous == SERVICE_V2_IMU_POSE_UPSIDE_DOWN &&
        sample->accel_z >= IMU_INTERACTION_POSE_STAY_Z_LSB && imu_orientation_quality(sample, false)) {
        return SERVICE_V2_IMU_POSE_UPSIDE_DOWN;
    }
    if (previous == SERVICE_V2_IMU_POSE_NORMAL &&
        sample->accel_z <= -IMU_INTERACTION_POSE_STAY_Z_LSB && imu_orientation_quality(sample, false)) {
        return SERVICE_V2_IMU_POSE_NORMAL;
    }
    if (sample->accel_z >= IMU_INTERACTION_POSE_ENTER_Z_LSB && imu_orientation_quality(sample, true)) {
        return SERVICE_V2_IMU_POSE_UPSIDE_DOWN;
    }
    if (sample->accel_z <= -IMU_INTERACTION_POSE_ENTER_Z_LSB && imu_orientation_quality(sample, true)) {
        return SERVICE_V2_IMU_POSE_NORMAL;
    }

    return SERVICE_V2_IMU_POSE_OTHER;
}

static void imu_shake_reset(bool reset_baseline)
{
    service_v2_shake_detector_reset(&s_detector.shake, reset_baseline);
}

static bool imu_pulse_near(uint16_t actual_us, uint16_t expected_us)
{
    uint16_t delta = actual_us > expected_us ? actual_us - expected_us : expected_us - actual_us;

    return delta <= IMU_INTERACTION_REVERSE_HOME_TOLERANCE_US;
}

static bool imu_servo_at_reverse_home(const service_v2_servo_status_t *servo)
{
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if (!imu_pulse_near(servo->current_us[i], s_reverse_home_us[i]) ||
            !imu_pulse_near(servo->target_us[i], s_reverse_home_us[i])) {
            return false;
        }
    }

    return true;
}

static bool imu_executor_inhibited(bool *sequence_changed, bool *reverse_home)
{
    service_v2_gait_status_t gait = {0};
    service_v2_servo_status_t servo = {0};
    int gait_ret = service_v2_gait_get_status(&gait);
    int servo_ret = service_v2_actuators_servo_get_status(&servo);

    *sequence_changed = false;
    *reverse_home = false;
    if (gait_ret != 0 || servo_ret != 0 || !gait.ready) {
        return true;
    }

    *reverse_home = imu_servo_at_reverse_home(&servo);

    if (!s_detector.servo_sequence_valid) {
        s_detector.servo_sequence_valid = true;
        s_detector.servo_sequence = servo.sequence;
    } else if (s_detector.servo_sequence != servo.sequence) {
        s_detector.servo_sequence = servo.sequence;
        *sequence_changed = true;
    }

    /* A non-zero watchdog belongs to Motion Studio/direct host control, even
     * between frames.  Do not let an impact from that path become a scene. */
    return gait.busy || servo.frame_active || servo.sweep_active || servo.watchdog_ms != 0U ||
           servo.watchdog_expired;
}

static void imu_feedback_count_played(void)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.feedback_played_count++;
    lisa_mutex_unlock(s_lock);
}

static void imu_feedback_count_suppressed(void)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.feedback_suppressed_count++;
    lisa_mutex_unlock(s_lock);
}

static void imu_feedback_count_missing(void)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.feedback_missing_count++;
    lisa_mutex_unlock(s_lock);
}

static void imu_feedback_on_voice_thread(void *data, uint32_t len)
{
    if (!__atomic_load_n(&s_interaction_enabled, __ATOMIC_ACQUIRE)) {
        imu_feedback_count_suppressed();
        return;
    }
    if (data == NULL || len != sizeof(imu_feedback_request_t)) {
        imu_feedback_count_suppressed();
        LISA_LOGW(TAG, "invalid voice feedback request");
        return;
    }

    const imu_feedback_request_t *request = data;
    uint16_t tone_id;

    if ((TickType_t)(xTaskGetTickCount() - request->emitted_tick) >
        pdMS_TO_TICKS(IMU_INTERACTION_FEEDBACK_MAX_AGE_MS)) {
        imu_feedback_count_suppressed();
        LISA_LOGI(TAG, "%s feedback expired before voice dispatch",
                  service_v2_imu_interaction_event_name(request->event));
        return;
    }

    switch (request->event) {
    case SERVICE_V2_IMU_EVENT_UPSIDE_DOWN:
        tone_id = TONE_ID_108;
        break;
    case SERVICE_V2_IMU_EVENT_TABLE_SHAKE:
        tone_id = TONE_ID_109;
        break;
    default:
        imu_feedback_count_suppressed();
        return;
    }

    /* This callback runs on the voice ebus thread, which owns the intent
     * stack and the prompt-tone queue.  Never inspect or mutate them from the
     * 20 ms IMU worker. */
    const char *url = app_tone_get_url(tone_id);
    if (url == NULL) {
        imu_feedback_count_missing();
        LISA_LOGW(TAG, "%s feedback unavailable: tone id=%u",
                  service_v2_imu_interaction_event_name(request->event), (unsigned)tone_id);
        return;
    }

    if (voice_player_try_play_prompt_tone_now_url(url)) {
        imu_feedback_count_played();
        LISA_LOGI(TAG, "%s feedback accepted immediately: tone id=%u",
                  service_v2_imu_interaction_event_name(request->event), (unsigned)tone_id);
    } else {
        imu_feedback_count_suppressed();
        LISA_LOGI(TAG, "%s feedback rejected because audio is busy",
                  service_v2_imu_interaction_event_name(request->event));
    }
}

static bool imu_emit_event(service_v2_imu_event_t event, uint32_t shake_peak)
{
    imu_feedback_request_t request = {
        .event = event,
        .emitted_tick = xTaskGetTickCount(),
    };

    if (event == SERVICE_V2_IMU_EVENT_UPSIDE_DOWN) {
        int gait_ret = service_v2_gait_submit_if_idle(SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE, 0U);
        if (gait_ret != 0) {
            /* 这里不锁存本次姿态；等队列空闲后再连续确认 300 ms。 */
            LISA_LOGI(TAG, "upside wiggle not queued, retry after debounce: %d", gait_ret);
            return false;
        }
    }

    /* Upside-down episodes already have a pose latch.  Sharing the shake
     * cooldown made one sensor event silently consume the other. */
    if (event == SERVICE_V2_IMU_EVENT_TABLE_SHAKE) {
        s_detector.cooldown_samples = IMU_INTERACTION_COOLDOWN_SAMPLES;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.last_event = event;
    if (event == SERVICE_V2_IMU_EVENT_UPSIDE_DOWN) {
        s_status.upside_down_count++;
    } else if (event == SERVICE_V2_IMU_EVENT_TABLE_SHAKE) {
        s_status.table_shake_count++;
        s_status.shake_peak_gyro_lsb = shake_peak;
    }
    lisa_mutex_unlock(s_lock);

    if (event == SERVICE_V2_IMU_EVENT_TABLE_SHAKE) {
        LISA_LOGI(TAG, "dizzy sustained shake detected: peak_gyro_lsb=%u rotations=%u angle_deg=%u min_duration_ms=%u",
                  (unsigned)shake_peak, (unsigned)SERVICE_V2_SHAKE_REQUIRED_ROTATIONS,
                  (unsigned)SERVICE_V2_SHAKE_ROTATION_DEGREES,
                  (unsigned)SERVICE_V2_SHAKE_WINDOW_MS);
        service_v2_rgb_effect_notify_dizzy();
    }

    /* Never call voice_invoke() from the deadline-driven 20 ms task: the
     * voice ebus uses a blocking publish.  This zero-wait local queue keeps
     * sensor sampling independent even if the voice queue is congested. */
    if (s_feedback_queue == NULL || xQueueSend(s_feedback_queue, &request, 0U) != pdPASS) {
        imu_feedback_count_suppressed();
        LISA_LOGW(TAG, "%s feedback queue full", service_v2_imu_interaction_event_name(event));
    }

    return true;
}

static void imu_feedback_task(void *arg)
{
    (void)arg;

    for (;;) {
        imu_feedback_request_t request = {0};

        if (xQueueReceive(s_feedback_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* voice_invoke() copies this small stack payload into the voice ebus
         * message before returning, so its lifetime does not escape here. */
        if (voice_invoke(imu_feedback_on_voice_thread, &request, sizeof(request)) != 0) {
            imu_feedback_count_suppressed();
            LISA_LOGW(TAG, "%s feedback invoke failed",
                      service_v2_imu_interaction_event_name(request.event));
        }
    }
}

static bool imu_shake_accept(const service_v2_mpu6050_raw_t *sample, uint32_t *trigger_peak)
{
    uint8_t before = s_detector.shake.rotation_count;
    bool triggered = service_v2_shake_detector_update(&s_detector.shake,
                                                       lisa_os_get_tick_ms(),
                                                       sample->gyro_x,
                                                       sample->gyro_y,
                                                       sample->gyro_z,
                                                       trigger_peak);
    uint8_t after = s_detector.shake.rotation_count;

    if (!triggered && after > before) {
        LISA_LOGI(TAG, "dizzy rotation accepted: %u/%u angle_deg=%u", (unsigned)after,
                  (unsigned)SERVICE_V2_SHAKE_REQUIRED_ROTATIONS,
                  (unsigned)SERVICE_V2_SHAKE_ROTATION_DEGREES);
    }
    return triggered;
}

static void imu_status_update_sample(const service_v2_mpu6050_raw_t *sample, bool inhibited)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.inhibited = inhibited;
    s_status.pose = s_detector.pose;
    s_status.accel_x = sample->accel_x;
    s_status.accel_y = sample->accel_y;
    s_status.accel_z = sample->accel_z;
    s_status.gyro_x = sample->gyro_x;
    s_status.gyro_y = sample->gyro_y;
    s_status.gyro_z = sample->gyro_z;
    s_status.sample_count++;
    s_status.quiet_remaining_ms = (uint32_t)s_detector.quiet_samples * IMU_INTERACTION_POLL_MS;
    s_status.cooldown_remaining_ms = (uint32_t)s_detector.cooldown_samples * IMU_INTERACTION_POLL_MS;
    lisa_mutex_unlock(s_lock);
}

static void imu_status_record_error(bool inhibited)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.inhibited = inhibited;
    s_status.imu_error_count++;
    s_status.quiet_remaining_ms = (uint32_t)s_detector.quiet_samples * IMU_INTERACTION_POLL_MS;
    s_status.cooldown_remaining_ms = (uint32_t)s_detector.cooldown_samples * IMU_INTERACTION_POLL_MS;
    lisa_mutex_unlock(s_lock);
}

static void imu_process_sample(const service_v2_mpu6050_raw_t *sample)
{
    bool sequence_changed = false;
    bool reverse_home = false;
    bool executor_busy = imu_executor_inhibited(&sequence_changed, &reverse_home);

    if (s_detector.cooldown_samples > 0U) {
        s_detector.cooldown_samples--;
    }

    s_detector.pose = imu_classify_pose(sample, s_detector.pose);

    if (executor_busy) {
        s_detector.quiet_samples = IMU_INTERACTION_QUIET_SAMPLES;
        s_detector.upside_samples = 0U;
        s_detector.upside_exit_samples = 0U;
        imu_shake_reset(true);
        imu_status_update_sample(sample, true);
        return;
    }

    if (sequence_changed) {
        /* 动作结束后仍让普通晃动检测静默1.5s，但朝天姿态不需要陪等。 */
        s_detector.quiet_samples = IMU_INTERACTION_QUIET_SAMPLES;
        imu_shake_reset(true);
    }

    bool emitted = false;

    if (s_detector.pose == SERVICE_V2_IMU_POSE_UPSIDE_DOWN) {
        s_detector.upside_exit_samples = 0U;
        if (!s_detector.upside_latched && imu_gyro_stationary(sample)) {
            /* Cooldown suppresses feedback, not episode tracking.  Consume a
             * stable episode at 300 ms so it cannot turn into a stale quip
             * several seconds later when cooldown expires. */
            if (s_detector.upside_samples < IMU_INTERACTION_UPSIDE_SAMPLES) {
                s_detector.upside_samples++;
            }
            if (s_detector.upside_samples >= IMU_INTERACTION_UPSIDE_SAMPLES) {
                bool final_sequence_changed = false;
                bool final_reverse_home = false;
                bool final_busy = imu_executor_inhibited(&final_sequence_changed, &final_reverse_home);

                if (final_busy || final_sequence_changed) {
                    s_detector.quiet_samples = IMU_INTERACTION_QUIET_SAMPLES;
                    s_detector.upside_samples = 0U;
                } else if (reverse_home && final_reverse_home) {
                    /* Consume this upside-sign episode exactly once.  It will
                     * re-arm only after leaving the pose for a full second. */
                    s_detector.upside_latched = true;
                    s_detector.upside_samples = 0U;
                    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
                    s_status.reverse_home_suppressed_count++;
                    lisa_mutex_unlock(s_lock);
                    LISA_LOGI(TAG, "upside-down feedback suppressed at reverse HOME");
                } else {
                    if (imu_emit_event(SERVICE_V2_IMU_EVENT_UPSIDE_DOWN, 0U)) {
                        s_detector.upside_latched = true;
                        emitted = true;
                    } else {
                        s_detector.upside_samples = 0U;
                    }
                }
            }
        } else if (!imu_gyro_stationary(sample)) {
            /* Rotation while still on the positive-Z side breaks the stable
             * debounce, but must never re-arm an already consumed episode. */
            s_detector.upside_samples = 0U;
        }
    } else {
        s_detector.upside_samples = 0U;
        if (s_detector.upside_latched) {
            if (s_detector.upside_exit_samples < IMU_INTERACTION_UPSIDE_REARM_SAMPLES) {
                s_detector.upside_exit_samples++;
            }
            if (s_detector.upside_exit_samples >= IMU_INTERACTION_UPSIDE_REARM_SAMPLES) {
                s_detector.upside_latched = false;
                s_detector.upside_exit_samples = 0U;
            }
        }
    }

    /* quiet_samples 仅用来隔离舵机冲击与普通晃动。朝天判定已在上面完成，
     * 因此翻转到位后不再多等1.5s。 */
    if (s_detector.quiet_samples > 0U) {
        s_detector.quiet_samples--;
        imu_shake_reset(true);
        imu_status_update_sample(sample, true);
        return;
    }

    /* A deliberate half-roll makes positive Z the supported side and moves
     * all four servos to reverse HOME.  Table vibration remains meaningful on
     * either supported face; an arbitrary positive-Z/fail-off pose does not. */
    /* Hand-held shaking naturally crosses every orientation.  Requiring a
     * supported face discarded the pulse window on each half turn. */
    if (emitted) {
        imu_shake_reset(true);
    } else if (s_detector.cooldown_samples == 0U) {
        uint32_t shake_peak = 0U;

        if (imu_shake_accept(sample, &shake_peak)) {
            /* Re-read both executors at the decision boundary. This closes
             * the gap where a touch event queues a gait during the 5 s shake
             * observation window. */
            bool final_sequence_changed = false;
            bool final_reverse_home = false;
            bool final_busy = imu_executor_inhibited(&final_sequence_changed, &final_reverse_home);
            if (!final_busy && !final_sequence_changed) {
                (void)imu_emit_event(SERVICE_V2_IMU_EVENT_TABLE_SHAKE, shake_peak);
            } else {
                s_detector.quiet_samples = IMU_INTERACTION_QUIET_SAMPLES;
            }
            imu_shake_reset(true);
        }
    } else {
        /* Do not retain energy that could become a delayed event when the
         * cooldown expires; establish a fresh baseline afterwards. */
        imu_shake_reset(true);
    }

    imu_status_update_sample(sample, s_detector.quiet_samples > 0U);
}

static void imu_interaction_task(void *arg)
{
    (void)arg;
    TickType_t next_poll = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&next_poll, pdMS_TO_TICKS(IMU_INTERACTION_POLL_MS));

        if (!__atomic_load_n(&s_interaction_enabled, __ATOMIC_ACQUIRE)) {
            continue;
        }
        if (__atomic_exchange_n(&s_detector_reset_requested, false, __ATOMIC_ACQ_REL)) {
            memset(&s_detector, 0, sizeof(s_detector));
            s_detector.pose = SERVICE_V2_IMU_POSE_UNKNOWN;
            s_detector.quiet_samples = IMU_INTERACTION_QUIET_SAMPLES;
            imu_shake_reset(true);
        }

        service_v2_mpu6050_raw_t sample = {0};
        int ret = service_v2_mpu6050_read_raw(&sample);

        if (ret == SERVICE_V2_SENSORS_OK) {
            imu_process_sample(&sample);
        } else {
            s_detector.quiet_samples = IMU_INTERACTION_QUIET_SAMPLES;
            s_detector.upside_samples = 0U;
            s_detector.upside_exit_samples = 0U;
            imu_shake_reset(true);
            imu_status_record_error(true);
            if (imu_error_log_due()) {
                LISA_LOGW(TAG, "MPU6050 read failed: %d", ret);
            }
        }
    }
}

int service_v2_imu_interaction_init(void)
{
    if (s_thread != NULL) {
        return 0;
    }

    service_v2_gait_status_t gait = {0};
    int ret = service_v2_gait_get_status(&gait);
    if (ret != 0 || !gait.ready) {
        LISA_LOGE(TAG, "gait service is not ready: %d", ret);
        return ret != 0 ? ret : -ENODEV;
    }

    service_v2_servo_status_t servo = {0};
    ret = service_v2_actuators_servo_get_status(&servo);
    if (ret != 0) {
        LISA_LOGE(TAG, "actuator status is not ready: %d", ret);
        return ret;
    }

    /* The interaction service polls; explicitly leave PA12 data-ready IRQ
     * disabled so repeated gait-side MPU init calls cannot change semantics. */
    ret = service_v2_mpu6050_init(false);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGE(TAG, "MPU6050 is not ready: %d", ret);
        return ret;
    }

    if (s_lock == NULL) {
        s_lock = lisa_mutex_create();
        if (s_lock == NULL) {
            LISA_LOGE(TAG, "failed to create interaction lock");
            return -ENOMEM;
        }
    }

    if (s_feedback_queue == NULL) {
        s_feedback_queue = xQueueCreate(IMU_INTERACTION_FEEDBACK_QUEUE_DEPTH, sizeof(imu_feedback_request_t));
        if (s_feedback_queue == NULL) {
            LISA_LOGE(TAG, "failed to create feedback queue");
            return -ENOMEM;
        }
    }

    if (s_feedback_thread == NULL) {
        lisa_thread_attr_t feedback_attr = {
            .name = (uint8_t *)"v2_imu_feedback",
            .stack_size = IMU_INTERACTION_FEEDBACK_TASK_STACK,
            .priority = LISA_OS_PRIORITY_LOW,
        };

        s_feedback_thread = lisa_thread_create(&feedback_attr, imu_feedback_task, NULL);
        if (s_feedback_thread == NULL) {
            LISA_LOGE(TAG, "failed to create feedback worker");
            return -ENOMEM;
        }
    }

    memset(&s_detector, 0, sizeof(s_detector));
    memset(&s_status, 0, sizeof(s_status));
    s_detector.pose = SERVICE_V2_IMU_POSE_UNKNOWN;
    s_detector.quiet_samples = IMU_INTERACTION_QUIET_SAMPLES;
    s_detector.servo_sequence_valid = true;
    s_detector.servo_sequence = servo.sequence;
    s_status.pose = SERVICE_V2_IMU_POSE_UNKNOWN;
    s_status.last_event = SERVICE_V2_IMU_EVENT_NONE;
    s_status.quiet_remaining_ms = IMU_INTERACTION_POST_MOTION_QUIET_MS;
    s_error_log_valid = false;

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"v2_imu_interact",
        .stack_size = IMU_INTERACTION_TASK_STACK,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    s_thread = lisa_thread_create(&attr, imu_interaction_task, NULL);
    if (s_thread == NULL) {
        LISA_LOGE(TAG, "failed to create IMU interaction worker");
        return -ENOMEM;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.ready = true;
    lisa_mutex_unlock(s_lock);

    LISA_LOGI(TAG,
              "ready poll=%ums quiet=%ums cooldown=%ums upside=(z>=%d,%ums) "
              "dizzy=(axes=x+y+z angle=%udeg rotations=%u window=%ums deadband=%u gyro_lsb)",
              (unsigned)IMU_INTERACTION_POLL_MS, (unsigned)IMU_INTERACTION_POST_MOTION_QUIET_MS,
              (unsigned)IMU_INTERACTION_EVENT_COOLDOWN_MS, IMU_INTERACTION_POSE_ENTER_Z_LSB,
              (unsigned)IMU_INTERACTION_UPSIDE_STABLE_MS,
              (unsigned)SERVICE_V2_SHAKE_ROTATION_DEGREES,
              (unsigned)SERVICE_V2_SHAKE_REQUIRED_ROTATIONS,
              (unsigned)SERVICE_V2_SHAKE_WINDOW_MS,
              (unsigned)SERVICE_V2_SHAKE_GYRO_DEADBAND_LSB);
    return 0;
}

int service_v2_imu_interaction_set_enabled(int enabled)
{
    if (s_thread == NULL) {
        return -ENODEV;
    }

    if (!enabled) {
        __atomic_store_n(&s_interaction_enabled, false, __ATOMIC_RELEASE);
        if (s_feedback_queue != NULL) {
            xQueueReset(s_feedback_queue);
        }
        if (s_lock != NULL) {
            lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
            s_status.inhibited = true;
            lisa_mutex_unlock(s_lock);
        }
        return 0;
    }

    __atomic_store_n(&s_detector_reset_requested, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_interaction_enabled, true, __ATOMIC_RELEASE);
    return 0;
}

int service_v2_imu_interaction_get_status(service_v2_imu_interaction_status_t *status)
{
    if (status == NULL) {
        return -EINVAL;
    }
    if (s_lock == NULL) {
        memset(status, 0, sizeof(*status));
        return -ENODEV;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    *status = s_status;
    lisa_mutex_unlock(s_lock);
    return status->ready ? 0 : -ENODEV;
}

const char *service_v2_imu_interaction_pose_name(service_v2_imu_pose_t pose)
{
    switch (pose) {
    case SERVICE_V2_IMU_POSE_NORMAL:
        return "normal";
    case SERVICE_V2_IMU_POSE_UPSIDE_DOWN:
        return "upside-down";
    case SERVICE_V2_IMU_POSE_OTHER:
        return "other";
    case SERVICE_V2_IMU_POSE_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *service_v2_imu_interaction_event_name(service_v2_imu_event_t event)
{
    switch (event) {
    case SERVICE_V2_IMU_EVENT_UPSIDE_DOWN:
        return "upside-down";
    case SERVICE_V2_IMU_EVENT_TABLE_SHAKE:
        return "table-shake";
    case SERVICE_V2_IMU_EVENT_NONE:
    default:
        return "none";
    }
}
