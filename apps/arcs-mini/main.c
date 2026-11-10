#include "FreeRTOS.h"
#include "task.h"
#include <errno.h>
#include "project_version.h"

#define TAG "main"

#include "lisa_log.h"

#include "service_led.h"
#include "service_volume.h"
#include "service_button.h"
#if CONFIG_LISA_DISPLAY_DEVICE
#include "service_brightness.h"
#endif
#if CONFIG_LISA_CAMERA_DEVICE
#include "service_camera.h"
#endif
#include "service_alarm.h"
#include "service_image.h"
#include "service_sd_music.h"
#include "service_motutu_web.h"
#include "car_motion.h"
#include "motutu_motion.h"
#include "voice_motutu_control.h"
#include "voice_car_control.h"
#include "voice_lingxin_action.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "app_netcfg_portal.h"
#include "app_manual_netcfg.h"
#include "lisa_device.h"
#include "lisa_wdt.h"
#include "service_v2_gait.h"
#include "service_v2_touch_action.h"
#include "service_v2_imu_interaction.h"
#include "service_voice_action.h"
#include "service_v2_actuators.h"
#include "service_v2_rgb_effect.h"
#ifdef CONFIG_APP_CODEX_STATUS
#include "service_codex_status.h"
#endif
#include "service_v2_nfc_peer.h"
#include "service_v2_sensors.h"
#include "service_v2_voice_standby.h"
#include "service_offline_action.h"
#include "service_v2_low_battery.h"
#endif
#include "alarm_ring.h"
#include "alarm.h"
#include "alarm_handler.h"

#include "voice_msg.h"
#include "lisa_kv.h"
#include "kv_user.h"
#include "sys_network_manager.h"
#include "sys_network_probe.h"
#include "sys_wifi.h"
#include "tone.h"
#include "app_player.h"
#include "pa_manager.h"
#include "voice_player_comm.h"
#include "voice_player.h"
#include "power/power_manager.h"
#include "uboot_features_api.h"
#if CONFIG_LISA_DISPLAY_DEVICE
#include "lisa_display.h"
#if CONFIG_APPLICATION_UI
#include "lisa_ui_nav_scr.h"
#include "apps/llm/lisa_ui_nav_scr_ids.h"
#include "apps/llm/models/model_qrcode.h"
#endif
#endif
#include "battery/battery.h"
#include "voice_cloud.h"
#ifdef CONFIG_VOICE_LINK_LINGXIN
#include "lingxin_server.h"
#endif
#include "app_ble_common.h"
#if CONFIG_LISA_CAMERA_DEVICE
#include "button_camera_preview.h"
#endif
#include "bt_app_hal.h"
#include "ota_manager.h"

extern int boot_watchdog_feed(void);

#if defined(CONFIG_LISA_KV_TYPE_EF) && defined(CONFIG_DISK_DRIVER_FLASH)
#define APP_EF_FLASH_OFFSET (CONFIG_LS_EF_START_ADDR - CONFIG_DISK_FLASH_CHIP_BASE_ADDR)
_Static_assert(APP_EF_FLASH_OFFSET + CONFIG_LS_EF_ENV_AREA_SIZE <= CONFIG_DISK_FLASH_START_ADDR ||
                   CONFIG_DISK_FLASH_START_ADDR + CONFIG_DISK_FLASH_VOLUME_SIZE <= APP_EF_FLASH_OFFSET,
               "EasyFlash KV overlaps the flash filesystem");
#if defined(CONFIG_WIFI_MGR_NVS_PARTITION_OFFSET) && defined(CONFIG_WIFI_MGR_NVS_PARTITION_SIZE)
_Static_assert(APP_EF_FLASH_OFFSET + CONFIG_LS_EF_ENV_AREA_SIZE <= CONFIG_WIFI_MGR_NVS_PARTITION_OFFSET ||
                   CONFIG_WIFI_MGR_NVS_PARTITION_OFFSET + CONFIG_WIFI_MGR_NVS_PARTITION_SIZE <= APP_EF_FLASH_OFFSET,
               "EasyFlash KV overlaps WiFi NVS");
_Static_assert(CONFIG_DISK_FLASH_START_ADDR + CONFIG_DISK_FLASH_VOLUME_SIZE <=
                   CONFIG_WIFI_MGR_NVS_PARTITION_OFFSET,
               "flash filesystem overlaps WiFi NVS");
#endif
#undef APP_EF_FLASH_OFFSET
#endif

#if !CONFIG_APPLICATION_UI
static uint8_t model_voice_cloud_is_running(void)
{
    return 0;
}

enum {
    QR_STATUS_NOT_CONNECTED = 0,
    QR_STATUS_CONNECTED = 1,
    QR_STATUS_CUSTOM_INFO = 2,
    QR_STATUS_BIND = 3,
    QR_STATUS_AUTH_FAILED = 4,
};
#endif

#define FACTORY_RESET_TONE_POLL_MS 20U
#define MOTUTU_BOOT_PROMPT_DELAY_MS 1200U
#define STATIC_WIFI_READY_TIMEOUT_MS 15000U
#define STATIC_WIFI_READY_POLL_MS 100U
#define V2_BOOT_IMU_SETTLE_MS 200U
#if defined(CONFIG_BOARD_ARCS_MINI_V2_1) && defined(CONFIG_LISA_WDT)
/*
 * 32 kHz WDT 只提供离散超时档位：请求 10 s 会向上取整到 16.384 s，
 * 再经过约 0.512 s 的 reset stage 整机复位。不要使用 legacy
 * CONFIG_WATCHDOG_ENABLE：它的 ISR 会置 recovery request，第一次超时就不再
 * 自动回应用。这里只由应用 main 循环喂狗，不能另建无条件喂狗任务掩盖卡死。
 */
#define V2_SYSTEM_WATCHDOG_DEVICE_NAME       "wdt0"
#define V2_SYSTEM_WATCHDOG_INT_TIMEOUT_MS  10000U
#define V2_SYSTEM_WATCHDOG_RST_TIMEOUT_MS    500U

static lisa_device_t *s_v2_system_watchdog;
static bool s_v2_system_watchdog_running;
static bool s_v2_system_watchdog_feed_error_logged;

static void app_v2_system_watchdog_expired(void *user_data)
{
    /* ISR 必须立即返回；硬件 reset stage 会继续完成整机复位。 */
    (void)user_data;
}

