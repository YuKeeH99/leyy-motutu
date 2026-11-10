#include "service_v2_low_battery.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "alarm_ring.h"
#include "app_netcfg_portal.h"
#include "app_tone.h"
#include "battery/battery.h"
#include "battery/battery_ui.h"
#include "lingxin_server.h"
#include "lisa_log.h"
#include "ota_manager.h"
#include "power/power_manager.h"
#include "tone.h"
#include "voice_player.h"
#include "voice_player_comm.h"

#include "service_v2_gait.h"
#include "service_v2_low_battery_policy.h"
#include "service_v2_voice_standby.h"

#define TAG "v2.low_battery"

#define LOW_BATTERY_TASK_STACK    2560U
#define LOW_BATTERY_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define LOW_BATTERY_POLL_MS       1000U

typedef struct {
    bool enabled;
    bool reset_policy;
    uint8_t level;
    service_v2_low_battery_sim_state_t state;
} low_battery_simulation_t;

static TaskHandle_t s_task;
static service_v2_low_battery_policy_t s_policy;
static service_v2_low_battery_status_t s_status;
static low_battery_simulation_t s_simulation;
static volatile bool s_shutdown_prompt_requested;

static uint32_t low_battery_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool low_battery_ota_busy(void)
{
    ota_state_e state = ota_manager_get_state();

    return state == OTA_STATE_CHECKING || state == OTA_STATE_PACKAGE_INFO ||
           state == OTA_STATE_UPDATING;
}

static bool low_battery_runtime_idle(void)
{
    service_v2_gait_status_t gait = {0};
    int gait_ret = service_v2_gait_get_status(&gait);

    return service_v2_voice_standby_get_state() == SERVICE_V2_VOICE_STANDBY_ACTIVE &&
           !lingxin_server_chat_is_active() && !voice_player_tts_is_active() &&
           !voice_player_is_audio_active() && !alarm_ring_is_active() &&
           !app_netcfg_portal_is_active() && !low_battery_ota_busy() &&
           gait_ret == 0 && gait.ready && !gait.busy;
}

static void low_battery_status_publish(uint8_t level, bool external_power,
                                       bool reminder_allowed, bool simulated)
{
    taskENTER_CRITICAL();
    s_status.ready = true;
    s_status.simulated = simulated;
    s_status.external_power = external_power;
    s_status.reminder_allowed = reminder_allowed;
    s_status.warning_active = s_policy.warning_active;
    s_status.critical_confirming = s_policy.critical_confirming;
    s_status.shutdown_requested = s_policy.shutdown_requested;
    s_status.level = level;
    taskEXIT_CRITICAL();
}

static bool low_battery_read_sample(uint8_t *level, bool *has_battery,
                                    bool *external_power,
                                    bool *reminder_allowed,
                                    bool *simulated,
                                    bool *simulated_sleep)
{
    low_battery_simulation_t simulation;

    taskENTER_CRITICAL();
    simulation = s_simulation;
    if (s_simulation.reset_policy) {
        s_simulation.reset_policy = false;
    }
    taskEXIT_CRITICAL();

    if (simulation.reset_policy) {
        service_v2_low_battery_policy_init(&s_policy);
    }

    if (simulation.enabled) {
        *level = simulation.level;
        *has_battery = true;
        *external_power = false;
        *reminder_allowed = simulation.state != SERVICE_V2_LOW_BATTERY_SIM_SLEEP;
        *simulated = true;
        *simulated_sleep = simulation.state == SERVICE_V2_LOW_BATTERY_SIM_SLEEP;
        return true;
    }

    voice_msg_battery_info_t battery = {0};
    if (!battery_ui_get_info(&battery)) {
        return false;
    }

    *level = battery.level;
    *has_battery = battery.status != VOICE_MSG_BATTERY_STATUS_NO_BATTERY &&
                   battery.status != VOICE_MSG_BATTERY_STATUS_UNKNOWN;
    *external_power = battery.status == VOICE_MSG_BATTERY_STATUS_CHARGING ||
                      battery.status == VOICE_MSG_BATTERY_STATUS_CHARGE_DONE;
    *reminder_allowed = low_battery_runtime_idle();
    *simulated = false;
    *simulated_sleep = false;
    return true;
}

static void low_battery_play_reminder(uint32_t now_ms, uint8_t level)
{
    const char *url = app_tone_get_url(TONE_ID_115);

    if (url == NULL) {
        LISA_LOGE(TAG, "low-battery reminder tone missing: id=%u",
                  (unsigned)TONE_ID_115);
        return;
    }
    if (!voice_player_try_play_prompt_tone_now_url(url)) {
        LISA_LOGI(TAG, "low-battery reminder deferred: audio became busy");
        return;
    }

    service_v2_low_battery_policy_mark_reminded(&s_policy, now_ms);
    taskENTER_CRITICAL();
    s_status.reminder_count++;
    taskEXIT_CRITICAL();
    LISA_LOGI(TAG, "low-battery reminder accepted: level=%u%%", (unsigned)level);
}

