#include "stdint.h"
#include "stdbool.h"

#include "FreeRTOS.h"
#include "task.h"

#define TAG "service_button"

#include "lisa_log.h"
#include "lisa_btn.h"
#include "lisa_gpio.h"

#include "Driver_GPADC.h"
#include "IOMuxManager.h"
#include "board.h"
#ifdef CONFIG_OTA
#include "ota_manager.h"
#endif

#include "voice_msg.h"
#include "service_button.h"
#include "service_sd_music.h"
#include "power_manager.h"
#ifdef CONFIG_VOICE_LINK_LINGXIN
#include "lingxin_server.h"
#include "app_datas.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
#endif
#endif

/* ---- 配置 --------------------------------------------------------------- */

#define MINI_POWER_KEY_SHORT_PRESS_MS 800U
#define MINI_POWER_KEY_LONG_PRESS_MS  1600U
#define MINI_POWER_KEY_LONG_HOLD_MS   1600U
#define MINI_POWER_KEY_FORCE_SHUTDOWN_MS 8000U
#define MINI_POWER_KEY_FORCE_SCAN_MS      50U
#define MINI_POWER_KEY_FORCE_TASK_STACK   1024U
#define MINI_POWER_KEY_FORCE_TASK_PRIORITY (tskIDLE_PRIORITY + 4)

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#define MINI_POWER_KEY_GPIO_DEVICE "gpiob"
static volatile bool s_power_key_input_armed;
static TaskHandle_t s_power_key_force_task;
#endif

/* ---- 内部工具函数 ------------------------------------------------------- */

#ifdef CONFIG_VOICE_LINK_LINGXIN
static uint32_t s_button_gesture_id;
static service_button_event_t s_button_press;

/* Capture at the physical publisher, before a busy event bus can delay PRESS.
 * SHORT_UP precedes final click counting, so it must retain this snapshot. */
static void service_button_snapshot_event(service_button_event_t *event)
{
    voice_msg_button_action_t action = event->button.action;
    taskENTER_CRITICAL();
    if (action == VOICE_MSG_BUTTON_ACTION_PRESS_DOWN) {
        if (++s_button_gesture_id == 0U) ++s_button_gesture_id;
        s_button_press = *event;
        s_button_press.gesture_id = s_button_gesture_id;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        s_button_press.owner_generation = service_offline_action_generation();
#endif
        s_button_press.dialog_intent =
            lingxin_server_dialog_phase() == LINGXIN_DIALOG_PHASE_LISTENING ?
            SERVICE_BUTTON_DIALOG_EXIT : SERVICE_BUTTON_DIALOG_WAKE;
    } else if (action != VOICE_MSG_BUTTON_ACTION_CLICK &&
               action != VOICE_MSG_BUTTON_ACTION_SHORT_UP) {
        s_button_press.dialog_intent = SERVICE_BUTTON_DIALOG_NONE;
        if (++s_button_gesture_id == 0U) ++s_button_gesture_id;
    }
    event->gesture_id = s_button_press.gesture_id;
    event->owner_generation = s_button_press.owner_generation;
    event->dialog_intent = s_button_press.dialog_intent;
    if (action == VOICE_MSG_BUTTON_ACTION_CLICK) {
        s_button_press.dialog_intent = SERVICE_BUTTON_DIALOG_NONE;
    }
    taskEXIT_CRITICAL();
}
#endif

bool service_button_dialog_click_current(const void *data, uint32_t len)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (!data || len != sizeof(service_button_event_t)) return false;
    const service_button_event_t *event = data;
    taskENTER_CRITICAL();
    bool current = event->gesture_id != 0U &&
                   event->gesture_id == s_button_gesture_id &&
                   event->dialog_intent != SERVICE_BUTTON_DIALOG_NONE;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    current = current && event->owner_generation == service_offline_action_generation();
#endif
    taskEXIT_CRITICAL();
    return current;
#else
    (void)data; (void)len;
    return false;
#endif
}

bool service_button_wakeup_event_current(const void *data, uint32_t len)
{
    if (!data && len == 0U) return true;
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (!data || len != sizeof(service_button_wakeup_t)) return false;
    const service_button_wakeup_t *event = data;
    taskENTER_CRITICAL();
    /* After acceptance, a subsequent raw PRESS is only another candidate
     * gesture; only a new accepted owner may retire this wake. */
    bool current = event->gesture_id != 0U;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    current = current && service_offline_action_online_current(event->owner_generation);
#endif
    taskEXIT_CRITICAL();
    return current;
#else
    return false;
#endif
}

bool service_button_wakeup_request(const service_button_event_t *event)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (!service_button_dialog_click_current(event, sizeof(*event))) return false;
    struct app_datas *app = get_app_datas();
    if (!app || !(app->voice_work_mode & VOICE_WORK_MODE_BUTTON_WAKEUP) ||
        !app->can_wakeup || service_sd_music_is_syncing()) return false;
