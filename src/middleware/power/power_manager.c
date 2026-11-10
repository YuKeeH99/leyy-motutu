/**
 * @file power_manager.c
 * @brief Power management functions for system boot and shutdown
 * @copyright Copyright (C) 2025 ANHUI LISTENAI Co., Ltd. All Rights Reserved.
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "power_manager.h"
#include "lisa_time.h"
#include "lisa_log.h"
#include "lisa_gpio.h"
#include "board.h"
#include "chip.h"           /* IP_AON_IOMUX */
#include "PowerManager.h"   /* __HAL_PMU_WholeChip_RST_ENABLE */
#include "uboot_features_api.h"
#include "uboot_power_api.h"
#include "sys/reboot.h"

#define TAG "power_mgr"

#define POWER_KEY_PAD CONFIG_POWER_MANAGER_POWER_KEY_GPIO_PAD
#define POWER_EN_PAD  CONFIG_POWER_MANAGER_POWER_EN_GPIO_PAD
#define USB_DET_PAD   CONFIG_POWER_MANAGER_USB_DETECT_GPIO_PAD

#define POWER_SHUTDOWN_WORKER_STACK_SIZE 1024U
#define POWER_SHUTDOWN_WORKER_PRIORITY   (tskIDLE_PRIORITY + 3)

static lisa_device_t *power_key_dev = NULL;
static lisa_device_t *power_en_dev = NULL;
static lisa_device_t *usb_det_dev = NULL;

static void (*shutdown_cb)(void) = NULL;
static void (*shutdown_cancelled_cb)(void) = NULL;
static void (*shutdown_prepare_cb)(void) = NULL;
static void (*shutdown_return_cb)(void) = NULL;
static TaskHandle_t shutdown_worker_task = NULL;
static bool shutdown_request_pending = false;
static bool shutdown_idle_only = false;
static bool shutdown_in_progress = false;
#if configSUPPORT_STATIC_ALLOCATION
static StackType_t shutdown_worker_stack[POWER_SHUTDOWN_WORKER_STACK_SIZE];
static StaticTask_t shutdown_worker_tcb;
#endif

static void power_gpio_init(void)
{
    power_key_dev = lisa_device_get(POWER_KEY_PAD);
    lisa_gpio_configure(power_key_dev, POWER_KEY_PIN, LISA_GPIO_CONFIG_INPUT_PULLUP);

    /* 上一轮可能走了 power_reboot_soft 把 AON force-output 打开，
     * 清掉，否则 GPIO 写入不生效，后续 power_shutdown / stage0 latch_set
     * 都会失灵。*/
    volatile uint32_t *aon_iomux =
        (volatile uint32_t *)&IP_AON_IOMUX->REG_PAD_AON_GPIOB_00.all + POWER_EN_PIN;
    *aon_iomux &= ~(0x1E00000u);

    /* 新 boot 已拉起 PWR_LOCK，OUTPUT_HIGH 接管避免掉电窗口 */
    power_en_dev = lisa_device_get(POWER_EN_PAD);
    lisa_gpio_flags_t en_cfg = uboot_features_has(UBOOT_FEATURE_POWER_GUARD)
                                   ? LISA_GPIO_CONFIG_OUTPUT_HIGH
                                   : LISA_GPIO_CONFIG_OUTPUT_LOW;
    lisa_gpio_configure(power_en_dev, POWER_EN_PIN, en_cfg);

    usb_det_dev = lisa_device_get(USB_DET_PAD);
    lisa_gpio_configure(usb_det_dev, USB_DET_PIN, LISA_GPIO_CONFIG_INPUT_PULLDOWN);

    LISA_LOGI(TAG, "Power GPIO initialized - Button: %s(%d), Latch: %s(%d), USB Detect: %s(%d)", POWER_KEY_PAD,
              POWER_KEY_PIN, POWER_EN_PAD, POWER_EN_PIN, USB_DET_PAD, USB_DET_PIN);
}

static bool power_button_pressed(void)
{
    return lisa_gpio_read_pin(power_key_dev, POWER_KEY_PIN) == LISA_GPIO_LOW;
}

static int power_latch_set(bool state)
{
    return lisa_gpio_write_pin(power_en_dev, POWER_EN_PIN, state ? LISA_GPIO_HIGH : LISA_GPIO_LOW);
}

static bool power_shutdown_gate_try_enter(void)
{
    bool entered = false;

    taskENTER_CRITICAL();
    if (!shutdown_in_progress) {
        shutdown_in_progress = true;
        entered = true;
    }
    taskEXIT_CRITICAL();

    return entered;
}

