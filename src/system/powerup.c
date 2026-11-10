#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>

#include "sys_init.h"
#include "board.h"
#include "lisa_gpio.h"
#include "pa_manager.h"
#if CONFIG_LISA_DISPLAY_DEVICE
#include "lisa_display.h"
#endif
#include "tone.h"
#include "app_player.h"
#include "voice_player_comm.h"
#include "power/power_manager.h"
#include "uboot_features_api.h"

#define TAG "powerup"
#include "lisa_log.h"

#define SHUTDOWN_TONE_WAIT_MS 4000U

extern void boot_watchdog_feed(void);

#ifdef CONFIG_BOARD_ARCS_MINI_V3
#define CH_4G_DET_POLL_INTERVAL_MS 1000
#define CH_4G_DET_POLL_STACK_SIZE  2048
#define CH_4G_DET_POLL_PRIORITY    (tskIDLE_PRIORITY + 1)
#endif

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static pa_manager_ref_token_t shutdown_pa_ref_token = PA_MANAGER_REF_TOKEN_INVALID;

static void shutdown(void)
{
    LISA_LOGI(TAG, "System shutting down...");

    if (shutdown_pa_ref_token == PA_MANAGER_REF_TOKEN_INVALID) {
        int pa_ret = pa_manager_release_reference(0, &shutdown_pa_ref_token);
        if (pa_ret != 0) {
            LISA_LOGW(TAG, "Failed to release PA reference for shutdown: %d", pa_ret);
        }
    } else {
        LISA_LOGW(TAG, "Shutdown PA reference is already released (token=%u)",
                  (unsigned int)shutdown_pa_ref_token);
    }


    vTaskDelay(pdMS_TO_TICKS(500));
}

static void shutdown_cancelled(void)
{
    if (shutdown_pa_ref_token == PA_MANAGER_REF_TOKEN_INVALID) {
        LISA_LOGI(TAG, "Shutdown cancelled; PA had no released reference");
        return;
    }

    int ret = pa_manager_restore_reference(&shutdown_pa_ref_token);
    if (ret != 0) {
        LISA_LOGE(TAG, "Failed to restore PA after cancelled shutdown: %d", ret);
        return;
    }

    LISA_LOGI(TAG, "Shutdown cancelled; PA reference reconciled");
}