static int app_v2_system_watchdog_start(void)
{
    lisa_wdt_config_t config = {
        .int_timeout_ms = V2_SYSTEM_WATCHDOG_INT_TIMEOUT_MS,
        .rst_timeout_ms = V2_SYSTEM_WATCHDOG_RST_TIMEOUT_MS,
    };
    int ret;

    s_v2_system_watchdog = lisa_device_get(V2_SYSTEM_WATCHDOG_DEVICE_NAME);
    if (!lisa_device_ready(s_v2_system_watchdog)) {
        return -ENODEV;
    }

    ret = lisa_wdt_setup(s_v2_system_watchdog, &config);
    if (ret != LISA_DEVICE_OK) {
        return ret;
    }
    ret = lisa_wdt_set_callback(s_v2_system_watchdog, app_v2_system_watchdog_expired, NULL);
    if (ret != LISA_DEVICE_OK) {
        return ret;
    }
    ret = lisa_wdt_start(s_v2_system_watchdog);
    if (ret != LISA_DEVICE_OK) {
        return ret;
    }

    s_v2_system_watchdog_running = true;
    ret = lisa_wdt_feed(s_v2_system_watchdog);
    if (ret != LISA_DEVICE_OK) {
        return ret;
    }

    LISA_LOGI(TAG, "system watchdog armed: requested=%ums reset-stage=%ums effective-total~=16896ms",
              (unsigned)V2_SYSTEM_WATCHDOG_INT_TIMEOUT_MS,
              (unsigned)V2_SYSTEM_WATCHDOG_RST_TIMEOUT_MS);
    return 0;
}

static void app_v2_system_watchdog_feed(void)
{
    if (!s_v2_system_watchdog_running) {
        return;
    }

    int ret = lisa_wdt_feed(s_v2_system_watchdog);
    if (ret != LISA_DEVICE_OK) {
        if (!s_v2_system_watchdog_feed_error_logged) {
            LISA_LOGE(TAG, "system watchdog feed failed: %d", ret);
            s_v2_system_watchdog_feed_error_logged = true;
        }
    } else {
        s_v2_system_watchdog_feed_error_logged = false;
    }
}
#endif

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#define WIFI_NETCFG_FALLBACK_POLL_MS 1000U
static bool s_wifi_netcfg_fallback_attempted;
static TickType_t s_wifi_netcfg_fallback_last_poll;
#endif

static volatile bool s_ble_netcfg_entering = false;
static bool s_netcfg_tone_latched = false;
static bool s_netcfg_tone_retry_pending = false;
static TickType_t s_netcfg_tone_retry_tick = 0;
static bool s_cloud_ever_connected = false;
static bool s_bind_prompted_before_first_cloud_ok = false;
static bool s_runtime_wifi_prompted = false;
static bool s_user_forced_netcfg = false;
static bool s_boot_probe_fail_prompted = false;
static volatile bool s_wifi_clear_netcfg_entering = false;

static bool app_clear_all_saved_wifi(const char *source)
{
    int ret = sys_wifi_clear_saved_aps();

    if (ret != 0) {
        LISA_LOGE(TAG, "%s failed to clear saved WiFi APs: %d", source, ret);
        return false;
    }
    LISA_LOGI(TAG, "%s cleared all saved WiFi APs", source);
    return true;
}

#if CONFIG_APP_STATIC_WIFI_BOOTSTRAP
static volatile bool s_static_wifi_bootstrap_pending;

static bool app_static_wifi_is_configured(void)
{
    return CONFIG_APP_STATIC_WIFI_SSID[0] != '\0';
}

static void app_static_wifi_bootstrap_task(void *arg)
{
    sys_network_status_t network_status = {0};
    uint32_t waited_ms = 0;
    int ret;

    (void)arg;

    while (!sys_wifi_is_ready() && waited_ms < STATIC_WIFI_READY_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(STATIC_WIFI_READY_POLL_MS));
        waited_ms += STATIC_WIFI_READY_POLL_MS;
    }

    if (!sys_wifi_is_ready()) {
        LISA_LOGW(TAG, "static WiFi bootstrap timed out waiting for WiFi manager");
        goto out;
    }

    if (!app_static_wifi_is_configured() || sys_wifi_has_ap()) {
        goto out;
    }

    if (sys_network_get_status(&network_status) != 0 ||
        network_status.mode != SYS_NETWORK_MODE_WIFI) {
        LISA_LOGI(TAG, "static WiFi bootstrap skipped outside WiFi mode");
        goto out;
    }

    ret = sys_wifi_save_ap(CONFIG_APP_STATIC_WIFI_SSID,
                           CONFIG_APP_STATIC_WIFI_PASSWORD,
                           NULL);
    if (ret != 0) {
        LISA_LOGE(TAG, "static WiFi bootstrap failed to save credentials: %d", ret);
        goto out;
    }

    LISA_LOGI(TAG, "static WiFi bootstrap credentials saved");

out:
    s_static_wifi_bootstrap_pending = false;
    vTaskDelete(NULL);
}
#endif

static uint32_t app_normalize_cloud_info_status(uint32_t status)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (status == QR_STATUS_BIND || status == QR_STATUS_AUTH_FAILED) {
        return QR_STATUS_CONNECTED;
    }
#endif

    return status;
}

#ifndef CONFIG_BOARD_ARCS_MINI_V2_1
static void motutu_boot_feedback_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(MOTUTU_BOOT_PROMPT_DELAY_MS));

    if (motutu_motion_is_ready()) {
        (void)motutu_stand();
    }
    voice_player_play_tone_url(app_tone_get_url(TONE_ID_0));
    vTaskDelete(NULL);
}

#endif

static bool app_cloud_info_page_enabled(void)
{
#if defined(CONFIG_BOARD_ARCS_MINI_DOLL_V2) || !CONFIG_LISA_DISPLAY_DEVICE
    return false;
#else
    return true;
#endif
}

static bool app_should_open_info_by_cloud_state(uint32_t *status_out)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    (void)status_out;
    return false;
