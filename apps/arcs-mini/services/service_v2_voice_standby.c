#include "service_v2_voice_standby.h"
#include "service_button.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "alarm_ring.h"
#include "app_netcfg_portal.h"
#include "app_ble_common.h"
#include "lingxin_server.h"
#include "lisa_log.h"
#include "ota_manager.h"
#include "power/power_manager.h"
#include "sys_wifi.h"
#include "voice_msg.h"
#include "voice_player.h"
#include "voice_player_comm.h"
#include "tone.h"
#include "uboot_features_api.h"

#include "service_v2_actuators.h"
#include "service_v2_gait.h"
#include "service_v2_imu_interaction.h"
#include "service_v2_nfc_peer.h"
#include "service_v2_rgb_effect.h"
#include "service_v2_sensors.h"
#include "service_v2_touch_action.h"

#define TAG "v2.voice_standby"

#define STANDBY_TASK_STACK              3072U
#define STANDBY_TASK_PRIORITY   (tskIDLE_PRIORITY + 2)
#define STANDBY_POLL_MS                  100U
#define STANDBY_FOLD_TIMEOUT_MS        15000U
#define STANDBY_SENSOR_SETTLE_MS          60U
#define STANDBY_MPU_WAKE_SETTLE_MS       200U
#define STANDBY_SERVO_RELEASE_ATTEMPTS      3U
#define STANDBY_SERVO_RELEASE_RETRY_MS     20U

#define STANDBY_NOTIFY_ACTIVITY    (1UL << 0)
#define STANDBY_NOTIFY_MANUAL      (1UL << 1)
#define STANDBY_NOTIFY_MUSIC_START (1UL << 2)
#define STANDBY_NOTIFY_MUSIC_STOP  (1UL << 3)
#define STANDBY_NOTIFY_GAIT        (1UL << 4)
#define STANDBY_NOTIFY_WAKE        (1UL << 5)

static TaskHandle_t s_standby_task;
static service_v2_voice_standby_policy_t s_policy;
static volatile service_v2_voice_standby_state_t s_public_state = SERVICE_V2_VOICE_STANDBY_ACTIVE;
static bool s_music_active;
static bool s_entry_waiting_for_reset;
static bool s_entry_gait_owned;
static bool s_nfc_was_enabled;
static TickType_t s_entry_deadline;
/* Written by accepted local inputs, consumed only by the standby worker. */
static uint8_t s_requested_pose; /* 0 none, 1 stand, 2 assembly */
static bool s_manual_requested;
/* Shared deadline base for awake idle and manual standby. */
static uint32_t s_sleep_started_ms;
/* Accepted inputs arbitrate with the deadline before their notification runs. */
static bool s_wake_pending;
static volatile bool s_auto_shutdown_queued;
static void standby_finish_entry(void);