#else
static void shutdown(void)
{
    LISA_LOGI(TAG, "System shutting down...");

    if (tone_player != NULL) {
        const char *tone_url = app_tone_get_url(TONE_ID_25);
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SHUTDOWN_TONE_WAIT_MS);

        if (tone_url != NULL) {
            int play_ret = app_player_play(tone_player, tone_url);
            LISA_LOGI(TAG, "shutdown tone request: tone_player=%p url=%s ret=%d",
                      tone_player, tone_url, play_ret);
            if (play_ret == APP_PLAYER_OK) {
                bool playback_started = false;
                while (xTaskGetTickCount() < deadline) {
                    app_player_state_t state = app_player_get_state(tone_player);
                    bool active = state == APP_PLAYER_STATE_PREPARING ||
                                  state == APP_PLAYER_STATE_PREPARED ||
                                  state == APP_PLAYER_STATE_PLAYING ||
                                  state == APP_PLAYER_STATE_PAUSED;

                    if (active) {
                        playback_started = true;
                    } else if (playback_started) {
                        break;
                    }

                    boot_watchdog_feed();
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        } else {
            LISA_LOGW(TAG, "shutdown tone request skipped: tone url is null");
        }
    } else {
        LISA_LOGW(TAG, "shutdown tone request skipped: tone_player is null");
    }

    pa_manager_control(0, 0);

#if CONFIG_LISA_DISPLAY_DEVICE
    lisa_device_t *disp = lisa_device_get("display");
    lisa_display_blanking_on(disp);
    lisa_display_set_brightness(disp, 0);
#endif

    vTaskDelay(pdMS_TO_TICKS(200));
}

#endif

#ifdef CONFIG_BOARD_ARCS_MINI_V3
static void powerup_force_camera_pwdn_low(void)
{
    struct lisa_device *pwdn_dev = lisa_device_get(CAM_PWDN_DEVICE_NAME);
    if (!pwdn_dev || !lisa_device_ready(pwdn_dev)) {
        LISA_LOGW(TAG, "Camera PWDN device %s not ready", CAM_PWDN_DEVICE_NAME);
        return;
    }

    int ret = lisa_gpio_configure(pwdn_dev, CAM_PWDN_PIN, LISA_GPIO_CONFIG_OUTPUT_LOW);
    if (ret != 0) {
        LISA_LOGW(TAG, "Failed to force camera PWDN low: pin=%d ret=%d", CAM_PWDN_PIN, ret);
        return;
    }

    LISA_LOGI(TAG, "Camera PWDN forced low at startup");
}

static void powerup_init_charge_en(void)
{
    struct lisa_device *charge_en_dev = lisa_device_get(CHARGE_EN_DEVICE_NAME);
    if (!charge_en_dev || !lisa_device_ready(charge_en_dev)) {
        LISA_LOGW(TAG, "Charge EN device %s not ready", CHARGE_EN_DEVICE_NAME);
        return;
    }

    int ret = lisa_gpio_configure(charge_en_dev, CHARGE_EN_PIN, LISA_GPIO_INPUT);
    if (ret != 0) {
        LISA_LOGW(TAG, "Failed to init charge EN: pin=%d ret=%d", CHARGE_EN_PIN, ret);
        return;
    }

    LISA_LOGI(TAG, "Charge EN initialized as floating input");
}

static void powerup_log_4g_det_state(void)
{
    struct lisa_device *det_dev = lisa_device_get(CH_4G_DET_DEVICE_NAME);
    if (!det_dev || !lisa_device_ready(det_dev)) {
        LISA_LOGW(TAG, "4G DET device %s not ready", CH_4G_DET_DEVICE_NAME);
        return;
    }

    int ret = lisa_gpio_configure(det_dev, CH_4G_DET_PIN, LISA_GPIO_INPUT);
    if (ret != 0) {
        LISA_LOGW(TAG, "Failed to configure 4G DET: device=%s pin=%d ret=%d",
                  CH_4G_DET_DEVICE_NAME, CH_4G_DET_PIN, ret);
        return;
    }

    int level = lisa_gpio_read_pin(det_dev, CH_4G_DET_PIN);
    if (level < 0) {
        LISA_LOGW(TAG, "Failed to read 4G DET: device=%s pin=%d ret=%d",
                  CH_4G_DET_DEVICE_NAME, CH_4G_DET_PIN, level);
        return;
    }

    bool active = level == (CH_4G_DET_ACTIVE_LEVEL ? LISA_GPIO_HIGH : LISA_GPIO_LOW);
    LISA_LOGI(TAG, "4G DET state: device=%s pin=%d level=%d active_level=%d active=%d",
              CH_4G_DET_DEVICE_NAME, CH_4G_DET_PIN, level, CH_4G_DET_ACTIVE_LEVEL, active);
}

static void powerup_4g_det_poll_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CH_4G_DET_POLL_INTERVAL_MS));
        powerup_log_4g_det_state();
    }
}

static void powerup_start_4g_det_poll(void)
{
    BaseType_t ret = xTaskCreate(powerup_4g_det_poll_task, "4g_det_poll",
                                 CH_4G_DET_POLL_STACK_SIZE, NULL,
                                 CH_4G_DET_POLL_PRIORITY, NULL);
    if (ret != pdPASS) {
        LISA_LOGW(TAG, "Failed to create 4G DET poll task: ret=%d", ret);
    }
}
#endif

static int power_up_guard(void)
{
    power_config_t power_cfg = {
        .on_shutdown = shutdown,
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        .on_shutdown_cancelled = shutdown_cancelled,
#endif
    };
    power_init(&power_cfg);

#ifdef CONFIG_BOARD_ARCS_MINI_V3
    powerup_force_camera_pwdn_low();
    powerup_init_charge_en();
    powerup_log_4g_det_state();
    powerup_start_4g_det_poll();
#endif

    /* 新 boot 已在 stage0 做长按守护 + 驱动指示 LED，app 只在老 boot 下兜底 */
    if (!uboot_features_has(UBOOT_FEATURE_POWER_GUARD)) {
        if (!power_wait_settle()) {
            power_shutdown();
            return 0;
        }

        // 点亮LED，表示系统已上电
#ifndef CONFIG_BOARD_ARCS_MINI_V3
        struct lisa_device *led_dev = lisa_device_get("gpiob");
        lisa_gpio_configure(led_dev, LED_PIN, LISA_GPIO_OUTPUT | LISA_GPIO_OUTPUT_INIT_LOW);
#endif
    }

    return 0;
}

SYS_INIT(power_up_guard, SYS_INIT_LEVEL_PRE_APPLICATION, 0);