#else
    uint32_t status = QR_STATUS_CONNECTED;
    voice_cloud_state_t cloud_state = voice_cloud_get_state();

    switch (cloud_state) {
    case VOICE_CLOUD_STATE_CONNECTING:
        return false;
    case VOICE_CLOUD_STATE_NO_NETWORK:
    case VOICE_CLOUD_STATE_NO_INTERNET:
        status = QR_STATUS_NOT_CONNECTED;
        break;
    case VOICE_CLOUD_STATE_TOKEN_FAILED:
        status = QR_STATUS_AUTH_FAILED;
        break;
    case VOICE_CLOUD_STATE_CONNECT_FAILED:
        status = QR_STATUS_BIND;
        break;
    default:
        return false;
    }

    if (status_out) {
        *status_out = app_normalize_cloud_info_status(status);
    }

    // #region debug-point A:legacy-cloud-gate
    LISA_LOGI(TAG, "cloud info gate, raw_state=%d, status=%u", (int)cloud_state, (unsigned)status);
    // #endregion

    return true;
#endif
}

static void app_open_cloud_info(uint32_t status)
{
#if CONFIG_WIFI_MANAGER
    status = app_normalize_cloud_info_status(status);
    voice_msg_pub(VOICE_MSG_CLOUD_OPEN_INFO, &status, sizeof(status));
#else
    (void)status;
#endif
}

static bool app_should_play_netcfg_tone(uint32_t status)
{
    sys_wifi_connect_result_t connect_result;

    if (status != QR_STATUS_NOT_CONNECTED) {
        return false;
    }

    if (!sys_wifi_has_ap()) {
        return true;
    }

    connect_result = sys_wifi_get_connect_result();
    if (connect_result == SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP ||
        connect_result == SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD ||
        connect_result == SYS_WIFI_CONNECT_RESULT_FAIL_IP ||
        connect_result == SYS_WIFI_CONNECT_RESULT_FAIL_OTHER) {
        return true;
    }

    switch (voice_cloud_get_state()) {
    case VOICE_CLOUD_STATE_CONNECTING:
        return false;
    case VOICE_CLOUD_STATE_NO_INTERNET:
        return true;
    default:
        return false;
    }
}

static bool app_try_play_netcfg_tone(uint32_t status)
{
    static TickType_t s_last_prompt_tick = 0;
    TickType_t now = xTaskGetTickCount();

    if (!app_should_play_netcfg_tone(status)) {
        s_netcfg_tone_retry_pending = false;
        return false;
    }

    if (s_netcfg_tone_latched) {
        s_netcfg_tone_retry_pending = false;
        return false;
    }

    if (s_last_prompt_tick == 0 || (now - s_last_prompt_tick) >= pdMS_TO_TICKS(1000)) {
        voice_player_play_tone_url(app_tone_get_url(TONE_ID_70));
        s_last_prompt_tick = now;
        s_netcfg_tone_latched = true;
        s_netcfg_tone_retry_pending = false;
        return true;
    }

    s_netcfg_tone_retry_pending = true;
    s_netcfg_tone_retry_tick = now + pdMS_TO_TICKS(1000);
    return false;
}

static void app_prompt_cloud_info(uint32_t status)
{
#if CONFIG_WIFI_MANAGER
    app_try_play_netcfg_tone(status);
    app_open_cloud_info(status);
#else
    (void)status;
#endif
}

static void factory_reset_wait_tone_finished(void)
{
    bool tone_started = false;

    while (1) {
        switch (app_player_get_state(tone_player)) {
        case APP_PLAYER_STATE_PREPARING:
        case APP_PLAYER_STATE_PREPARED:
        case APP_PLAYER_STATE_PLAYING:
        case APP_PLAYER_STATE_PAUSED:
            tone_started = true;
            break;
        case APP_PLAYER_STATE_STOPPED:
        case APP_PLAYER_STATE_ERROR:
            return;
        default:
            if (tone_started) {
                return;
            }
            break;
        }

        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_TONE_POLL_MS));
    }
}

static void app_ble_netcfg_enter_task(void *arg)
{
    sys_network_status_t status = {0};
    bool status_ok;

    (void)arg;

#if CONFIG_LISA_CAMERA_DEVICE
    if (!button_camera_preview_wait_exit()) {
        LISA_LOGW(TAG, "Timed out waiting camera preview to exit before BLE config");
    }
#endif

    status_ok = sys_network_get_status(&status) == 0;

    LISA_LOGI(TAG, "power button multi click, enter BLE config");
    s_user_forced_netcfg = true;
    service_alarm_deinit();
    kv_user_clear_sd_card_sync();
    LISA_LOGI(TAG, "TF card sync KV cleared before BLE config");

    if (!status_ok || status.active_bearer != SYS_NETWORK_BEARER_MODEM) {
#ifdef CONFIG_VOICE_LINK_LINGXIN
        int stop_ret = lingxin_server_chat_stop();
        if (stop_ret != 0) {
            LISA_LOGW(TAG, "Lingxin stop before BLE netcfg returned %d", stop_ret);
        }
#else
        voice_cloud_disconnect();
#endif
    } else {
        LISA_LOGI(TAG, "keep voice cloud connected on modem bearer before BLE config");
    }

    app_ble_netcfg_adv_start_delayed();
    app_prompt_cloud_info(QR_STATUS_NOT_CONNECTED);

    s_ble_netcfg_entering = false;
    vTaskDelete(NULL);
}

static void app_ble_netcfg_enter_async(void)
{
    BaseType_t ret;

    if (s_ble_netcfg_entering) {
        LISA_LOGI(TAG, "BLE netcfg enter already in progress");
        return;
    }

#if CONFIG_LISA_MODEM
    {
        sys_network_status_t net_status = {0};
        if (sys_network_get_status(&net_status) == 0 && net_status.mode == SYS_NETWORK_MODE_MODEM) {
            if (net_status.connected) {
#ifdef CONFIG_VOICE_LINK_LINGXIN
                LISA_LOGI(TAG, "4G online, Lingxin link active, skip legacy bind QR");
                return;
#else
                LISA_LOGI(TAG, "4G online, show binding QR");
                app_prompt_cloud_info(QR_STATUS_BIND);
                return;
#endif
            }
            LISA_LOGI(TAG, "4G offline, switch to WiFi mode for netcfg");
            sys_network_request_mode(SYS_NETWORK_MODE_WIFI, true);
        }
    }
#endif

    s_ble_netcfg_entering = true;
    ret = xTaskCreate(app_ble_netcfg_enter_task, "ble_netcfg", 3072, NULL, 5, NULL);
    if (ret != pdPASS) {
        s_ble_netcfg_entering = false;
        LISA_LOGE(TAG, "Failed to create BLE netcfg task");
    }
}