#ifdef CONFIG_OTA
    ota_state_e ota = ota_manager_get_state();
    if (ota == OTA_STATE_CHECKING || ota == OTA_STATE_UPDATING ||
        ota == OTA_STATE_PACKAGE_INFO) return false;
#endif
    service_button_wakeup_t wake = {
        .gesture_id = event->gesture_id,
        .owner_generation = event->owner_generation,
    };
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_offline_button_result_t result = service_offline_action_begin_button_for_generation(
        event->owner_generation, &wake.owner_generation);
    if (result == SERVICE_OFFLINE_BUTTON_REJECTED) return false;
    if (result == SERVICE_OFFLINE_BUTTON_OFFLINE) return true;
#endif
    /* Accepted before publishing: subscribers neither claim again nor depend
     * on subscription order. The existing wake worker owns asynchronous exit. */
    int ret = voice_msg_pub(VOICE_MSG_WAKEUP_BUTTON_START, &wake, sizeof(wake));
    if (ret != 0) {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_offline_action_close_online_window(wake.owner_generation);
#endif
        LISA_LOGE(TAG, "Button wake publish failed: owner=%u ret=%d",
                  (unsigned)wake.owner_generation, ret);
    }
    return ret == 0;
#else
    (void)event;
    return false;
#endif
}

static voice_msg_button_action_t map_lisa_btn_event(lisa_btn_event_t lisa_event)
{
    switch (lisa_event) {
    case LISA_BTN_PRESS_DOWN:
        return VOICE_MSG_BUTTON_ACTION_PRESS_DOWN;
    case LISA_BTN_PRESS_CLICK:
        return VOICE_MSG_BUTTON_ACTION_CLICK;
    case LISA_BTN_PRESS_DOUBLE_CLICK:
        return VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK;
    case LISA_BTN_PRESS_TRIPLE_CLICK:
        return VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK;
    case LISA_BTN_PRESS_QUADRUPLE_CLICK:
        return VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK;
    case LISA_BTN_PRESS_QUINTUPLE_CLICK:
        return VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK;
    case LISA_BTN_PRESS_SEXTUPLE_CLICK:
        return VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK;
    case LISA_BTN_PRESS_SEPTUPLE_CLICK:
        return VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK;
    case LISA_BTN_PRESS_REPEAT_CLICK:
        return VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK;
    case LISA_BTN_PRESS_SHORT_START:
        return VOICE_MSG_BUTTON_ACTION_SHORT_START;
    case LISA_BTN_PRESS_SHORT_UP:
        return VOICE_MSG_BUTTON_ACTION_SHORT_UP;
    case LISA_BTN_PRESS_LONG_START:
        return VOICE_MSG_BUTTON_ACTION_LONG_START;
    case LISA_BTN_PRESS_LONG_UP:
        return VOICE_MSG_BUTTON_ACTION_LONG_UP;
    case LISA_BTN_PRESS_LONG_HOLD:
        return VOICE_MSG_BUTTON_ACTION_LONG_HOLD;
    case LISA_BTN_PRESS_LONG_HOLD_UP:
        return VOICE_MSG_BUTTON_ACTION_LONG_HOLD_UP;
    default:
        return VOICE_MSG_BUTTON_ACTION_UNKNOWN;
    }
}

/*
 * 将按键事件发布到消息总线。
 *
 * OTA 升级期间忽略所有按键事件，避免升级过程中意外触发语音交互。
 * OTA 包信息确认阶段放行确认、跳过和办公模式四击，
 * 过滤 PRESS_DOWN/SHORT_UP 等原始事件，防止其他按键处理器（如 PTT 语音
 * 唤醒）误触发 INFO 页面导致 UI 在确认升级后闪烁跳转。
 */
static void publish_button_event(uint8_t btn_id, lisa_btn_event_t action)
{
    bool mode_switch = false;
#ifdef CONFIG_APP_CODEX_STATUS
    mode_switch = btn_id == 0U && action == LISA_BTN_PRESS_QUADRUPLE_CLICK;
#endif
#ifdef CONFIG_OTA
    ota_state_e ota_state = ota_manager_get_state();
    if (ota_state == OTA_STATE_UPDATING || (ota_state == OTA_STATE_CHECKING && !mode_switch)) {
        LISA_LOGI(TAG, "Ignore button %d action=%d during OTA state=%d",
                  btn_id, action, ota_state);
        return;
    }

    if (ota_state == OTA_STATE_PACKAGE_INFO) {
        if (!mode_switch && action != LISA_BTN_PRESS_CLICK &&
            action != LISA_BTN_PRESS_LONG_HOLD) {
            LISA_LOGI(TAG, "Filter button %d action=%d during OTA package info",
                      btn_id, action);
            return;
        }
    }
#endif

    if (!mode_switch && service_sd_music_is_syncing()) {
        LISA_LOGI(TAG, "Ignore button %d action=%d during SD music sync",
                  btn_id, action);
        return;
    }

    service_button_event_t evt = {
        .button = {.button_id = btn_id, .action = map_lisa_btn_event(action)},
    };
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (btn_id == 0U) service_button_snapshot_event(&evt);
#endif
    service_image_waiting_cancel();
    LISA_LOGI(TAG, "[Button %d] action=%d", btn_id, action);
    voice_msg_pub(VOICE_MSG_BUTTON_CHANGE, &evt, sizeof(evt));
}