static uint32_t standby_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void standby_publish_state(void)
{
    taskENTER_CRITICAL();
    if (s_policy.state == SERVICE_V2_VOICE_STANDBY_ASLEEP &&
        s_public_state != SERVICE_V2_VOICE_STANDBY_ASLEEP) {
        s_sleep_started_ms = standby_now_ms();
    }
    if (s_policy.state != SERVICE_V2_VOICE_STANDBY_ASLEEP) {
        s_auto_shutdown_queued = false;
    }
    __atomic_store_n(&s_public_state, s_policy.state, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL();
}

const char *service_v2_voice_standby_state_name(service_v2_voice_standby_state_t state)
{
    switch (state) {
    case SERVICE_V2_VOICE_STANDBY_ACTIVE:
        return "active";
    case SERVICE_V2_VOICE_STANDBY_ENTER_PENDING:
        return "enter-pending";
    case SERVICE_V2_VOICE_STANDBY_ENTERING:
        return "entering";
    case SERVICE_V2_VOICE_STANDBY_ASLEEP:
        return "voice-standby";
    case SERVICE_V2_VOICE_STANDBY_RESUMING:
        return "resuming";
    default:
        return "unknown";
    }
}

service_v2_voice_standby_state_t service_v2_voice_standby_get_state(void)
{
    return __atomic_load_n(&s_public_state, __ATOMIC_ACQUIRE);
}

bool service_v2_voice_standby_background_audio_suppressed(void)
{
    return service_v2_voice_standby_policy_suppress_background_audio(
        service_v2_voice_standby_get_state());
}

static bool standby_ota_busy(void)
{
    ota_state_e state = ota_manager_get_state();

    return state == OTA_STATE_CHECKING || state == OTA_STATE_PACKAGE_INFO ||
           state == OTA_STATE_UPDATING;
}

static bool standby_is_eligible(void)
{
    service_v2_gait_status_t gait = {0};
    int gait_ret = service_v2_gait_get_status(&gait);

    return service_v2_voice_standby_policy_resources_idle(
        lingxin_server_chat_is_active(), voice_player_tts_is_active(),
        voice_player_is_audio_active(), app_netcfg_portal_is_active(),
        alarm_ring_is_active(), standby_ota_busy(), s_music_active,
        gait_ret == 0 && gait.ready, gait_ret == 0 && gait.busy);
}

/* Polling covers long OTA/provisioning/audio work even when it has no end
 * event. Autonomous idle gait deliberately does not reset user inactivity. */
static bool standby_has_foreground_work(void)
{
    return lingxin_server_chat_is_active() || voice_player_tts_is_active() ||
        voice_player_is_audio_active() || app_netcfg_portal_is_active() ||
        alarm_ring_is_active() || standby_ota_busy() || s_music_active;
}

static void standby_poll_auto_shutdown(void)
{
    /* USB time never counts toward unattended power-off, on either boot
     * generation. Poll refresh gives unplugged operation a fresh 20 minutes. */
    if (power_is_usb_plugged()) {
        taskENTER_CRITICAL();
        s_sleep_started_ms = standby_now_ms();
        taskEXIT_CRITICAL();
        return;
    }
    if (s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE && standby_has_foreground_work()) {
        taskENTER_CRITICAL();
        s_sleep_started_ms = standby_now_ms();
        taskEXIT_CRITICAL();
        return;
    }
    if ((s_policy.state != SERVICE_V2_VOICE_STANDBY_ASLEEP &&
         s_policy.state != SERVICE_V2_VOICE_STANDBY_ACTIVE) ||
        s_auto_shutdown_queued || !standby_is_eligible()) return;
    uint32_t elapsed;
    bool queued = false;
    taskENTER_CRITICAL();
    elapsed = standby_now_ms() - s_sleep_started_ms;
    if (!s_wake_pending && !s_auto_shutdown_queued &&
        (s_policy.state == SERVICE_V2_VOICE_STANDBY_ASLEEP ||
         s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE) &&
        elapsed >= CONFIG_APP_V2_STANDBY_SHUTDOWN_TIMEOUT_MS) {
        /* This API only sets a flag and notifies the power worker, with no
         * blocking operations. Queue atomically against accepted user wake;
         * playback and power release happen after this critical section. */
        queued = power_request_idle_shutdown();
        s_auto_shutdown_queued = queued;
    }
    taskEXIT_CRITICAL();
    if (queued) {
        LISA_LOGI(TAG, "continuous idle %ums: automatic shutdown queued", (unsigned)elapsed);
    }
}

static void standby_resume_peripherals(bool restore_pose)
{
    int ret;

    ret = service_v2_mpu6050_set_sleep(false);
    if (ret != SERVICE_V2_SENSORS_OK && ret != SERVICE_V2_SENSORS_ERR_NOT_READY) {
        LISA_LOGW(TAG, "MPU6050 wake failed: %d", ret);
    }
    if (restore_pose && ret == SERVICE_V2_SENSORS_OK) {
        /*
         * The first sample immediately after MPU6050 sleep exit can contain a
         * large gyro transient.  STAND intentionally runs the roll safety
         * check, so submitting it too early makes a normal prone device look
         * as if it is tumbling and the gait layer rejects recovery.  Keep the
         * IMU interaction worker paused until the sensor has settled as well.
         */
        vTaskDelay(pdMS_TO_TICKS(STANDBY_MPU_WAKE_SETTLE_MS));
    }
    ret = service_v2_touch_action_set_enabled(1);
    if (ret != 0) {
        LISA_LOGW(TAG, "touch resume failed: %d", ret);
    }
    ret = service_v2_imu_interaction_set_enabled(1);
    if (ret != 0) {
        LISA_LOGW(TAG, "IMU interaction resume failed: %d", ret);
    }
    ret = service_v2_rgb_effect_set_enabled(1);
    if (ret != 0) {
        LISA_LOGW(TAG, "RGB resume failed: %d", ret);
    }
#if CONFIG_APP_V2_NFC_PEER_ENABLE
    if (s_nfc_was_enabled) {
        ret = service_v2_nfc_peer_start();
        if (ret != 0 && ret != -EALREADY) {
            LISA_LOGW(TAG, "NFC resume failed: %d", ret);
        }
    }
#endif
    s_nfc_was_enabled = false;
    s_entry_gait_owned = false;
    /* No automatic pose here: a network/music callback is not permission to
     * move. The accepted wake worker alone drains motion and queues STAND. */
}

static void standby_finish_resume(bool restore_pose)
{
    standby_resume_peripherals(restore_pose);
    int wifi_ret = sys_wifi_resume_radio();
    (void)app_ble_netcfg_set_paused(false);
    service_v2_voice_standby_policy_mark_resumed(&s_policy, standby_now_ms());
    standby_publish_state();
    LISA_LOGI(TAG, "resumed: peripherals restored, Wi-Fi radio ret=%d; wake worker owns connection", wifi_ret);
}

static void standby_abort_entry(void)
{
    service_v2_voice_standby_action_t action;

    s_entry_waiting_for_reset = false;
    s_entry_gait_owned = false;
    action = service_v2_voice_standby_policy_mark_entered(&s_policy);
    if (action == SERVICE_V2_VOICE_STANDBY_ACTION_RESUME) {
        standby_finish_resume(true);
    } else {
        service_v2_voice_standby_policy_mark_entry_failed(&s_policy, standby_now_ms());
        standby_publish_state();
    }
    LISA_LOGI(TAG, "standby entry cancelled by new activity");
}

static void standby_begin_entry(void)
{
    int ret;
#if CONFIG_APP_V2_NFC_PEER_ENABLE
    service_v2_nfc_peer_status_t nfc_status = {0};
#endif

    standby_publish_state();
    LISA_LOGI(TAG, "entry begin: stand, fold rear legs, then fold front legs before releasing servo torque");

    (void)sys_wifi_set_realtime_mode(false);
    /* Close provisioning intake before waiting for its workers. Do not shut
     * radio power under a submitted credential connection. */
    ret = app_ble_netcfg_set_paused(true);
    if (ret == 0) {
        ret = app_netcfg_portal_stop();
    }
    if (ret != 0) {
        (void)app_ble_netcfg_set_paused(false);
        service_v2_voice_standby_policy_mark_entry_failed(&s_policy, standby_now_ms());
        standby_publish_state();
        LISA_LOGW(TAG, "network provisioning busy, standby cancelled: %d", ret);
        return;
    }
    ret = service_v2_touch_action_set_enabled(0);
    if (ret != 0) {
        LISA_LOGW(TAG, "touch suspend failed: %d", ret);
    }
    ret = service_v2_imu_interaction_set_enabled(0);
    if (ret != 0) {
        LISA_LOGW(TAG, "IMU interaction suspend failed: %d", ret);
    }
    vTaskDelay(pdMS_TO_TICKS(STANDBY_SENSOR_SETTLE_MS));
#if CONFIG_APP_V2_NFC_PEER_ENABLE
    ret = service_v2_nfc_peer_get_status(&nfc_status);
    s_nfc_was_enabled = ret == SERVICE_V2_SENSORS_OK && nfc_status.enabled;
    if (s_nfc_was_enabled) {
        ret = service_v2_nfc_peer_stop();
        if (ret != SERVICE_V2_SENSORS_OK) {
            LISA_LOGW(TAG, "NFC suspend failed: %d", ret);
        }
    }
#else
    s_nfc_was_enabled = false;
#endif

    if (s_policy.resume_after_enter) {
        standby_abort_entry();
        return;
    }

    if (!service_v2_gait_has_local_pose()) {
        /* Never move an untouched, freshly booted toy just to put it asleep. */
        standby_finish_entry();
        return;
    }
    s_entry_gait_owned = true;
    ret = service_v2_gait_submit_if_idle(SERVICE_V2_GAIT_ACTION_STANDBY_FOLD, 0U);
    if (ret != 0) {
        s_entry_gait_owned = false;
        LISA_LOGE(TAG, "standby fold submit failed, standby aborted: %d", ret);
        standby_resume_peripherals(false);
        (void)app_ble_netcfg_set_paused(false);
        service_v2_voice_standby_policy_mark_entry_failed(&s_policy, standby_now_ms());
        standby_publish_state();
        return;
    }

    s_entry_waiting_for_reset = true;
    s_entry_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STANDBY_FOLD_TIMEOUT_MS);
}