void factory_reset(void)
{
    LOGI("factory reset running...");
    lisa_kv_clear();
    if (sys_wifi_clear_saved_aps() != 0) {
        LISA_LOGW(TAG, "Failed to reset WiFi state");
    }
    LOGI("factory reset done.");
    LOGI("Wait for factory reset tone playback before reboot.");
    const char *tone_url = app_tone_get_url(TONE_ID_103);
    if (tone_url && app_player_play(tone_player, tone_url) == APP_PLAYER_OK) {
        factory_reset_wait_tone_finished();
    } else {
        LISA_LOGW(TAG, "Failed to play factory reset tone (url=%p), reboot immediately", (void *)tone_url);
    }
    power_reboot_soft();
}

static void voice_system_network_probe_success(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    if (msg_id == VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS && !network_probe_event_current(data, len, true)) return;
    (void)unused;
    (void)msg_id;
    (void)data;
    (void)len;
    (void)user_data;

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* Also subscribed to IP_GOT: retain success even if link loss happens
     * between main's polls. Runtime recovery must never become boot setup. */
    __atomic_store_n(&s_wifi_netcfg_fallback_attempted, true, __ATOMIC_RELEASE);
#endif
    s_boot_probe_fail_prompted = false;
}

static void voice_cloud_connected(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    (void)unused;
    (void)msg_id;
    (void)data;
    (void)len;
    (void)user_data;

    s_netcfg_tone_latched = false;
    s_cloud_ever_connected = true;
    s_bind_prompted_before_first_cloud_ok = false;
    s_runtime_wifi_prompted = false;
    s_user_forced_netcfg = false;
    s_boot_probe_fail_prompted = false;
}

static void app_open_info_with_tone(uint32_t status, uint16_t tone_id)
{
    status = app_normalize_cloud_info_status(status);

    if (tone_id != TONE_ID_0) {
        voice_player_play_tone_url(app_tone_get_url(tone_id));
    }

    app_open_cloud_info(status);
}


static void voice_cloud_auth_failed(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    (void)unused;
    (void)msg_id;
    (void)data;
    (void)len;
    (void)user_data;

#ifdef CONFIG_VOICE_LINK_LINGXIN
    LISA_LOGI(TAG, "ignore legacy cloud auth failed event in Lingxin mode");
    return;
#endif

    if (s_user_forced_netcfg || s_cloud_ever_connected || s_bind_prompted_before_first_cloud_ok) {
        return;
    }

    app_open_cloud_info(QR_STATUS_AUTH_FAILED);
    s_bind_prompted_before_first_cloud_ok = true;
}


static void voice_cloud_auth_success(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    int network_mode = SYS_NETWORK_MODE_WIFI;

#ifdef CONFIG_VOICE_LINK_LINGXIN
    LISA_LOGI(TAG, "ignore legacy cloud auth success event in Lingxin mode");
    return;
#endif

    service_alarm_init();
    (void)lisa_kv_get_int("user.network_mode", &network_mode);
#if CONFIG_MIDDLEWARE_SD_MUSIC
    if (network_mode != SYS_NETWORK_MODE_MODEM) {
        service_sd_music_init();
    }
#else
    (void)network_mode;
#endif
}

#ifdef CONFIG_OTA
static bool app_should_defer_wifi_provision_prompt(void)
{
    switch (ota_manager_get_state()) {
    case OTA_STATE_CHECKING:
    case OTA_STATE_PACKAGE_INFO:
    case OTA_STATE_UPDATING:
    case OTA_STATE_FAILED:
        return true;
    default:
        return false;
    }
}
#endif

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static void app_wifi_netcfg_fallback_disable(void)
{
    __atomic_store_n(&s_wifi_netcfg_fallback_attempted, true, __ATOMIC_RELEASE);
}

/*
 * Automatic setup is a first-boot connection fallback, never runtime recovery.
 * Manager exhaustion may be one transient missed scan. After any successful
 * connection, leave every later outage to the saved-AP recovery worker.
 */
static void app_wifi_netcfg_fallback_poll(void)
{
    sys_network_status_t status = {0};
    TickType_t now = xTaskGetTickCount();
    int ret;

    if (s_wifi_netcfg_fallback_last_poll != 0 &&
        (TickType_t)(now - s_wifi_netcfg_fallback_last_poll) < pdMS_TO_TICKS(WIFI_NETCFG_FALLBACK_POLL_MS)) {
        return;
    }
    s_wifi_netcfg_fallback_last_poll = now;

    if (sys_wifi_is_connected()) {
        app_wifi_netcfg_fallback_disable();
        return;
    }

    if (app_netcfg_portal_is_active()) {
        app_wifi_netcfg_fallback_disable();
        return;
    }
    if (__atomic_load_n(&s_wifi_netcfg_fallback_attempted, __ATOMIC_ACQUIRE) ||
        service_offline_action_network_busy() || sys_wifi_radio_suspended() ||
        service_v2_voice_standby_background_audio_suppressed() ||
        sys_network_get_status(&status) != 0 || status.mode != SYS_NETWORK_MODE_WIFI ||
        !status.wifi_available || status.switching || status.wifi_connected) return;

#if CONFIG_APP_STATIC_WIFI_BOOTSTRAP
    if (s_static_wifi_bootstrap_pending) {
        return;
    }
#endif

    if (!sys_wifi_reconnect_saved_exhausted()) return;

    /* The manager/worker can finish or claim a recovery while exhaustion is
     * sampled. Give that newer state priority over this boot-only fallback. */
    if (sys_wifi_is_connected()) {
        app_wifi_netcfg_fallback_disable();
        return;
    }
    if (__atomic_load_n(&s_wifi_netcfg_fallback_attempted, __ATOMIC_ACQUIRE) ||
        service_offline_action_network_busy()) return;
    app_wifi_netcfg_fallback_disable();
    LISA_LOGW(TAG, "initial saved WiFi cycle exhausted before first connection, entering netcfg portal");
    ret = app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_CONNECT_FAILED);
    if (ret == 0 || ret == -EALREADY) {
        app_ble_netcfg_adv_start_delayed();
        return;
    }

    LISA_LOGE(TAG, "failed to start netcfg fallback portal: %d", ret);
    /* Keep the episode consumed on resource failure; manual entry remains available. */
}