/*
 * lisa_btn 硬件层回调，将原始事件透传给 publish_button_event 统一处理。
 *
 * 不再展开 switch-case —— lisa_btn_event_t 到业务事件的映射已由
 * map_lisa_btn_event() 集中完成，回调只负责过滤和转发。
 */
static void btn_event_callback(lisa_btn_event_t event, uint8_t btn_id, void *user)
{
    (void)user;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /*
     * The same physical press powers a battery-only board and can still be
     * held when the application button task starts.  Swallow that initial
     * gesture through its release so it can never become an immediate app
     * shutdown.  A board that starts with the key already released is armed
     * below immediately after GPIO initialization.
     */
    if (!s_power_key_input_armed) {
        if (event == LISA_BTN_PRESS_SHORT_UP ||
            event == LISA_BTN_PRESS_LONG_UP ||
            event == LISA_BTN_PRESS_LONG_HOLD_UP) {
            s_power_key_input_armed = true;
            LISA_LOGI(TAG, "Power-on key released; runtime button actions armed");
        }
        return;
    }
#endif
    publish_button_event(btn_id, event);
}

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static void power_key_force_shutdown_task(void *arg)
{
    lisa_device_t *button_gpio = (lisa_device_t *)arg;
    uint32_t held_ms = 0U;
    bool request_sent = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MINI_POWER_KEY_FORCE_SCAN_MS));
        int level = lisa_gpio_read_pin(button_gpio, POWER_KEY_PIN);

        if (level == LISA_GPIO_HIGH) {
            s_power_key_input_armed = true;
            held_ms = 0U;
            request_sent = false;
            continue;
        }
        if (level != LISA_GPIO_LOW || !s_power_key_input_armed || request_sent) {
            continue;
        }

        held_ms += MINI_POWER_KEY_FORCE_SCAN_MS;
        if (held_ms >= MINI_POWER_KEY_FORCE_SHUTDOWN_MS) {
            request_sent = true;
            LISA_LOGW(TAG, "Power key held %u ms; request emergency shutdown outside voice bus",
                      (unsigned)held_ms);
            if (!power_request_shutdown()) {
                LISA_LOGW(TAG, "Emergency shutdown already pending or worker unavailable");
            }
        }
    }
}
#endif

/* ---- 公开 API ----------------------------------------------------------- */

int service_button_init(void)
{
    static const lisa_btn_gpio_item_t mini_power_key_gpio = {
        .pin_num      = POWER_KEY_PIN,
        .iomux_func   = CSK_AON_IOMUX_FUNC_DEFAULT,
        .active_level = 0,
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        .pull_enable  = true,
        .pull_up      = true,
#else
        .pull_enable  = false,
#endif
    };

    static const lisa_btn_gpio_config_t mini_power_key_cfg = {
        .gpio_dev_name = "gpiob",
        .button_count  = 1,
        .buttons       = &mini_power_key_gpio,
        .time_config   = {
            .short_press_time = MINI_POWER_KEY_SHORT_PRESS_MS,
            .long_press_time  = MINI_POWER_KEY_LONG_PRESS_MS,
            .long_hold_time   = MINI_POWER_KEY_LONG_HOLD_MS,
            .scan_period      = 20,
            .multiple_click_interval_ms = 500U,
        },
        .callback   = btn_event_callback,
        .user_data  = NULL,
    };

    int ret = lisa_btn_gpio_init(&mini_power_key_cfg);
    if (ret != 0) {
        LISA_LOGE(TAG, "Failed to init GPIO button: %d", ret);
        return ret;
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    lisa_device_t *button_gpio = lisa_device_get(MINI_POWER_KEY_GPIO_DEVICE);
    int power_key_level = lisa_device_ready(button_gpio)
                              ? lisa_gpio_read_pin(button_gpio, POWER_KEY_PIN)
                              : -1;
    if (power_key_level == LISA_GPIO_HIGH) {
        s_power_key_input_armed = true;
    } else {
        LISA_LOGI(TAG, "Power-on key held or unavailable (level=%d); suppress actions until release",
                  power_key_level);
    }

    if (lisa_device_ready(button_gpio) && s_power_key_force_task == NULL) {
        BaseType_t task_ret = xTaskCreate(power_key_force_shutdown_task,
                                          "power.force",
                                          MINI_POWER_KEY_FORCE_TASK_STACK,
                                          button_gpio,
                                          MINI_POWER_KEY_FORCE_TASK_PRIORITY,
                                          &s_power_key_force_task);
        if (task_ret != pdPASS) {
            s_power_key_force_task = NULL;
            LISA_LOGE(TAG, "Failed to create emergency power-key monitor: %d", (int)task_ret);
        }
    }
#endif

    return 0;
}