static int standby_release_servo_torque(void)
{
    int last_error = -EBUSY;

    for (uint32_t attempt = 1U; attempt <= STANDBY_SERVO_RELEASE_ATTEMPTS;
         ++attempt) {
        service_v2_servo_status_t status = {0};
        int disable_ret = service_v2_actuators_servo_disable_all();
        int status_ret = service_v2_actuators_servo_get_status(&status);
        service_v2_voice_standby_servo_release_t decision =
            service_v2_voice_standby_policy_servo_release(
                disable_ret, status_ret, status.enabled_mask, attempt,
                STANDBY_SERVO_RELEASE_ATTEMPTS);

        if (decision == SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_DONE) {
            LISA_LOGI(TAG, "servo PWM released and verified: enabled_mask=0x%02x attempt=%u",
                      status.enabled_mask, (unsigned)attempt);
            return 0;
        }

        last_error = disable_ret != 0 ? disable_ret
                                      : (status_ret != 0 ? status_ret : -EBUSY);
        if (decision == SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_FAILED) {
            LISA_LOGE(TAG,
                      "servo PWM release verification failed: disable=%d status=%d mask=0x%02x attempts=%u",
                      disable_ret, status_ret, status.enabled_mask,
                      (unsigned)attempt);
            return last_error;
        }

        LISA_LOGW(TAG,
                  "servo PWM still active after release: disable=%d status=%d mask=0x%02x attempt=%u/%u",
                  disable_ret, status_ret, status.enabled_mask,
                  (unsigned)attempt,
                  (unsigned)STANDBY_SERVO_RELEASE_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(STANDBY_SERVO_RELEASE_RETRY_MS));
    }

    return last_error;
}