static void power_shutdown_gate_leave(void)
{
    taskENTER_CRITICAL();
    shutdown_in_progress = false;
    taskEXIT_CRITICAL();
}

static void power_shutdown_worker(void *arg)
{
    (void)arg;

    while (1) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool began_as_idle = shutdown_idle_only;
        if (!shutdown_idle_only || !power_is_usb_plugged()) {
            if (shutdown_prepare_cb) {
                shutdown_prepare_cb();
            }
            if (!shutdown_idle_only || !power_is_usb_plugged()) {
                power_shutdown();
            }
        }

        taskENTER_CRITICAL();
        /* An explicit request may arrive just after a USB cancellation check.
         * Keep that upgrade pending for another pass if power returned. */
        bool retry_explicit = began_as_idle && !shutdown_idle_only;
        if (!retry_explicit) {
            shutdown_request_pending = false;
            shutdown_idle_only = false;
        }
        taskEXIT_CRITICAL();
        if (shutdown_return_cb) {
            shutdown_return_cb();
        }
        if (retry_explicit) {
            xTaskNotifyGive(shutdown_worker_task);
        }
    }
}

static void power_shutdown_worker_init(void)
{
    if (shutdown_worker_task) {
        return;
    }

#if configSUPPORT_STATIC_ALLOCATION
    shutdown_worker_task = xTaskCreateStatic(power_shutdown_worker, "power.off",
                                              POWER_SHUTDOWN_WORKER_STACK_SIZE, NULL,
                                              POWER_SHUTDOWN_WORKER_PRIORITY,
                                              shutdown_worker_stack, &shutdown_worker_tcb);
#else
    BaseType_t ret = xTaskCreate(power_shutdown_worker, "power.off",
                                 POWER_SHUTDOWN_WORKER_STACK_SIZE, NULL,
                                 POWER_SHUTDOWN_WORKER_PRIORITY, &shutdown_worker_task);
    if (ret != pdPASS) {
        shutdown_worker_task = NULL;
    }
#endif

    if (!shutdown_worker_task) {
        LISA_LOGE(TAG, "Failed to create shutdown worker");
    }
}

void power_init(const power_config_t *config)
{
    if (config) {
        shutdown_cb = config->on_shutdown;
        shutdown_cancelled_cb = config->on_shutdown_cancelled;
    }

    power_gpio_init();
    power_shutdown_worker_init();
}

bool power_wait_settle(void)
{
    if (power_is_usb_plugged()) {
        power_latch_set(true);
        LISA_LOGI(TAG, "USB connected, power on directly");
        return true;
    }

    while (1) {
        uint64_t elapsed = lisa_os_get_tick_ms();

        if (!power_button_pressed()) {
            LISA_LOGI(TAG, "Power button released at %llu ms before settle time", elapsed);
            return false;
        }

        if (elapsed >= POWER_BUTTON_HOLD_TIME_MS) {
            LISA_LOGI(TAG, "Power button held for required settle time: %llu ms", elapsed);
            break;
        }

        lisa_thread_mdelay(POWER_SAMPLE_INTERVAL_MS);
    }

    power_latch_set(true);
    LISA_LOGI(TAG, "Latch set, system powered on");

    return true;
}

bool power_is_usb_plugged(void)
{
    return lisa_gpio_read_pin(usb_det_dev, USB_DET_PIN) == LISA_GPIO_HIGH;
}

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static bool power_restore_cancelled_shutdown(const char *phase)
{
    LISA_LOGI(TAG, "Restore cancelled shutdown %s on V2.1", phase);

    int ret = power_latch_set(true);
    if (ret != 0) {
        LISA_LOGW(TAG, "PWR_LOCK write-high failed (%d), retry by configuring output-high", ret);
        ret = lisa_gpio_configure(power_en_dev, POWER_EN_PIN, LISA_GPIO_CONFIG_OUTPUT_HIGH);
        if (ret != 0) {
            LISA_LOGE(TAG, "Failed to restore PWR_LOCK before shutdown cancellation: %d", ret);
            return false;
        }
    }

    if (shutdown_cancelled_cb) {
        shutdown_cancelled_cb();
    }

    return true;
}
#endif