#endif

static void voice_wifi_provision_guard(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    if (msg_id == VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL && !network_probe_event_current(data, len, false)) return;
    bool mark_boot_probe_prompted = false;

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_network_prompt_allowed() || sys_wifi_radio_suspended() || service_v2_voice_standby_background_audio_suppressed()) return;
    /* Link/DNS/cloud failure messages arrive before a complete candidate cycle.
     * The fallback poll opens provisioning after manager exhaustion. */
    sys_network_status_t guard_status = {0};
    if (sys_network_get_status(&guard_status) == 0 && guard_status.mode == SYS_NETWORK_MODE_WIFI &&
        (sys_wifi_is_connected() || sys_wifi_has_ap())) return;
#endif

#if CONFIG_APP_STATIC_WIFI_BOOTSTRAP
    if (s_static_wifi_bootstrap_pending && !sys_wifi_has_ap()) {
        LISA_LOGI(TAG, "defer network failure prompt while static WiFi credentials are being restored");
        return;
    }
#endif

    if (msg_id == VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL) {
        if (s_user_forced_netcfg || s_cloud_ever_connected || s_boot_probe_fail_prompted) {
            return;
        }
        mark_boot_probe_prompted = true;
    }

    if (msg_id == VOICE_MSG_WIFI_DISCONNECTED) {
        if (s_cloud_ever_connected && !s_user_forced_netcfg && !s_runtime_wifi_prompted) {
            uint32_t status = QR_STATUS_NOT_CONNECTED;
            (void)app_should_open_info_by_cloud_state(&status);
            app_open_cloud_info(status);
            s_runtime_wifi_prompted = true;
        }

        if (s_cloud_ever_connected || s_user_forced_netcfg) {
            return;
        }
    }

    if (s_user_forced_netcfg) {
        return;
    }

    if (msg_id == VOICE_MSG_SYSTEM_NETWORK_SWITCH_DONE && sys_wifi_has_ap()) {
        return;
    }

    /* 4G 模式：NETWORK_SWITCH_DONE 仅表示模组启动完成，网络探测（SNTP）尚未结束。
     * 此时不应播报配网提示音，应等待 NETWORK_PROBE_FAIL 再做最终判断。 */
    if (msg_id == VOICE_MSG_SYSTEM_NETWORK_SWITCH_DONE) {
        sys_network_status_t _guard_net_status;
        if (sys_network_get_status(&_guard_net_status) == 0 &&
            _guard_net_status.active_bearer == SYS_NETWORK_BEARER_MODEM) {
            return;
        }
    }

#ifdef CONFIG_OTA
    if (app_should_defer_wifi_provision_prompt()) {
        LISA_LOGI(TAG, "OTA is active, defer WiFi provisioning page");
        return;
    }
#endif

    uint32_t status = QR_STATUS_CONNECTED;
    if (!app_should_open_info_by_cloud_state(&status)) {
        return;
    }

    LISA_LOGI(TAG, "Cloud unavailable(%u), keep info page on top", (unsigned)status);
    app_open_cloud_info(status);
    app_try_play_netcfg_tone(status);

    if (mark_boot_probe_prompted) {
        s_boot_probe_fail_prompted = true;
    }
}

static bool app_try_handle_ota_package_info(const voice_msg_button_evt_t *evt)
{
#ifdef CONFIG_OTA
    if (evt == NULL || evt->button_id != 0 || ota_manager_get_state() != OTA_STATE_PACKAGE_INFO) {
        return false;
    }

    if (!ota_manager_app_update_input_ready()) {
        LISA_LOGI(TAG, "Ignore button action=%d during OTA package info guard window", evt->action);
        return true;
    }

    if (evt->action == VOICE_MSG_BUTTON_ACTION_CLICK) {
        if (ota_manager_confirm_app_update() == 0) {
            LISA_LOGI(TAG, "User confirmed app OTA update");
        } else {
            LISA_LOGW(TAG, "Failed to confirm app OTA update");
        }
        return true;
    }

    if (evt->action == VOICE_MSG_BUTTON_ACTION_LONG_HOLD) {
        if (ota_manager_skip_app_update() == 0) {
            LISA_LOGI(TAG, "User deferred app OTA update");
        } else {
            LISA_LOGW(TAG, "Failed to defer app OTA update");
        }
        return true;
    }

    LISA_LOGI(TAG, "Ignore button action=%d while waiting app OTA confirmation", evt->action);
    return true;
#else
    (void)evt;
    return false;
#endif
}

static bool app_button_action_should_redirect_cloud_info(voice_msg_button_action_t action)
{
    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK:
        return true;
    default:
        return false;
    }
}

static void app_prepare_shutdown_audio(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    voice_player_begin_shutdown_prompt();
    service_offline_action_shutdown();
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    int prompt_ret = voice_player_play_shutdown_prompt_sync();
    if (prompt_ret != 0) {
        LISA_LOGW(TAG, "shutdown prompt unavailable: %d", prompt_ret);
    }
    voice_player_abort_shutdown_prompt();
#endif
}

static void app_shutdown_returned(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_offline_action_shutdown_cancelled();
    service_v2_voice_standby_shutdown_cancelled();
#endif
}

static void app_play_netcfg_enter_prompt(void)
{
    const char *url = app_tone_get_url(TONE_ID_70);

    /* Triple-click is an explicit user acknowledgement point.  Prefer the
     * immediate path, but retain the queued path when another short prompt is
     * just finishing so repeated triple-clicks never become silent. */
    if (!voice_player_try_play_prompt_tone_now_url(url)) {
        voice_player_play_prompt_tone_url(url);
    }
}

int app_enter_manual_netcfg(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    int ret;

    s_user_forced_netcfg = true;
    if (app_netcfg_portal_is_active()) {
        ret = app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST);
        if (ret != 0 && ret != -EALREADY) return ret;
        LISA_LOGI(TAG, "power button triple click, netcfg portal already active");
        app_ble_netcfg_adv_start_delayed();
        app_play_netcfg_enter_prompt();
        return 0;
    }