static void standby_finish_entry(void)
{
    int ret;
    service_v2_voice_standby_action_t action;

    s_entry_waiting_for_reset = false;
    ret = standby_release_servo_torque();
    if (ret != 0) {
        LISA_LOGE(TAG, "servo torque release failed, standby aborted: %d", ret);
        standby_resume_peripherals(true);
        (void)app_ble_netcfg_set_paused(false);
        service_v2_voice_standby_policy_mark_entry_failed(&s_policy, standby_now_ms());
        standby_publish_state();
        return;
    }

    ret = service_v2_mpu6050_set_sleep(true);
    if (ret != SERVICE_V2_SENSORS_OK && ret != SERVICE_V2_SENSORS_ERR_NOT_READY) {
        LISA_LOGW(TAG, "MPU6050 sleep failed: %d", ret);
    }
    ret = service_v2_rgb_effect_set_enabled(0);
    if (ret != 0) {
        LISA_LOGW(TAG, "RGB off failed: %d", ret);
    }
    /* Do not manipulate PA references here.  app_player owns the PA lifecycle
     * and has already released it before standby is eligible; borrowing or
     * decrementing that reference here can race the wake prompt and leave a
     * stale reference count. */

    ret = sys_wifi_suspend_radio();
    if (ret != 0) {
        LISA_LOGE(TAG, "Wi-Fi radio did not stop; cancel standby: %d", ret);
        standby_finish_resume(true);
        return;
    }

    action = service_v2_voice_standby_policy_mark_entered(&s_policy);
    standby_publish_state();
    if (action == SERVICE_V2_VOICE_STANDBY_ACTION_RESUME) {
        standby_finish_resume(true);
        return;
    }

    LISA_LOGI(TAG, "entered: Wi-Fi radio=off, servo PWM=off (supply still on); MIC/ADC/ACOMP, button and watchdog remain active");
}