void power_shutdown(void)
{
    if (!power_shutdown_gate_try_enter()) {
        LISA_LOGW(TAG, "Shutdown already in progress; ignore duplicate request");
        return;
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* Type-C keeps VCC alive, so a new boot with POWER_GUARD performs a
     * deliberate stage0 soft shutdown. Legacy boot cannot provide that state
     * and must keep the application alive. */
    if (power_is_usb_plugged() &&
        (shutdown_idle_only || !uboot_features_has(UBOOT_FEATURE_POWER_GUARD))) {
        LISA_LOGI(TAG, "USB connected: ignore idle or unsupported legacy shutdown");
        /* This also retries a previously failed hot-plug recovery.  The PA
         * callback is token-based, so invoking it without an outstanding
         * shutdown release is a safe no-op. */
        (void)power_restore_cancelled_shutdown("before shutdown");
        power_shutdown_gate_leave();
        return;
    }
#endif

    LISA_LOGI(TAG, "Executing system shutdown...");

    if (shutdown_cb) {
        LISA_LOGI(TAG, "Calling shutdown callback...");
        shutdown_cb();
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* shutdown_cb may wait while audio and other peripherals settle. Handle a
     * Type-C hot-plug using the same new-boot soft-shutdown path. */
    if (power_is_usb_plugged()) {
        if (!shutdown_idle_only && uboot_features_has(UBOOT_FEATURE_POWER_GUARD)) {
            LISA_LOGI(TAG, "USB plugged, V2.1 soft shutdown via stage0");
            uboot_shutdown_request();
        } else {
            (void)power_restore_cancelled_shutdown("after USB hot-plug during callback");
        }
        power_shutdown_gate_leave();
        return;
    }
#else
    /* USB 插着时拉低 PWR_LOCK 无效（VBUS 维持 VCC），新 boot 走 stage0
     * 假关机。老 boot 回退到直接拉低，USB 下不生效。*/
    if (power_is_usb_plugged() && uboot_features_has(UBOOT_FEATURE_POWER_GUARD)) {
        LISA_LOGI(TAG, "USB plugged, soft shutdown via stage0");
        uboot_shutdown_request();
    }
#endif

    LISA_LOGI(TAG, "Goodbye!");
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    int latch_ret = power_latch_set(false);
    if (latch_ret != 0) {
        LISA_LOGE(TAG, "Failed to release PWR_LOCK: %d", latch_ret);
        (void)power_restore_cancelled_shutdown("after PWR_LOCK release failure");
        power_shutdown_gate_leave();
        return;
    }
#else
    power_latch_set(false);
#endif

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* On battery the CPU loses power and never reaches this check.  Reaching
     * it with USB present means Type-C landed in the narrow interval between
     * the final check and releasing PWR_LOCK; restore the cancelled shutdown
     * before handing control back to main. */
    if (power_is_usb_plugged()) {
        (void)power_restore_cancelled_shutdown("after USB hot-plug while releasing PWR_LOCK");
        power_shutdown_gate_leave();
        return;
    }
#endif

    /* Normally battery power disappears before this point.  If the GPIO write
     * returned while the CPU is still executing, release only the software
     * serialization gate so a later request can retry; shutdown callbacks are
     * still protected from concurrent execution. */
    power_shutdown_gate_leave();
}

static bool power_queue_shutdown(bool idle_only)
{
    TaskHandle_t worker = NULL;
    bool upgraded = false;

    taskENTER_CRITICAL();
    if (shutdown_worker_task && shutdown_request_pending &&
        shutdown_idle_only && !idle_only) {
        /* A button request must not disappear when USB cancels an idle
         * shutdown already waiting in the worker. Reuse its notification. */
        shutdown_idle_only = false;
        upgraded = true;
    } else if (shutdown_worker_task && !shutdown_request_pending &&
        (!idle_only || !power_is_usb_plugged())) {
        shutdown_request_pending = true;
        shutdown_idle_only = idle_only;
        worker = shutdown_worker_task;
    }
    taskEXIT_CRITICAL();

    if (!worker) {
        return upgraded;
    }

    xTaskNotifyGive(worker);
    return true;
}

bool power_request_shutdown(void)
{
    return power_queue_shutdown(false);
}

bool power_request_idle_shutdown(void)
{
    return power_queue_shutdown(true);
}

void power_set_shutdown_prepare_callback(void (*callback)(void))
{
    taskENTER_CRITICAL();
    shutdown_prepare_cb = callback;
    taskEXIT_CRITICAL();
}

void power_set_shutdown_return_callback(void (*callback)(void))
{
    taskENTER_CRITICAL();
    shutdown_return_cb = callback;
    taskEXIT_CRITICAL();
}

void power_reboot_soft(void)
{
    sys_arch_reboot(SYS_REBOOT_SOFT);

    while (1) {
        __asm__ volatile("wfi");
    }
}