#ifdef CONFIG_VOICE_LINK_LINGXIN
    /* V2.1 has a single Wi-Fi VIF. Ask the voice session to release its
     * WebSocket before the portal task changes STA into SoftAP; the bounded
     * pre-AP scan also gives asynchronous close callbacks time to drain. */
    ret = lingxin_server_chat_stop();
    if (ret != 0) {
        LISA_LOGW(TAG, "Lingxin stop before web netcfg returned %d", ret);
    }
#endif
    service_alarm_deinit();
    ret = app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST);
    if (ret != 0) {
        if (ret == -EBUSY || ret == -EINPROGRESS || ret == -EAGAIN) return ret;
        LISA_LOGE(TAG, "power button triple click, failed to start netcfg portal: %d", ret);
        voice_player_play_prompt_tone_url(app_tone_get_url(TONE_ID_65));
        return ret;
    }

    /* V2.1 supports Wi-Fi/BLE coexistence. Keep the browser portal and the
     * original mini-program BLE provisioning discoverable at the same time. */
    app_ble_netcfg_adv_start_delayed();
    app_play_netcfg_enter_prompt();
    LISA_LOGI(TAG, "manual netcfg portal scheduled");
    return 0;
#else
    LISA_LOGI(TAG, "power button triple click, web netcfg portal is unavailable on this board");
    voice_player_play_prompt_tone_url(app_tone_get_url(TONE_ID_65));
    return -ENOTSUP;
#endif
}

static void app_enter_web_netcfg_by_button(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_offline_action_request_netcfg();
#else
    (void)app_enter_manual_netcfg();
#endif
}

static void app_clear_all_wifi_and_enter_netcfg_task(void *arg)
{
    int ret = 0;
    const char *clear_prompt = app_tone_get_url(TONE_ID_113);

    (void)arg;
    s_user_forced_netcfg = true;
    service_alarm_deinit();
    kv_user_clear_sd_card_sync();

#ifdef CONFIG_VOICE_LINK_LINGXIN
    ret = lingxin_server_chat_stop();
    if (ret != 0) {
        LISA_LOGW(TAG, "Lingxin stop before WiFi clear returned %d", ret);
    }
#else
    voice_cloud_disconnect();
#endif

    if (!app_clear_all_saved_wifi("six-click")) {
        voice_player_play_prompt_tone_url(app_tone_get_url(TONE_ID_65));
        s_wifi_clear_netcfg_entering = false;
        vTaskDelete(NULL);
        return;
    }

    if (clear_prompt == NULL) {
        LISA_LOGE(TAG, "WiFi clear prompt tone is unavailable");
    } else if (!voice_player_try_play_prompt_tone_now_url(clear_prompt)) {
        voice_player_play_prompt_tone_url(clear_prompt);
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!app_netcfg_portal_is_active()) {
        ret = app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST);
        if (ret != 0) {
            LISA_LOGE(TAG, "six-click failed to start netcfg portal: %d", ret);
            voice_player_play_prompt_tone_url(app_tone_get_url(TONE_ID_65));
        }
    }
#endif
    app_ble_netcfg_adv_start_delayed();
    LISA_LOGI(TAG, "Six clicks: all saved WiFi APs cleared; provisioning started");

    s_wifi_clear_netcfg_entering = false;
    vTaskDelete(NULL);
}

static void app_clear_all_wifi_and_enter_netcfg_async(void)
{
    if (s_wifi_clear_netcfg_entering) {
        LISA_LOGI(TAG, "WiFi clear/provisioning task already in progress");
        return;
    }

    s_wifi_clear_netcfg_entering = true;
    if (xTaskCreate(app_clear_all_wifi_and_enter_netcfg_task,
                    "wifi.clear_cfg", 3072, NULL, 5, NULL) != pdPASS) {
        s_wifi_clear_netcfg_entering = false;
        LISA_LOGE(TAG, "Failed to create WiFi clear/provisioning task");
        voice_player_play_prompt_tone_url(app_tone_get_url(TONE_ID_65));
    }
}

#ifdef CONFIG_VOICE_LINK_LINGXIN
static void app_play_dialog_exit_prompt(void)
{
    const char *url = app_tone_get_url(TONE_ID_114);

    if (url == NULL) {
        LISA_LOGE(TAG, "Single click exit prompt is unavailable");
    } else if (!voice_player_try_play_prompt_tone_now_url(url)) {
        voice_player_play_prompt_tone_url(url);
    }
}

static bool app_lingxin_single_click_execute(const service_button_event_t *event)
{
    if (!service_button_dialog_click_current(event, sizeof(*event))) return false;
    if (event->dialog_intent == SERVICE_BUTTON_DIALOG_WAKE) {
        /* The existing wake worker waits for old playback/transport cleanup.
         * Never reinterpret this intent as exit when the SDK changes state. */
        return service_button_wakeup_request(event);
    }
    if (event->dialog_intent == SERVICE_BUTTON_DIALOG_EXIT) {
        (void)lingxin_server_chat_end_record();
        (void)lingxin_server_chat_stop();
        app_play_dialog_exit_prompt();
        return true;
    }
    return false;
}

#endif

static void button_changed(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    voice_msg_button_evt_t *evt = (voice_msg_button_evt_t *)data;
    bool should_redirect_cloud_info = false;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    bool local_exit_checked = false;
#endif

    if (!evt || len < sizeof(*evt)) {
        return;
    }

    if (evt->button_id != 0) {
        return;
    }

#ifdef CONFIG_APP_CODEX_STATUS
    if (evt->action == VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK) {
        service_codex_status_toggle_mode();
        return;
    }
#endif

    if (app_try_handle_ota_package_info(evt)) {
        return;
    }

#ifdef CONFIG_VOICE_LINK_LINGXIN
    /* Reject a retired physical click before it can cancel a newer local action.
     * OTA's dedicated confirmation path still accepts its legacy short payload. */
    if (evt->action == VOICE_MSG_BUTTON_ACTION_CLICK && len == sizeof(service_button_event_t)) {
        /* Both helpers below only change local policy flags. Keep the owner
         * check and local cancellation atomic, never chat/audio waits. */
        taskENTER_CRITICAL();
        bool current = service_button_dialog_click_current(data, len);
        bool consumed = false;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (current) consumed = service_offline_action_button_exit(evt->action);
        local_exit_checked = true;
#endif
        taskEXIT_CRITICAL();
        if (!current || consumed) return;
    }
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!local_exit_checked && service_offline_action_button_exit(evt->action)) return;
    if (evt->action == VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK) {
        int ret = service_v2_voice_standby_request();

        if (ret == 0) {
            LISA_LOGI(TAG, "Five clicks: voice standby requested");
        } else {
            LISA_LOGE(TAG, "Five clicks: voice standby request failed: %d", ret);
        }
        return;
    }
    service_v2_voice_standby_note_activity();
