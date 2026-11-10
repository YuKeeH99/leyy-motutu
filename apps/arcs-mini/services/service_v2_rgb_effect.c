#include "service_v2_rgb_effect.h"
#ifdef CONFIG_APP_CODEX_STATUS
#include "lingxin_server.h"
#include "service_codex_status.h"
#endif
#include "service_button.h"
#include "service_offline_action.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "lisa_log.h"
#include "service_v2_actuators.h"
#include "service_v2_rgb_effect_policy.h"
#include "voice_msg.h"
#include "voice_msg_structure.h"
#include "battery/battery_ui.h"
#include "sys_network_manager.h"
#include "service_volume.h"
#ifdef CONFIG_OTA
#include "ota_manager.h"
#endif

#define TAG "v2.rgb-effect"

#define RGB_EFFECT_PERIOD_MS SERVICE_V2_RGB_FRAME_PERIOD_MS
#define RGB_EFFECT_TASK_STACK 2048U
#define RGB_EFFECT_TASK_PRIORITY 2U
#define RGB_EFFECT_QUEUE_DEPTH 16U

typedef enum {
    RGB_COMMAND_DIALOGUE = 0,
    RGB_COMMAND_BATTERY,
    RGB_COMMAND_EMOTION,
    RGB_COMMAND_PROVISIONING,
    RGB_COMMAND_DIZZY,
} rgb_command_type_t;

typedef struct {
    rgb_command_type_t type;
    service_v2_rgb_effect_event_t event;
    voice_msg_battery_info_t battery;
    service_v2_rgb_provisioning_state_t provisioning_state;
    char emotion[16];
} rgb_command_t;

static QueueHandle_t s_event_queue;
static bool s_ready;
static bool s_output_enabled = true;
static bool s_first_output_logged;
static uint32_t s_output_failure_count;
static uint32_t s_queue_drop_count;
static service_v2_rgb_show_t s_show = {.enabled = true};