static void standby_poll_entry(void)
{
    service_v2_gait_status_t gait = {0};

    if (!s_entry_waiting_for_reset) {
        return;
    }
    if (s_policy.resume_after_enter) {
        standby_abort_entry();
        return;
    }

    int ret = service_v2_gait_get_status(&gait);
    if (ret == 0 && gait.ready && !gait.busy) {
        if (gait.current == SERVICE_V2_GAIT_ACTION_STANDBY_FOLD &&
            gait.last_error == 0) {
            standby_finish_entry();
        } else {
            LISA_LOGE(TAG,
                      "standby fold did not complete, standby aborted: current=%s error=%d",
                      service_v2_gait_action_name(gait.current), gait.last_error);
            s_entry_waiting_for_reset = false;
            s_entry_gait_owned = false;
            standby_resume_peripherals(true);
            (void)app_ble_netcfg_set_paused(false);
            service_v2_voice_standby_policy_mark_entry_failed(
                &s_policy, standby_now_ms());
            standby_publish_state();
        }
        return;
    }
    if ((int32_t)(xTaskGetTickCount() - s_entry_deadline) >= 0) {
        LISA_LOGE(TAG, "standby fold timed out, standby aborted");
        standby_resume_peripherals(true);
        (void)app_ble_netcfg_set_paused(false);
        service_v2_voice_standby_policy_mark_entry_failed(&s_policy, standby_now_ms());
        standby_publish_state();
        s_entry_waiting_for_reset = false;
    }
}

static void standby_process_notifications(uint32_t notifications)
{
    service_v2_voice_standby_action_t action;

    if ((notifications & STANDBY_NOTIFY_WAKE) != 0U) {
        taskENTER_CRITICAL();
        s_wake_pending = false;
        taskEXIT_CRITICAL();
    }

    if ((notifications & STANDBY_NOTIFY_MUSIC_START) != 0U) {
        s_music_active = true;
        notifications |= STANDBY_NOTIFY_ACTIVITY;
    }
    if ((notifications & STANDBY_NOTIFY_MUSIC_STOP) != 0U) {
        s_music_active = false;
        notifications |= STANDBY_NOTIFY_ACTIVITY;
    }
    if ((notifications & STANDBY_NOTIFY_GAIT) != 0U && !s_entry_gait_owned) {
        notifications |= STANDBY_NOTIFY_ACTIVITY;
    }
    if ((notifications & STANDBY_NOTIFY_WAKE) != 0U ||
        ((notifications & STANDBY_NOTIFY_ACTIVITY) != 0U &&
         s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE)) {
        if ((notifications & STANDBY_NOTIFY_WAKE) != 0U &&
            s_policy.state == SERVICE_V2_VOICE_STANDBY_ENTER_PENDING) {
            /* A user changed their mind before folding began. */
            service_v2_voice_standby_policy_mark_resumed(&s_policy, standby_now_ms());
        }
        taskENTER_CRITICAL();
        s_sleep_started_ms = standby_now_ms();
        taskEXIT_CRITICAL();
        action = service_v2_voice_standby_policy_note_activity(&s_policy, standby_now_ms());
        standby_publish_state();
        if (action == SERVICE_V2_VOICE_STANDBY_ACTION_RESUME) {
            standby_finish_resume(true);
        } else if (s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE &&
                   sys_wifi_radio_suspended()) {
            /* A failed power-on keeps the radio gate closed, but the local
             * button/wake detector remains usable for the next retry. */
            (void)sys_wifi_resume_radio();
        }
    }
    if ((notifications & STANDBY_NOTIFY_MANUAL) != 0U) {
        taskENTER_CRITICAL();
        bool manual = s_manual_requested;
        s_manual_requested = false;
        taskEXIT_CRITICAL();
        if (manual) {
            service_v2_voice_standby_policy_request_manual(&s_policy);
            standby_publish_state();
            LISA_LOGI(TAG, "manual standby requested");
        }
    }
}