#endif

    /* Six clicks deliberately clears credentials.  Keep this local recovery
     * path ahead of cloud/UI redirection; five clicks remains manual standby. */
    if (evt->action == VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK) {
        app_clear_all_wifi_and_enter_netcfg_async();
        return;
    }

    /* Provisioning must remain available even when cloud state/UI handling is unavailable. */
    if (evt->action == VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK) {
        app_enter_web_netcfg_by_button();
        return;
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* V2.1 has no camera on this key.  Handle the mechanical assembly reset
     * before cloud-state redirection so a disconnected cloud cannot swallow
     * the local safety/recovery operation. */
    if (evt->action == VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK) {
        int ret = service_v2_voice_standby_wake_pose(true);

        if (ret == 0) {
            LISA_LOGI(TAG, "Double click: servo assembly reset queued "
                           "(FL/FR/RL/RR=1500/1500/1615/1315us)");
        } else {
            LISA_LOGE(TAG, "Double click: failed to queue servo assembly reset: %d", ret);
        }
        return;
    }
#endif

#ifdef CONFIG_VOICE_LINK_LINGXIN
    /* Lingxin 模式下，单击按键优先用于语音唤醒，不再被旧 LSChat 云状态页拦截。 */
    should_redirect_cloud_info = app_button_action_should_redirect_cloud_info(evt->action) &&
                                 evt->action != VOICE_MSG_BUTTON_ACTION_CLICK;
#else
    should_redirect_cloud_info = app_button_action_should_redirect_cloud_info(evt->action);
#endif

    uint32_t redirect_status = QR_STATUS_CONNECTED;
    if (should_redirect_cloud_info &&
        app_should_open_info_by_cloud_state(&redirect_status)) {
        // #region debug-point B:button-redirect
        LISA_LOGI(TAG, "button action=%d redirected to cloud info, status=%u", evt->action, (unsigned)redirect_status);
        // #endregion
        LISA_LOGI(TAG, "Button redirects to cloud info, status=%u", (unsigned)redirect_status);
        app_prompt_cloud_info(redirect_status);
        return;
    }

    switch (evt->action) {
        case VOICE_MSG_BUTTON_ACTION_CLICK:
        {
            /* 单击：照片预览时触发拍照；响铃时稍后提醒；非主页先回主页；主页则触发按键唤醒 */
            // 触发拍照
#if CONFIG_LISA_CAMERA_DEVICE
            if (button_camera_preview_handle_click()) {
                break;
            }
#endif
            // 响铃时稍后提醒
            if (alarm_ring_is_active()) {
                LISA_LOGI(TAG, "Single click: alarm ringing, handle snooze");
                alarm_handle_snooze();
                alarm_ring_stop();
                break;
            }
            // 非主页先回主页
#if CONFIG_LISA_DISPLAY_DEVICE
            if (lisa_ui_nav_scr_get_top_id() != 0) {
                LISA_LOGI(TAG, "Single click: not on home page, navigating home");
                lisa_ui_nav_scr_nav_to(0);
                break;
            }
#endif
            // Use the intent captured at physical PRESS, after single-click confirmation.
#ifdef CONFIG_VOICE_LINK_LINGXIN
            if (len == sizeof(service_button_event_t)) {
                (void)app_lingxin_single_click_execute(data);
            }
#else
            LISA_LOGI(TAG, "Single click: wakeup trigger");
            // 如果会话中，退出会话
            if (model_voice_cloud_is_running()) {
                voice_msg_pub(VOICE_MSG_CLOUD_MCP_CHAT_EXIT, NULL, 0);
            } else {
                // 否则触发唤醒
                const char keyword[] = "xiao ling xiao ling";
                voice_msg_pub(VOICE_MSG_WAKEUP_KEYWORD, (void *)keyword, sizeof(keyword));
            }
#endif
            break;
        }
        case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
        {
#if CONFIG_LISA_CAMERA_DEVICE
            button_camera_preview_handle_double_click();
#else
            LISA_LOGI(TAG, "Double click ignored: camera is not populated on this board");
#endif
            break;
        }
        case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
        {
            /* 四击： */
            break;
        }
        case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
        {
            app_ble_netcfg_enter_async();
            break;
        }
        case VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK: 
        {
            /* 连击 >=8：恢复出厂 */
            LISA_LOGI(TAG, "power button repeat click, factory reset");
            factory_reset();
            break;
        }
        case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
        {
            /* 长按：响铃时停止闹铃并生成下一个闹钟；照片预览页则退出预览页；否则关机（USB 供电时忽略） */
            // 退出照片预览
#if CONFIG_LISA_CAMERA_DEVICE
            if (button_camera_preview_handle_long_hold()) {
                break;
            }
#endif
            // 关闭闹铃并生成下一个闹钟
            if (alarm_ring_is_active()) {
                LISA_LOGI(TAG, "Double click: alarm ringing, stop and generate next");
                alarm_ring_stop();
                alarm_handle_stop_and_next();
                break;
            }
            /* 关机：新 boot 下 power_shutdown 自身会按供电方式分流
             * （纯电池真关机；USB 插着走 stage0 假关机）。老 boot 下
             * USB 插着拉低 PWR_LOCK 无效，跳过避免用户以为关了却没关。*/
            if (!uboot_features_has(UBOOT_FEATURE_POWER_GUARD) && power_is_usb_plugged()) {
                LISA_LOGI(TAG, "USB connected on legacy boot, ignore long press shutdown");
            } else {
                LISA_LOGI(TAG, "power button long press hold, shutting down...");
                if (!power_request_shutdown()) {
                    LISA_LOGW(TAG, "shutdown already pending or worker unavailable");
                }
            }
            break;
        }
    }
}