void service_v2_rgb_effect_show_start(service_v2_rgb_show_kind_t kind)
{
    taskENTER_CRITICAL();
    service_v2_rgb_show_start(&s_show, kind, (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
    taskEXIT_CRITICAL();
}

void service_v2_rgb_effect_show_stop(service_v2_rgb_show_kind_t kind)
{
    taskENTER_CRITICAL();
    service_v2_rgb_show_stop(&s_show, kind);
    taskEXIT_CRITICAL();
}

static void rgb_effect_cancel_show(void)
{
    taskENTER_CRITICAL();
    service_v2_rgb_show_cancel(&s_show);
    taskEXIT_CRITICAL();
}

static void rgb_effect_post(const rgb_command_t *command)
{
    if (s_event_queue == NULL || command == NULL) {
        s_queue_drop_count++;
        return;
    }
    if (xQueueSend(s_event_queue, command, 0U) != pdTRUE) {
        rgb_command_t discarded;
        /* Latest state is more valuable than a stale intermediate event.
         * Make room without ever blocking the voice/battery publisher. */
        (void)xQueueReceive(s_event_queue, &discarded, 0U);
        if (xQueueSend(s_event_queue, command, 0U) != pdTRUE) s_queue_drop_count++;
    }
    if (s_queue_drop_count != 0U &&
        (s_queue_drop_count == 1U || (s_queue_drop_count % 20U) == 0U)) {
        LISA_LOGW(TAG, "state queue unavailable, drops=%u", (unsigned)s_queue_drop_count);
    }
}

static const char *rgb_effect_state_name(service_v2_rgb_effect_state_t state)
{
    switch (state) {
    case SERVICE_V2_RGB_EFFECT_LISTENING:
        return "listening";
    case SERVICE_V2_RGB_EFFECT_SPEAKING:
        return "speaking";
    case SERVICE_V2_RGB_EFFECT_IDLE:
    default:
        return "idle";
    }
}

static bool rgb_effect_event_from_voice(uint32_t msg_id, service_v2_rgb_effect_event_t *event)
{
    switch (msg_id) {
    case VOICE_MSG_WAKEUP_KEYWORD:
    case VOICE_MSG_OFFLINE_ACTION_PROMPT:
    case VOICE_MSG_WAKEUP_BUTTON_START:
        *event = SERVICE_V2_RGB_EVENT_WAKEUP;
        return true;
    case VOICE_MSG_CLOUD_SESSION_STARTING:
        *event = SERVICE_V2_RGB_EVENT_SESSION_STARTING;
        return true;
    case VOICE_MSG_PLAYER_TTS_PLAYING:
        *event = SERVICE_V2_RGB_EVENT_TTS_PLAYING;
        return true;
    case VOICE_MSG_PLAYER_TTS_STOPED:
        *event = SERVICE_V2_RGB_EVENT_TTS_STOPPED;
        return true;
    case VOICE_MSG_CLOUD_SESSION_FINISHED:
    case VOICE_MSG_OFFLINE_ACTION_FINISHED:
        *event = SERVICE_V2_RGB_EVENT_SESSION_FINISHED;
        return true;
    case VOICE_MSG_CLOUD_DISCONNECTED:
        *event = SERVICE_V2_RGB_EVENT_DISCONNECTED;
        return true;
    default:
        return false;
    }
}

static void rgb_effect_on_voice(void *unused, uint32_t msg_id, void *data,
                               uint32_t len, void *user_data)
{
    if (msg_id == VOICE_MSG_WAKEUP_BUTTON_START &&
        !service_button_wakeup_event_current(data, len)) return;
    if (service_offline_action_active() &&
        (msg_id == VOICE_MSG_CLOUD_SESSION_STARTING || msg_id == VOICE_MSG_CLOUD_SESSION_FINISHED ||
         msg_id == VOICE_MSG_CLOUD_DISCONNECTED || msg_id == VOICE_MSG_PLAYER_TTS_PLAYING ||
         msg_id == VOICE_MSG_PLAYER_TTS_STOPED)) return;
    rgb_command_t command = {0};
    (void)unused;
    (void)user_data;

    if (rgb_effect_event_from_voice(msg_id, &command.event)) {
        /* These events supersede a show. Terminal TTS events may be caused by
         * dance music taking focus, so they only update the underlying eyes. */
        if (command.event == SERVICE_V2_RGB_EVENT_WAKEUP ||
            command.event == SERVICE_V2_RGB_EVENT_SESSION_STARTING ||
            command.event == SERVICE_V2_RGB_EVENT_TTS_PLAYING) rgb_effect_cancel_show();
        command.type = RGB_COMMAND_DIALOGUE;
        rgb_effect_post(&command);
    } else if (msg_id == VOICE_MSG_POWER_BATTERY_UPDATE && data != NULL &&
               len == sizeof(voice_msg_battery_info_t)) {
        command.type = RGB_COMMAND_BATTERY;
        command.battery = *(const voice_msg_battery_info_t *)data;
        rgb_effect_post(&command);
    } else if ((msg_id == VOICE_MSG_CLOUD_EMOJI || msg_id == VOICE_MSG_CLOUD_MCP_EMOJI) &&
               data != NULL && len > 1U) {
        command.type = RGB_COMMAND_EMOTION;
        size_t copy = len - 1U;
        if (copy >= sizeof(command.emotion)) copy = sizeof(command.emotion) - 1U;
        memcpy(command.emotion, data, copy);
        command.emotion[copy] = '\0';
        rgb_effect_post(&command);
    }
}

static void rgb_effect_apply(const service_v2_rgb_effect_policy_t *policy, uint32_t phase)
{
    static service_v2_rgb_color_t displayed;
    service_v2_rgb_status_t status;
    service_v2_rgb_color_t color;

    int ret = service_v2_actuators_rgb_get_status(&status);
    if (ret != 0) {
        s_output_failure_count++;
        if (s_output_failure_count == 1U || (s_output_failure_count % 50U) == 0U) {
            LISA_LOGE(TAG, "PA08 RGB status failed: ret=%d count=%u",
                      ret, (unsigned)s_output_failure_count);
        }
        return;
    }
    if (!__atomic_load_n(&s_output_enabled, __ATOMIC_ACQUIRE) ||
        status.owner != SERVICE_V2_RGB_OWNER_SYSTEM) {
        return;
    }

    service_v2_rgb_color_t alert_pixels[5];
    if (service_v2_rgb_effect_policy_alert_frame(policy, alert_pixels)) {
        /* Alert edges and the three non-eye LEDs must never inherit a show
         * or the ordinary colour crossfade. */
        displayed = alert_pixels[1];
        ret = service_v2_actuators_rgb_system_frame(alert_pixels);
    } else {
        service_v2_rgb_effect_policy_color(
            policy, service_v2_rgb_effect_breath_percent(phase), &color);
        /* Both eyes share a colour. Crossfade changes rather than jumping hue.
         * Standby still bypasses this path and sends an immediate black frame. */
#define APPROACH(channel) do { \
        int delta = (int)color.channel - (int)displayed.channel; \
        if (delta > 8) delta = 8; \
        if (delta < -8) delta = -8; \
        displayed.channel = (uint8_t)((int)displayed.channel + delta); \
    } while (0)
        APPROACH(red);
        APPROACH(green);
        APPROACH(blue);
#undef APPROACH
        color = displayed;
        taskENTER_CRITICAL();
        /* Capture the clock with the show: a newer start must not look expired. */
        uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        service_v2_rgb_show_kind_t show = service_v2_rgb_show_current(&s_show, now_ms);
        uint32_t show_ms = now_ms - s_show.started_ms;
        taskEXIT_CRITICAL();
        if (show != SERVICE_V2_RGB_SHOW_NONE) {
            service_v2_rgb_color_t pixels[5];
            service_v2_rgb_show_frame(show, show_ms, pixels);
            ret = service_v2_actuators_rgb_system_frame(pixels);
        } else {
#ifdef CONFIG_APP_CODEX_STATUS
            /* A late TTS stop event can leave the visual policy listening
             * after the Lingxin session has closed. Use live ownership. */
            bool codex_can_show = !lingxin_server_chat_is_active() &&
                                  !service_offline_action_active();
            codex_can_show |= service_codex_status_feedback_active();
#ifdef CONFIG_APP_CODEX_STATUS_AGENT_VOICE
            codex_can_show |= service_codex_status_notice_active();
#endif
            if (codex_can_show && !policy->dizzy_active) {
                service_codex_status_apply_color(&color);
            }
#endif
            ret = service_v2_actuators_rgb_set(color.red, color.green, color.blue);
        }
    }
    /* Closing the standby gate can race a worker that passed the first check.
     * Re-check after the write so the final latched WS2812 frame is always off. */
    if (ret == 0 && !__atomic_load_n(&s_output_enabled, __ATOMIC_ACQUIRE)) {
        ret = service_v2_actuators_rgb_off();
    }
    if (ret != 0) {
        s_output_failure_count++;
        if (s_output_failure_count == 1U || (s_output_failure_count % 50U) == 0U) {
            LISA_LOGE(TAG, "PA08 RGB write failed: ret=%d count=%u",
                      ret, (unsigned)s_output_failure_count);
        }
    } else if (!s_first_output_logged) {
        s_first_output_logged = true;
        LISA_LOGI(TAG, "PA08 RGB output accepted: count=%u rgb=%u,%u,%u brightness=%u%%",
                  (unsigned)status.count,
                  (unsigned)displayed.red,
                  (unsigned)displayed.green,
                  (unsigned)displayed.blue,
                  (unsigned)status.brightness_percent);
    }
}

static void rgb_effect_update_ota(service_v2_rgb_effect_policy_t *policy)
{
#ifdef CONFIG_OTA
    ota_state_t ota = {0};
    if (ota_manager_get_state_snapshot(&ota) != 0) return;
    service_v2_rgb_ota_state_t state = SERVICE_V2_RGB_OTA_NONE;
    if (ota.state == OTA_STATE_UPDATING ||
        (ota.state == OTA_STATE_SUCCESSED && ota.target == OTA_TARGET_APP)) {
        /* APP success means staged for boot, not installed. Keep the update
         * indication until reboot; never announce installation prematurely. */
        state = SERVICE_V2_RGB_OTA_UPDATING;
    } else if (ota.state == OTA_STATE_SUCCESSED) {
        state = SERVICE_V2_RGB_OTA_SUCCESS;
    } else if (ota.state == OTA_STATE_FAILED) {
        state = SERVICE_V2_RGB_OTA_FAILED;
    }
    service_v2_rgb_effect_policy_ota(policy, state);
#else
    (void)policy;
#endif
}

static void rgb_effect_task(void *arg)
{
    service_v2_rgb_effect_policy_t policy;
    rgb_command_t command;
    uint32_t effect_ms = 0U;
    uint32_t pending_ms = RGB_EFFECT_PERIOD_MS;
    TickType_t last_tick = xTaskGetTickCount();
    (void)arg;

    /* Normal eyes run continuously beneath the explicit music/motion show. */

    service_v2_rgb_effect_policy_init(&policy,
                                      (uint32_t)xTaskGetTickCount() ^ 0xa5082026U);
    voice_msg_battery_info_t initial_battery = {0};
    if (battery_ui_get_info(&initial_battery)) {
        service_v2_rgb_power_state_t power = SERVICE_V2_RGB_POWER_DISCHARGING;
        if (initial_battery.status == VOICE_MSG_BATTERY_STATUS_CHARGING) {
            power = SERVICE_V2_RGB_POWER_CHARGING;
        } else if (initial_battery.status == VOICE_MSG_BATTERY_STATUS_CHARGE_DONE) {
            power = SERVICE_V2_RGB_POWER_CHARGE_DONE;
        }
        service_v2_rgb_effect_policy_battery(&policy, initial_battery.level, power);
    }
    LISA_LOGI(TAG, "state=%s", rgb_effect_state_name(policy.state));

    for (;;) {
        if (xQueueReceive(s_event_queue, &command,
                          pdMS_TO_TICKS(RGB_EFFECT_PERIOD_MS)) == pdTRUE) {
            service_v2_rgb_effect_state_t old_state = policy.state;
            if (command.type == RGB_COMMAND_DIALOGUE) {
                service_v2_rgb_effect_policy_event(&policy, command.event);
            } else if (command.type == RGB_COMMAND_BATTERY) {
                service_v2_rgb_power_state_t power = SERVICE_V2_RGB_POWER_DISCHARGING;
                if (command.battery.status == VOICE_MSG_BATTERY_STATUS_CHARGING) {
                    power = SERVICE_V2_RGB_POWER_CHARGING;
                } else if (command.battery.status == VOICE_MSG_BATTERY_STATUS_CHARGE_DONE) {
                    power = SERVICE_V2_RGB_POWER_CHARGE_DONE;
                }
                service_v2_rgb_effect_policy_battery(&policy, command.battery.level, power);
            } else if (command.type == RGB_COMMAND_EMOTION) {
                if (!service_v2_rgb_effect_policy_emotion(&policy, command.emotion)) {
                    LISA_LOGW(TAG, "unknown emotion=%s", command.emotion);
                } else {
                    LISA_LOGI(TAG, "emotion=%s", command.emotion);
                }
            } else if (command.type == RGB_COMMAND_PROVISIONING) {
                service_v2_rgb_effect_policy_provisioning(
                    &policy, command.provisioning_state);
                LISA_LOGI(TAG, "provisioning=%u",
                          (unsigned)command.provisioning_state);
            } else if (command.type == RGB_COMMAND_DIZZY) {
                service_v2_rgb_effect_policy_event(&policy, SERVICE_V2_RGB_EVENT_DIZZY);
                LISA_LOGI(TAG, "dizzy effect started");
            }
            if (old_state != policy.state) {
                effect_ms = 0U;
                LISA_LOGI(TAG, "state=%s", rgb_effect_state_name(policy.state));
            }
        }
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = (uint32_t)((now - last_tick) * portTICK_PERIOD_MS);
        last_tick = now;
        effect_ms = (effect_ms + elapsed_ms) % 3200U;
        if (service_v2_rgb_effect_frame_due(elapsed_ms, &pending_ms)) {
            /* Polling cannot lose an upgrade state to a full voice queue. */
            rgb_effect_update_ota(&policy);
            service_v2_rgb_effect_policy_mute(&policy, service_volume_is_muted());
            /* Explicit offline survives an idle window and successful Wi-Fi;
             * it must not be inferred from the current link/session state. */
            service_v2_rgb_effect_policy_offline(&policy, service_offline_action_forced_offline());
            /* Sample cached bearer/IP/reachability state, not cloud session
             * events. Polling also heals dropped event-queue notifications. */
            sys_network_status_t network = {0};
            if (sys_network_get_status(&network) == 0) {
                bool link = network.active_bearer == SYS_NETWORK_BEARER_WIFI
                                ? network.wifi_connected
                                : network.active_bearer == SYS_NETWORK_BEARER_MODEM && network.modem_connected;
                service_v2_rgb_effect_policy_network(&policy,
                    link && network.connected && !network.switching);
            }
            rgb_effect_apply(&policy, effect_ms / RGB_EFFECT_PERIOD_MS);
        }
        service_v2_rgb_effect_policy_tick(&policy, elapsed_ms);
    }
}

static int rgb_effect_subscribe(uint32_t msg_id)
{
    return voice_msg_sub(msg_id, rgb_effect_on_voice, NULL) == 0 ? 0 : -EIO;
}

int service_v2_rgb_effect_init(void)
{
    if (s_ready) {
        return 0;
    }
    if (!service_v2_actuators_is_ready()) {
        return -ENODEV;
    }

    s_event_queue = xQueueCreate(RGB_EFFECT_QUEUE_DEPTH, sizeof(rgb_command_t));
    if (s_event_queue == NULL) {
        return -ENOMEM;
    }

    if (rgb_effect_subscribe(VOICE_MSG_WAKEUP_KEYWORD) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_OFFLINE_ACTION_PROMPT) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_OFFLINE_ACTION_FINISHED) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_WAKEUP_BUTTON_START) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_CLOUD_SESSION_STARTING) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_PLAYER_TTS_PLAYING) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_PLAYER_TTS_STOPED) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_CLOUD_SESSION_FINISHED) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_CLOUD_DISCONNECTED) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_POWER_BATTERY_UPDATE) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_CLOUD_EMOJI) != 0 ||
        rgb_effect_subscribe(VOICE_MSG_CLOUD_MCP_EMOJI) != 0) {
        LISA_LOGE(TAG, "subscribe dialogue state failed");
        return -EIO;
    }

    if (xTaskCreate(rgb_effect_task, "v2_rgb_fx", RGB_EFFECT_TASK_STACK,
                    NULL, RGB_EFFECT_TASK_PRIORITY, NULL) != pdPASS) {
        LISA_LOGE(TAG, "create effect task failed");
        return -ENOMEM;
    }

    s_ready = true;
    LISA_LOGI(TAG, "dialogue RGB effect ready");
    return 0;
}

int service_v2_rgb_effect_set_enabled(int enabled)
{
    if (!s_ready) {
        return -ENODEV;
    }

    taskENTER_CRITICAL();
    service_v2_rgb_show_enable(&s_show, enabled != 0);
    __atomic_store_n(&s_output_enabled, enabled != 0, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL();
    if (!enabled) {
        return service_v2_actuators_rgb_off();
    }
    return 0;
}

void service_v2_rgb_effect_set_provisioning(
    service_v2_rgb_provisioning_state_t state)
{
    if (state != SERVICE_V2_RGB_PROVISIONING_INACTIVE) rgb_effect_cancel_show();
    rgb_command_t command = {
        .type = RGB_COMMAND_PROVISIONING,
        .provisioning_state = state,
    };
    rgb_effect_post(&command);
}

void service_v2_rgb_effect_notify_dizzy(void)
{
    rgb_effect_cancel_show();
    rgb_command_t command = {.type = RGB_COMMAND_DIZZY};
    rgb_effect_post(&command);
}