static void standby_task(void *arg)
{
    (void)arg;
    service_v2_voice_standby_policy_init(&s_policy, standby_now_ms(),
                                         CONFIG_APP_V2_VOICE_STANDBY_TIMEOUT_MS);
    s_sleep_started_ms = standby_now_ms();
    standby_publish_state();
    LISA_LOGI(TAG, "ready: automatic timeout=%ums", (unsigned)CONFIG_APP_V2_VOICE_STANDBY_TIMEOUT_MS);

    for (;;) {
        uint32_t notifications = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notifications,
                              pdMS_TO_TICKS(STANDBY_POLL_MS));
        standby_process_notifications(notifications);
        standby_poll_entry();

        if (s_policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE) {
            taskENTER_CRITICAL();
            uint8_t pose = s_requested_pose;
            s_requested_pose = 0U;
            taskEXIT_CRITICAL();
            if (pose != 0U) {
                int pose_ret = service_v2_gait_wake_pose(
                    pose == 2U ? SERVICE_V2_GAIT_ACTION_ASSEMBLY : SERVICE_V2_GAIT_ACTION_STAND,
                    3000U);
                if (pose_ret == 0 && pose == 2U) {
                    voice_player_play_prompt_tone_url(app_tone_get_url(TONE_ID_112));
                } else if (pose_ret != 0) {
                    LISA_LOGW(TAG, "accepted local wake pose failed: %d", pose_ret);
                }
            }
        }

        if (!s_entry_waiting_for_reset) {
            bool automatic_entry_allowed = false;
#ifdef CONFIG_APP_V2_VOICE_STANDBY_AUTO_ENABLE
            automatic_entry_allowed = !power_is_usb_plugged();
#endif
            service_v2_voice_standby_action_t action =
                service_v2_voice_standby_policy_poll(&s_policy, standby_now_ms(),
                                                     standby_is_eligible(),
                                                     automatic_entry_allowed);
            if (action == SERVICE_V2_VOICE_STANDBY_ACTION_ENTER) {
                standby_begin_entry();
            }
        }
        standby_poll_auto_shutdown();
    }
}

static void standby_on_voice(void *unused, uint32_t msg_id, void *data,
                             uint32_t len, void *user_data)
{
    if (msg_id == VOICE_MSG_WAKEUP_BUTTON_START &&
        !service_button_wakeup_event_current(data, len)) return;
    uint32_t bits = STANDBY_NOTIFY_ACTIVITY;
    (void)unused;
    (void)data;
    (void)len;
    (void)user_data;

    /* Raw wake events may be rejected by the voice mode/interaction checks.
     * Only wake_pose() after those checks is allowed to wake from sleep. */
    if (msg_id == VOICE_MSG_PLAYER_MUSIC_PLAYING) {
        bits = STANDBY_NOTIFY_MUSIC_START;
    } else if (msg_id == VOICE_MSG_PLAYER_MUSIC_STOPPED) {
        bits = STANDBY_NOTIFY_MUSIC_STOP;
    } else if (msg_id == VOICE_MSG_V2_GAIT_ACTION_STARTED ||
               msg_id == VOICE_MSG_V2_GAIT_ACTION_FINISHED) {
        if (data != NULL && len == sizeof(service_v2_gait_action_finished_t) &&
            ((const service_v2_gait_action_finished_t *)data)->background) return;
        bits = STANDBY_NOTIFY_GAIT;
    }
    if (s_standby_task != NULL) {
        (void)xTaskNotify(s_standby_task, bits, eSetBits);
    }
}

static int standby_subscribe(uint32_t msg_id)
{
    return voice_msg_sub(msg_id, standby_on_voice, NULL) == 0 ? 0 : -EIO;
}