int main(int argc, char **argv)
{
    sys_network_status_t network_status = {0};
    bool wifi_mode = false;

    LOGI("Application version: %s-%s", PROJECT_VERSION_STR, PROJECT_VERSION_COMMIT);

    boot_watchdog_feed();
#if CONFIG_LISA_CAMERA_DEVICE
    button_camera_preview_init();
#endif

    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS, voice_system_network_probe_success, NULL);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    voice_msg_sub(VOICE_MSG_WIFI_IP_GOT, voice_system_network_probe_success, NULL);
#endif
    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL, voice_wifi_provision_guard, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_CONNECTED, voice_cloud_connected, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_CLOUD_AUTH_FAILED, voice_cloud_auth_failed, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_CLOUD_AUTH_SUCCESS , voice_cloud_auth_success, NULL);
    voice_msg_sub(VOICE_MSG_BUTTON_CHANGE, button_changed, NULL);
    voice_msg_sub(VOICE_MSG_WIFI_DISCONNECTED, voice_wifi_provision_guard, NULL);
    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_SWITCH_DONE, voice_wifi_provision_guard, NULL);

    service_led_init();
    service_volume_init();
    voice_lingxin_action_init();
    power_set_shutdown_prepare_callback(app_prepare_shutdown_audio);
    power_set_shutdown_return_callback(app_shutdown_returned);
    service_button_init();
    service_image_init();
#if CONFIG_LISA_DISPLAY_DEVICE
    service_brightness_init();
#endif
#if CONFIG_LISA_CAMERA_DEVICE
    service_camera_init();
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    int v2_sensor_ret = service_v2_sensors_init();
    if (v2_sensor_ret != 0) {
        LISA_LOGE(TAG, "V2.1 sensor initialization failed: %d", v2_sensor_ret);
    }

    if (service_v2_actuators_init() != 0) {
        LISA_LOGE(TAG, "V2.1 actuator initialization failed");
    } else {
        if (service_v2_rgb_effect_init() != 0) {
            LISA_LOGE(TAG, "V2.1 RGB effect initialization failed");
        }
        if (service_v2_gait_init() != 0) {
            LISA_LOGE(TAG, "V2.1 gait initialization failed");
        } else {
            LISA_LOGI(TAG, "V2.1 boot: servo outputs off; first standing pose waits for wakeup");
            if (service_voice_action_init() != 0) {
                LISA_LOGE(TAG, "V2.1 cloud/offline action bridge initialization failed");
            }
        }
    }
#if CONFIG_APP_V2_NFC_PEER_ENABLE
    if (v2_sensor_ret == 0 && service_v2_nfc_peer_init() != 0) {
        LISA_LOGE(TAG, "V2.1 NFC peer discovery initialization failed");
    }
#else
    LISA_LOGI(TAG, "V2.1 NFC peer discovery disabled by build configuration");
#endif
#endif
#ifndef CONFIG_BOARD_ARCS_MINI_V2_1
    motutu_motion_init();       /* motutu1.1: 4 路位置舵机(PA04-07) */
    voice_motutu_control_init();/* 本地 ASR 关键词控狗，命中后吞掉云端兜底 TTS */
    service_motutu_web_init();  /* 局域网 H5 动作编排和 JSON 序列接口 */
    xTaskCreate(motutu_boot_feedback_task, "motutu_boot", 2048, NULL, 3, NULL);
#endif

#if CONFIG_APPLICATION_UI && CONFIG_LISA_DISPLAY_DEVICE
    extern int lisa_ui_init(void);
    lisa_ui_init();
#endif

    battery_init();
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (service_v2_touch_action_init() != 0) {
        LISA_LOGE(TAG, "V2.1 touch action initialization failed");
    }
    if (service_v2_imu_interaction_init() != 0) {
        LISA_LOGE(TAG, "V2.1 IMU interaction initialization failed");
    }
    if (service_v2_voice_standby_init() != 0) {
        LISA_LOGE(TAG, "V2.1 voice standby initialization failed");
    }
    if (service_v2_low_battery_init() != 0) {
        LISA_LOGE(TAG, "V2.1 low-battery interaction initialization failed");
    }
#endif

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    app_netcfg_portal_init();
#ifdef CONFIG_APP_CODEX_STATUS
    if (service_codex_status_init() != 0) {
        LISA_LOGE(TAG, "Codex status initialization failed");
    }
#endif
    if (service_offline_action_init() != 0) {
        LISA_LOGE(TAG, "offline action initialization failed");
    }
#endif

#if CONFIG_APP_STATIC_WIFI_BOOTSTRAP
    if (app_static_wifi_is_configured() && !sys_wifi_has_ap()) {
        s_static_wifi_bootstrap_pending = true;
        if (xTaskCreate(app_static_wifi_bootstrap_task, "wifi_bootstrap", 2048,
                        NULL, 4, NULL) != pdPASS) {
            s_static_wifi_bootstrap_pending = false;
            LISA_LOGE(TAG, "failed to create static WiFi bootstrap task");
        }
    }
#endif

    wifi_mode = (sys_network_get_status(&network_status) == 0) &&
                (network_status.mode == SYS_NETWORK_MODE_WIFI);
    if (wifi_mode && !sys_wifi_has_ap()) {
#if CONFIG_APP_STATIC_WIFI_BOOTSTRAP
        if (s_static_wifi_bootstrap_pending) {
            LISA_LOGI(TAG, "waiting for static WiFi bootstrap");
        } else
#endif
        {
#ifdef CONFIG_OTA
            if (!app_should_defer_wifi_provision_prompt())
#endif
            {
                app_open_cloud_info(QR_STATUS_NOT_CONNECTED);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_NO_CREDENTIAL);
                app_ble_netcfg_adv_start_delayed();
#endif
            }
        }
    }


    #if defined(CONFIG_BOARD_ARCS_MINI_V2_1) && defined(CONFIG_LISA_WDT)
    if (app_v2_system_watchdog_start() != 0) {
        LISA_LOGE(TAG, "system watchdog start failed");
    }
    #endif
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (1) {
#if defined(CONFIG_BOARD_ARCS_MINI_V2_1) && defined(CONFIG_LISA_WDT)
        app_v2_system_watchdog_feed();
#else
        boot_watchdog_feed();
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        app_wifi_netcfg_fallback_poll();
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return 0;
}