static void low_battery_request_shutdown(uint8_t level, bool simulated_sleep)
{
    service_v2_voice_standby_state_t standby_state =
        service_v2_voice_standby_get_state();
    bool sleeping = simulated_sleep ||
                    standby_state == SERVICE_V2_VOICE_STANDBY_ENTERING ||
                    standby_state == SERVICE_V2_VOICE_STANDBY_ASLEEP ||
                    standby_state == SERVICE_V2_VOICE_STANDBY_RESUMING;

    if (!sleeping) {
        int gait_ret = service_v2_gait_submit(SERVICE_V2_GAIT_ACTION_ALL_OFF, 0U);
        if (gait_ret != 0) {
            LISA_LOGW(TAG, "critical all-off submit failed: %d", gait_ret);
        }
    }

    service_v2_low_battery_policy_mark_shutdown_requested(&s_policy);
    __atomic_store_n(&s_shutdown_prompt_requested, true, __ATOMIC_RELEASE);
    int prompt_ret = voice_player_play_shutdown_prompt_sync();
    if (prompt_ret != 0) {
        LISA_LOGW(TAG, "critical shutdown prompt unavailable: %d", prompt_ret);
    }
    voice_player_abort_shutdown_prompt();

    /* The shutdown prompt is intentionally synchronous so a charger inserted
     * while it is playing can still save the current session.  Check both the
     * raw pin and the debounced battery state immediately before releasing the
     * power latch. */
    if (power_is_usb_plugged() || battery_usb_plugged_stable_get()) {
        taskENTER_CRITICAL();
        if (s_simulation.enabled) {
            memset(&s_simulation, 0, sizeof(s_simulation));
        }
        taskEXIT_CRITICAL();
        service_v2_low_battery_policy_init(&s_policy);
        LISA_LOGI(TAG,
                  "critical shutdown cancelled: external power detected after prompt");
        return;
    }

    taskENTER_CRITICAL();
    s_status.shutdown_count++;
    taskEXIT_CRITICAL();
    LISA_LOGW(TAG, "critical battery shutdown now: level=%u%% standby=%s",
              (unsigned)level, sleeping ? "yes" : "no");
    power_shutdown();

    /* A correctly wired battery latch never returns.  If it does, re-arm the
     * policy so the hard power-off can be retried instead of silently sticking
     * in a shutdown-requested state. */
    service_v2_low_battery_policy_init(&s_policy);
    LISA_LOGE(TAG, "critical power_shutdown returned unexpectedly; retry armed");
}

static void low_battery_task(void *arg)
{
    (void)arg;
    service_v2_low_battery_policy_init(&s_policy);

    for (;;) {
        uint8_t level = 100U;
        bool has_battery = false;
        bool external_power = false;
        bool reminder_allowed = false;
        bool simulated = false;
        bool simulated_sleep = false;
        uint32_t now_ms = low_battery_now_ms();

        if (low_battery_read_sample(&level, &has_battery, &external_power,
                                    &reminder_allowed, &simulated,
                                    &simulated_sleep)) {
            service_v2_low_battery_action_t action =
                service_v2_low_battery_policy_poll(
                    &s_policy, now_ms, level, has_battery, external_power,
                    reminder_allowed);

            if (action == SERVICE_V2_LOW_BATTERY_ACTION_REMIND) {
                low_battery_play_reminder(now_ms, level);
            } else if (action == SERVICE_V2_LOW_BATTERY_ACTION_SHUTDOWN) {
                low_battery_request_shutdown(level, simulated_sleep);
            }
            low_battery_status_publish(level, external_power,
                                       reminder_allowed, simulated);
        }

        vTaskDelay(pdMS_TO_TICKS(LOW_BATTERY_POLL_MS));
    }
}

int service_v2_low_battery_init(void)
{
    if (s_task != NULL) {
        return 0;
    }

    memset(&s_status, 0, sizeof(s_status));
    memset(&s_simulation, 0, sizeof(s_simulation));
    __atomic_store_n(&s_shutdown_prompt_requested, false, __ATOMIC_RELEASE);
    if (xTaskCreate(low_battery_task, "v2_low_bat", LOW_BATTERY_TASK_STACK,
                    NULL, LOW_BATTERY_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return -ENOMEM;
    }

    LISA_LOGI(TAG,
              "ready warning=<%u%% confirm=%ums repeat=%ums critical=<%u%%",
              (unsigned)SERVICE_V2_LOW_BATTERY_WARNING_PERCENT,
              (unsigned)SERVICE_V2_LOW_BATTERY_CONFIRM_MS,
              (unsigned)SERVICE_V2_LOW_BATTERY_REMINDER_INTERVAL_MS,
              (unsigned)SERVICE_V2_LOW_BATTERY_CRITICAL_PERCENT);
    return 0;
}

int service_v2_low_battery_get_status(service_v2_low_battery_status_t *status)
{
    if (status == NULL) {
        return -EINVAL;
    }

    taskENTER_CRITICAL();
    *status = s_status;
    taskEXIT_CRITICAL();
    return s_task != NULL ? 0 : -ENODEV;
}

int service_v2_low_battery_simulate(uint8_t level,
                                    service_v2_low_battery_sim_state_t state)
{
    if (level > 100U || state > SERVICE_V2_LOW_BATTERY_SIM_SLEEP) {
        return -EINVAL;
    }

    taskENTER_CRITICAL();
    s_simulation.enabled = true;
    s_simulation.reset_policy = true;
    s_simulation.level = level;
    s_simulation.state = state;
    taskEXIT_CRITICAL();
    return 0;
}

void service_v2_low_battery_clear_simulation(void)
{
    taskENTER_CRITICAL();
    memset(&s_simulation, 0, sizeof(s_simulation));
    s_simulation.reset_policy = true;
    taskEXIT_CRITICAL();
}

bool service_v2_low_battery_consume_shutdown_prompt_request(void)
{
    return __atomic_exchange_n(&s_shutdown_prompt_requested, false,
                               __ATOMIC_ACQ_REL);
}