int service_v2_voice_standby_init(void)
{
    if (s_standby_task != NULL) {
        return 0;
    }

    if (standby_subscribe(VOICE_MSG_WAKEUP_KEYWORD) != 0 ||
        standby_subscribe(VOICE_MSG_WAKEUP_BUTTON_START) != 0 ||
        standby_subscribe(VOICE_MSG_CLOUD_SESSION_STARTING) != 0 ||
        standby_subscribe(VOICE_MSG_CLOUD_SESSION_FINISHED) != 0 ||
        standby_subscribe(VOICE_MSG_PLAYER_TTS_PLAYING) != 0 ||
        standby_subscribe(VOICE_MSG_PLAYER_TTS_STOPED) != 0 ||
        standby_subscribe(VOICE_MSG_PLAYER_MUSIC_PLAYING) != 0 ||
        standby_subscribe(VOICE_MSG_PLAYER_MUSIC_STOPPED) != 0 ||
        standby_subscribe(VOICE_MSG_CLOUD_ACTION_LIST) != 0 ||
        standby_subscribe(VOICE_MSG_V2_GAIT_ACTION_STARTED) != 0 ||
        standby_subscribe(VOICE_MSG_V2_GAIT_ACTION_FINISHED) != 0) {
        return -EIO;
    }

    if (xTaskCreate(standby_task, "v2_standby", STANDBY_TASK_STACK, NULL,
                    STANDBY_TASK_PRIORITY, &s_standby_task) != pdPASS) {
        s_standby_task = NULL;
        return -ENOMEM;
    }
    return 0;
}

void service_v2_voice_standby_note_activity(void)
{
    if (s_standby_task != NULL) {
        taskENTER_CRITICAL();
        s_manual_requested = false;
        s_wake_pending = true;
        taskEXIT_CRITICAL();
        (void)xTaskNotify(s_standby_task, STANDBY_NOTIFY_WAKE, eSetBits);
    }
}

void service_v2_voice_standby_note_keyword(void)
{
    taskENTER_CRITICAL();
    if (s_public_state == SERVICE_V2_VOICE_STANDBY_ASLEEP ||
        s_public_state == SERVICE_V2_VOICE_STANDBY_ACTIVE) {
        s_sleep_started_ms = standby_now_ms();
    }
    taskEXIT_CRITICAL();
}

void service_v2_voice_standby_shutdown_cancelled(void)
{
    taskENTER_CRITICAL();
    s_auto_shutdown_queued = false;
    s_sleep_started_ms = standby_now_ms();
    taskEXIT_CRITICAL();
}

int service_v2_voice_standby_wake_pose(bool assembly)
{
    if (s_standby_task == NULL) return -ENODEV;
    taskENTER_CRITICAL();
    s_requested_pose = assembly ? 2U : 1U;
    s_manual_requested = false;
    s_wake_pending = true;
    taskEXIT_CRITICAL();
    return xTaskNotify(s_standby_task, STANDBY_NOTIFY_WAKE, eSetBits) == pdPASS ? 0 : -EIO;
}

int service_v2_voice_standby_resume_wait(uint32_t timeout_ms)
{
    if (s_standby_task == NULL) {
        return -ENODEV;
    }
    /* Accepted local input already requested wake. A delayed network worker
     * must not override a newer manual sleep command. */
    uint32_t started_ms = standby_now_ms();
    do {
        if (service_v2_voice_standby_get_state() == SERVICE_V2_VOICE_STANDBY_ACTIVE &&
            !sys_wifi_radio_suspended()) {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(STANDBY_POLL_MS));
    } while ((uint32_t)(standby_now_ms() - started_ms) < timeout_ms);
    return -ETIMEDOUT;
}

int service_v2_voice_standby_request(void)
{
    if (s_standby_task == NULL) {
        return -ENODEV;
    }
    taskENTER_CRITICAL();
    s_manual_requested = true;
    s_requested_pose = 0U;
    taskEXIT_CRITICAL();
    return xTaskNotify(s_standby_task, STANDBY_NOTIFY_MANUAL, eSetBits) == pdPASS
               ? 0
               : -EIO;
}
