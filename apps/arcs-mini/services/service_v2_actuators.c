#include "service_v2_actuators.h"
#include "service_v2_rgb_layout.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "ClockManager.h"
#include "Driver_GPT_PWM.h"
#include "FreeRTOS.h"
#include "IOMuxManager.h"
#include "arcs_ap.h"
#include "core_feature_base.h"
#include "lisa_device.h"
#include "lisa_gpio.h"
#include "lisa_log.h"
#include "lisa_mutex.h"
#include "lisa_thread.h"
#include "lisa_typedef.h"
#include "power/power_manager.h"
#include "kv_user.h"
#include "systick.h"
#include "task.h"

#define TAG "v2.actuators"

#define ACTUATOR_GPIO_DEVICE  "gpioa"
#define ACTUATOR_PWM_DEVICE   "pwm0"
#define ACTUATOR_PAD          CSK_IOMUX_PAD_A
#define SERVO_GPIO_FUNCTION   CSK_IOMUX_FUNC_DEFAULT
#define RGB_GPIO_FUNCTION     CSK_IOMUX_FUNC_ALTER1
#define ACTUATOR_PWM_FUNCTION CSK_IOMUX_FUNC_ALTER12

#define WS2812_PIN               8U
#define WS2812_PIN_MASK          (1UL << WS2812_PIN)
#define WS2812_SCOPE_STOP_MS     50U
#define WS2812_T0H_NS            300U
#define WS2812_T1H_NS            650U
/* Schedule rising edges against one accumulated MCYCLE timeline.  A relative
 * per-bit wait made the loop tail add about 280 ns to every wire period. */
#define WS2812_BIT_PERIOD_NS     1280U
#define WS2812_RESET_US          300U
#define WS2812_CALIBRATION_US    100U
#define WS2812_MIN_CORE_CLOCK_HZ 80000000U
#define WS2812_SELF_TEST_LEVEL   50U
#define WS2812_SELF_TEST_HOLD_MS 120U
#define WS2812_SCOPE_TRAIN_BITS  16384U
#define WS2812_SCOPE_TASK_STACK  2048U
#define WS2812_SCOPE_YIELD_MS    1U
/* Product-wide optical limit.  Keep logical colours/status unchanged and
 * apply the cap only once at the physical WS2812 encoding boundary. */
#define RGB_OUTPUT_SCALE_PERCENT 50U

#define RGB_CALIBRATION_RECORD_MAGIC 0x314C4143UL /* "CAL1" in little endian. */

#define SERVO_SWEEP_TASK_STACK 2048U
#define SERVO_FRAME_TASK_STACK 2048U
/* One 50 Hz servo frame, so every step lands on a fresh shadow load. */
#define SERVO_SWEEP_STEP_MS    SERVICE_V2_SERVO_FRAME_STEP_MS
#define SERVO_SWEEP_STOP_MS    600U
#define SERVO_EASING_SCALE     1000000U

typedef struct {
    uint8_t pin;
    GPT_CHANNEL_TYPE channel;
    GPT_CHANNEL_SYNC_TYPE sync;
    uint16_t pulse_us;
    bool enabled;
} servo_state_t;

typedef struct {
    uint16_t start_us[SERVICE_V2_SERVO_COUNT];
    uint16_t target_us[SERVICE_V2_SERVO_COUNT];
    uint32_t sequence;
    uint32_t duration_ms;
    uint32_t elapsed_ms;
    uint32_t watchdog_ms;
    uint32_t watchdog_remaining_ms;
    service_v2_servo_easing_t easing;
    bool active;
    bool watchdog_expired;
} servo_frame_state_t;

/* Keep the persisted representation independent from compiler enum size and
 * initialize every reserved byte before checksumming it. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t order;
    uint8_t reserved[3];
    uint16_t red_gain_permille;
    uint16_t green_gain_permille;
    uint16_t blue_gain_permille;
    uint16_t gamma_x100;
    uint32_t checksum;
} rgb_calibration_record_t;

_Static_assert(sizeof(rgb_calibration_record_t) == 24U, "RGB calibration KV record layout changed");

/* Keep the pin/channel relationship explicit; it comes from the V2 schematic. */
static servo_state_t s_servos[SERVICE_V2_SERVO_COUNT] = {
    {4U, GPT_CHANNEL4, GPT_CHANNEL4_SYNC, SERVICE_V2_SERVO_CENTER_PULSE_US, false},
    {5U, GPT_CHANNEL5, GPT_CHANNEL5_SYNC, SERVICE_V2_SERVO_CENTER_PULSE_US, false},
    {6U, GPT_CHANNEL6, GPT_CHANNEL6_SYNC, SERVICE_V2_SERVO_CENTER_PULSE_US, false},
    {7U, GPT_CHANNEL7, GPT_CHANNEL7_SYNC, SERVICE_V2_SERVO_CENTER_PULSE_US, false},
};

static const uint32_t s_gpt_divider_controls[] = {
    CSK_GPT_PWM_CLKDIV_1,  CSK_GPT_PWM_CLKDIV_2,  CSK_GPT_PWM_CLKDIV_4,  CSK_GPT_PWM_CLKDIV_8,
    CSK_GPT_PWM_CLKDIV_16, CSK_GPT_PWM_CLKDIV_32, CSK_GPT_PWM_CLKDIV_64, CSK_GPT_PWM_CLKDIV_128,
};

static lisa_device_t *s_gpioa;
static lisa_device_t *s_pwm0;
static lisa_mutex_t *s_lock;
static void *s_gpt;
static uint32_t s_servo_pclk_hz;
static uint32_t s_servo_divisor;
static uint32_t s_servo_divider_control;
static uint16_t s_servo_period_counts;
static uint32_t s_ws2812_cycle_hz;
static volatile bool s_ws2812_scope_running;
static volatile bool s_ws2812_scope_task_alive;
static bool s_ws2812_scope_one;
static volatile bool s_servo_sweep_running;
static volatile bool s_servo_sweep_task_alive;
static uint8_t s_servo_sweep_channel;
static uint16_t s_servo_sweep_period_ms;
static servo_frame_state_t s_servo_frame;
static bool s_usb_idle_released;
static service_v2_rgb_status_t s_rgb_status = {
    .count = SERVICE_V2_RGB_STRIP_DEFAULT_COUNT,
    .brightness_percent = SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT,
    .owner = SERVICE_V2_RGB_OWNER_SYSTEM,
};
static service_v2_rgb_calibration_t s_rgb_calibration = {
    .version = SERVICE_V2_RGB_CALIBRATION_VERSION,
    .order = SERVICE_V2_RGB_ORDER_GRB,
    .red_gain_permille = SERVICE_V2_RGB_GAIN_MAX_PERMILLE,
    .green_gain_permille = SERVICE_V2_RGB_GAIN_MAX_PERMILLE,
    .blue_gain_permille = SERVICE_V2_RGB_GAIN_MAX_PERMILLE,
    .gamma_x100 = SERVICE_V2_RGB_GAMMA_MIN_X100,
};
static uint8_t s_rgb_gamma_lut[UINT8_MAX + 1U];
static bool s_rgb_calibration_persistent;
static bool s_ready;

static int rgb_strip_write_locked(uint8_t count, uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness_percent,
                                  service_v2_rgb_owner_t owner);
static int rgb_release_locked(void);

static const service_v2_rgb_calibration_t s_rgb_calibration_default = {
    .version = SERVICE_V2_RGB_CALIBRATION_VERSION,
    .order = SERVICE_V2_RGB_ORDER_GRB,
    .red_gain_permille = SERVICE_V2_RGB_GAIN_MAX_PERMILLE,
    .green_gain_permille = SERVICE_V2_RGB_GAIN_MAX_PERMILLE,
    .blue_gain_permille = SERVICE_V2_RGB_GAIN_MAX_PERMILLE,
    .gamma_x100 = SERVICE_V2_RGB_GAMMA_MIN_X100,
};

static bool rgb_calibration_valid(const service_v2_rgb_calibration_t *calibration)
{
    return calibration != NULL && calibration->version == SERVICE_V2_RGB_CALIBRATION_VERSION &&
           calibration->order >= SERVICE_V2_RGB_ORDER_RGB && calibration->order <= SERVICE_V2_RGB_ORDER_BGR &&
           calibration->red_gain_permille <= SERVICE_V2_RGB_GAIN_MAX_PERMILLE &&
           calibration->green_gain_permille <= SERVICE_V2_RGB_GAIN_MAX_PERMILLE &&
           calibration->blue_gain_permille <= SERVICE_V2_RGB_GAIN_MAX_PERMILLE &&
           calibration->gamma_x100 >= SERVICE_V2_RGB_GAMMA_MIN_X100 &&
           calibration->gamma_x100 <= SERVICE_V2_RGB_GAMMA_MAX_X100;
}

static uint32_t rgb_calibration_record_checksum(const rgb_calibration_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t checksum = 2166136261UL;

    for (size_t i = 0U; i < offsetof(rgb_calibration_record_t, checksum); ++i) {
        checksum ^= bytes[i];
        checksum *= 16777619UL;
    }
    return checksum;
}

static void rgb_calibration_make_record(const service_v2_rgb_calibration_t *calibration,
                                        rgb_calibration_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = RGB_CALIBRATION_RECORD_MAGIC;
    record->version = SERVICE_V2_RGB_CALIBRATION_VERSION;
    record->size = (uint16_t)sizeof(*record);
    record->order = (uint8_t)calibration->order;
    record->red_gain_permille = calibration->red_gain_permille;
    record->green_gain_permille = calibration->green_gain_permille;
    record->blue_gain_permille = calibration->blue_gain_permille;
    record->gamma_x100 = calibration->gamma_x100;
    record->checksum = rgb_calibration_record_checksum(record);
}

static int rgb_calibration_store(const service_v2_rgb_calibration_t *calibration)
{
    rgb_calibration_record_t record;
    rgb_calibration_make_record(calibration, &record);
    return lisa_kv_set_blob(KV_KEY_USER_V2_RGB_CALIBRATION, (uint8_t *)&record, (int)sizeof(record)) == 0 ? 0 : -EIO;
}

static bool rgb_calibration_record_valid(const rgb_calibration_record_t *record)
{
    if (record == NULL || record->magic != RGB_CALIBRATION_RECORD_MAGIC ||
        record->version != SERVICE_V2_RGB_CALIBRATION_VERSION || record->size != sizeof(*record) ||
        record->checksum != rgb_calibration_record_checksum(record)) {
        return false;
    }

    service_v2_rgb_calibration_t calibration = {
        .version = record->version,
        .order = (service_v2_rgb_order_t)record->order,
        .red_gain_permille = record->red_gain_permille,
        .green_gain_permille = record->green_gain_permille,
        .blue_gain_permille = record->blue_gain_permille,
        .gamma_x100 = record->gamma_x100,
    };
    return rgb_calibration_valid(&calibration);
}

static void rgb_calibration_build_lut(uint16_t gamma_x100, uint8_t lut[UINT8_MAX + 1U])
{
    if (gamma_x100 == SERVICE_V2_RGB_GAMMA_MIN_X100) {
        for (uint32_t value = 0U; value <= UINT8_MAX; ++value) {
            lut[value] = (uint8_t)value;
        }
        return;
    }

    const double gamma = (double)gamma_x100 / 100.0;
    lut[0] = 0U;
    lut[UINT8_MAX] = UINT8_MAX;
    for (uint32_t value = 1U; value < UINT8_MAX; ++value) {
        double corrected = pow((double)value / (double)UINT8_MAX, gamma) * (double)UINT8_MAX;
        uint32_t rounded = (uint32_t)(corrected + 0.5);
        lut[value] = (uint8_t)(rounded > UINT8_MAX ? UINT8_MAX : rounded);
    }
}

static void rgb_calibration_load(void)
{
    uint8_t *blob = NULL;
    int blob_len = 0;
    bool valid = false;
    service_v2_rgb_calibration_t calibration = s_rgb_calibration_default;

    if (lisa_kv_get_blob(KV_KEY_USER_V2_RGB_CALIBRATION, &blob, &blob_len) == 0 && blob != NULL &&
        blob_len == (int)sizeof(rgb_calibration_record_t)) {
        const rgb_calibration_record_t *record = (const rgb_calibration_record_t *)blob;
        if (rgb_calibration_record_valid(record)) {
            calibration.version = record->version;
            calibration.order = (service_v2_rgb_order_t)record->order;
            calibration.red_gain_permille = record->red_gain_permille;
            calibration.green_gain_permille = record->green_gain_permille;
            calibration.blue_gain_permille = record->blue_gain_permille;
            calibration.gamma_x100 = record->gamma_x100;
            valid = true;
        }
    }
    if (blob != NULL) {
        lisa_kv_free(blob);
    }

    if (!valid) {
        /* A missing, truncated, unknown-version or checksum-damaged record is
         * never partially applied. Repair it with the safe legacy defaults. */
        s_rgb_calibration_persistent = rgb_calibration_store(&calibration) == 0;
        LISA_LOGW(TAG, "RGB calibration KV missing/corrupt; defaults restored (persistent=%u)",
                  s_rgb_calibration_persistent ? 1U : 0U);
    } else {
        s_rgb_calibration_persistent = true;
    }

    s_rgb_calibration = calibration;
    rgb_calibration_build_lut(calibration.gamma_x100, s_rgb_gamma_lut);
}

static int actuator_pin_gpio_low(uint8_t pin)
{
    uint32_t gpio_function = pin == WS2812_PIN ? RGB_GPIO_FUNCTION : SERVO_GPIO_FUNCTION;
    int ret = lisa_gpio_configure(s_gpioa, pin, LISA_GPIO_CONFIG_OUTPUT_LOW);
    if (ret != 0) {
        return ret;
    }

    /* Configure the output value before exposing the GPIO function on the pad. */
    IP_GPIOA->REG_DOUTCLEAR.all = 1UL << pin;
    ret = IOMuxManager_PinConfigure(ACTUATOR_PAD, pin, gpio_function);
    if (ret != CSK_DRIVER_OK) {
        return ret;
    }

    IP_GPIOA->REG_DOUTCLEAR.all = 1UL << pin;
    return 0;
}

static int servo_clock_select(void)
{
    uint32_t pclk = CRM_GetCmn_peri_pclkFreq();
    if (pclk == 0U) {
        pclk = CRM_GetSrcFreq(CRM_IpSrcPeriClk);
    }
    if (pclk == 0U) {
        return -EIO;
    }

    for (uint32_t shift = 0; shift < (sizeof(s_gpt_divider_controls) / sizeof(s_gpt_divider_controls[0])); shift++) {
        uint32_t divisor = 1UL << shift;
        uint64_t denominator = (uint64_t)SERVICE_V2_SERVO_FREQUENCY_HZ * divisor;
        uint64_t counts = ((uint64_t)pclk + denominator / 2U) / denominator;

        if (counts >= 2U && counts <= UINT16_MAX) {
            s_servo_pclk_hz = pclk;
            s_servo_divisor = divisor;
            s_servo_divider_control = s_gpt_divider_controls[shift];
            s_servo_period_counts = (uint16_t)counts;
            return 0;
        }
    }

    return -ERANGE;
}

static int servo_reload_values(uint16_t pulse_us, uint16_t *reload_high, uint16_t *reload_low)
{
    uint64_t denominator = (uint64_t)s_servo_divisor * 1000000U;
    uint64_t high_counts = ((uint64_t)s_servo_pclk_hz * pulse_us + denominator / 2U) / denominator;

    if (high_counts < 2U || high_counts >= s_servo_period_counts) {
        return -ERANGE;
    }

    uint32_t low_counts = (uint32_t)s_servo_period_counts - (uint32_t)high_counts;
    if (low_counts < 2U) {
        return -ERANGE;
    }

    /* GPT reload fields encode interval length minus one, like the HAL's
     * frequency/duty implementation. Keep both intervals non-zero. */
    *reload_high = (uint16_t)(high_counts - 1U);
    *reload_low = (uint16_t)(low_counts - 1U);
    return 0;
}

static uint8_t servo_enabled_mask_locked(void)
{
    uint8_t mask = 0U;
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if (s_servos[i].enabled) {
            mask |= (uint8_t)(1U << i);
        }
    }
    return mask;
}

static int servo_apply_mask_locked(uint8_t mask)
{
    uint32_t sync_mask = 0U;

    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if ((mask & (1U << i)) == 0U) {
            continue;
        }

        uint16_t reload_high;
        uint16_t reload_low;
        int ret = servo_reload_values(s_servos[i].pulse_us, &reload_high, &reload_low);
        if (ret != 0) {
            return ret;
        }
        ret = HAL_GPT_SetPWMFreqDutyByCounter_NoShadow(s_gpt, s_servos[i].channel, reload_high, reload_low);
        if (ret != CSK_DRIVER_OK) {
            return -EIO;
        }
        sync_mask |= (uint32_t)s_servos[i].sync;
    }

    if (sync_mask == 0U) {
        return 0;
    }

    int ret = HAL_GPT_ShadowLoad(s_gpt, (GPT_CHANNEL_SYNC_TYPE)sync_mask);
    return ret == CSK_DRIVER_OK ? 0 : -EIO;
}

static int servo_enable_mask_locked(uint8_t mask)
{
    uint8_t newly_enabled = 0U;
    uint32_t control = CSK_GPT_PWM_MODE | CSK_GPT_PWM_CLKSRC_PCLK | CSK_GPT_PWM_OUTMODE_EDGE_ALIGNED |
                       CSK_GPT_PWM_OUTPOLARITY_HIGH | s_servo_divider_control | CSK_GPT_PWM_OPERATION_MODE_PWM;

    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if ((mask & (1U << i)) == 0U || s_servos[i].enabled) {
            continue;
        }
        if (HAL_GPT_PWMControl(s_gpt, control, s_servos[i].channel) != CSK_DRIVER_OK) {
            return -EIO;
        }
        newly_enabled |= (uint8_t)(1U << i);
    }

    int ret = servo_apply_mask_locked(mask);
    if (ret != 0) {
        return ret;
    }

    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if ((newly_enabled & (1U << i)) == 0U) {
            continue;
        }
        if (IOMuxManager_PinConfigure(ACTUATOR_PAD, s_servos[i].pin, ACTUATOR_PWM_FUNCTION) != CSK_DRIVER_OK ||
            HAL_GPT_EnablePWM(s_gpt, s_servos[i].channel) != CSK_DRIVER_OK) {
            ret = -EIO;
            break;
        }
        s_servos[i].enabled = true;
    }

    if (ret != 0) {
        for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
            if ((newly_enabled & (1U << i)) != 0U) {
                (void)HAL_GPT_DisablePWM(s_gpt, s_servos[i].channel);
                (void)actuator_pin_gpio_low(s_servos[i].pin);
                s_servos[i].enabled = false;
            }
        }
    }
    return ret;
}

static int servo_apply_locked(servo_state_t *servo)
{
    uint32_t index = (uint32_t)(servo - s_servos);
    return index < SERVICE_V2_SERVO_COUNT ? servo_apply_mask_locked((uint8_t)(1U << index)) : -EINVAL;
}

static int servo_enable_locked(servo_state_t *servo)
{
    s_usb_idle_released = false;
    if (servo->enabled) {
        return servo_apply_locked(servo);
    }

    /* Deliberately do not initialize or power-cycle GPT0 here. The display
     * backlight owns channel 1; a global GPT reset would blank or glitch it. */
    uint32_t control = CSK_GPT_PWM_MODE | CSK_GPT_PWM_CLKSRC_PCLK | CSK_GPT_PWM_OUTMODE_EDGE_ALIGNED |
                       CSK_GPT_PWM_OUTPOLARITY_HIGH | s_servo_divider_control | CSK_GPT_PWM_OPERATION_MODE_PWM;

    int ret = HAL_GPT_PWMControl(s_gpt, control, servo->channel);
    if (ret != CSK_DRIVER_OK) {
        return -EIO;
    }

    ret = servo_apply_locked(servo);
    if (ret != 0) {
        return ret;
    }

    ret = IOMuxManager_PinConfigure(ACTUATOR_PAD, servo->pin, ACTUATOR_PWM_FUNCTION);
    if (ret != CSK_DRIVER_OK) {
        actuator_pin_gpio_low(servo->pin);
        return -EIO;
    }

    ret = HAL_GPT_EnablePWM(s_gpt, servo->channel);
    if (ret != CSK_DRIVER_OK) {
        actuator_pin_gpio_low(servo->pin);
        return -EIO;
    }

    servo->enabled = true;
    return 0;
}

static int servo_disable_locked(servo_state_t *servo)
{
    s_usb_idle_released = false;
    int hal_ret = 0;
    if (servo->enabled) {
        hal_ret = HAL_GPT_DisablePWM(s_gpt, servo->channel);
    }

    /* Remapping to a preconfigured low GPIO guarantees no residual pulse even
     * if the GPT stop operation reports an error. */
    int gpio_ret = actuator_pin_gpio_low(servo->pin);
    servo->enabled = false;

    if (hal_ret != CSK_DRIVER_OK || gpio_ret != 0) {
        return -EIO;
    }
    return 0;
}

static int servo_disable_all_locked(void)
{
    int first_error = 0;
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        int ret = servo_disable_locked(&s_servos[i]);
        if (first_error == 0 && ret != 0) {
            first_error = ret;
        }
    }
    return first_error;
}

static inline __attribute__((always_inline)) uint32_t ws2812_cycle_now(void)
{
    return (uint32_t)__RV_CSR_READ(CSR_MCYCLE);
}

static inline __attribute__((always_inline)) void ws2812_wait_until(uint32_t deadline)
{
    /* All waits are far shorter than 2^31 MCYCLE ticks, so the signed delta is
     * wrap-safe even if the 32-bit counter rolls over during a frame. */
    while ((int32_t)(ws2812_cycle_now() - deadline) < 0) {
    }
}

static uint32_t ws2812_delay_derived_cycle_hz(void)
{
    /* Diagnostic only.  SysTick_Delay_Us() is not an accurate reference for
     * MCYCLE on this target: a requested 100 us produced a value about 14.5%
     * above the rate confirmed by both CRM and the PA08 wire period. */
    taskENTER_CRITICAL();
    uint32_t start = ws2812_cycle_now();
    SysTick_Delay_Us(WS2812_CALIBRATION_US);
    uint32_t elapsed = ws2812_cycle_now() - start;
    taskEXIT_CRITICAL();

    return (uint32_t)(((uint64_t)elapsed * 1000000U) / WS2812_CALIBRATION_US);
}

static uint32_t ws2812_cycle_rate_hz(void)
{
    /* MCYCLE follows the CP CPU clock on ARCS.  The 2026-08-05 PA08 capture
     * independently confirmed that CRM_GetCpuFreq()=300 MHz predicts the wire
     * period, while the delay-derived 343.56 MHz value produced 1.47 us from a
     * requested 1.28 us period. */
    return CRM_GetCpuFreq();
}

static uint8_t rgb_calibration_apply_gain(uint8_t value, uint16_t gain_permille)
{
    return (uint8_t)(((uint32_t)value * gain_permille + 500U) / 1000U);
}

static uint32_t rgb_calibration_wire_word(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness_percent)
{
    uint32_t effective_brightness_percent =
        ((uint32_t)brightness_percent * RGB_OUTPUT_SCALE_PERCENT + 50U) / 100U;

    red = rgb_calibration_apply_gain(s_rgb_gamma_lut[red], s_rgb_calibration.red_gain_permille);
    green = rgb_calibration_apply_gain(s_rgb_gamma_lut[green], s_rgb_calibration.green_gain_permille);
    blue = rgb_calibration_apply_gain(s_rgb_gamma_lut[blue], s_rgb_calibration.blue_gain_permille);

    uint32_t channels[3] = {
        ((uint32_t)red * effective_brightness_percent + 50U) / 100U,
        ((uint32_t)green * effective_brightness_percent + 50U) / 100U,
        ((uint32_t)blue * effective_brightness_percent + 50U) / 100U,
    };
    uint32_t first;
    uint32_t second;
    uint32_t third;

    switch (s_rgb_calibration.order) {
    case SERVICE_V2_RGB_ORDER_RGB:
        first = channels[0];
        second = channels[1];
        third = channels[2];
        break;
    case SERVICE_V2_RGB_ORDER_RBG:
        first = channels[0];
        second = channels[2];
        third = channels[1];
        break;
    case SERVICE_V2_RGB_ORDER_GBR:
        first = channels[1];
        second = channels[2];
        third = channels[0];
        break;
    case SERVICE_V2_RGB_ORDER_BRG:
        first = channels[2];
        second = channels[0];
        third = channels[1];
        break;
    case SERVICE_V2_RGB_ORDER_BGR:
        first = channels[2];
        second = channels[1];
        third = channels[0];
        break;
    case SERVICE_V2_RGB_ORDER_GRB:
    default:
        first = channels[1];
        second = channels[0];
        third = channels[2];
        break;
    }

    return (first << 16U) | (second << 8U) | third;
}

static int _FAST_FUNC_SRAM __attribute__((noinline))
ws2812_send_pixels_locked(uint8_t count, const uint32_t *pixels)
{
    if (count < SERVICE_V2_RGB_STRIP_MIN_COUNT || count > SERVICE_V2_RGB_STRIP_MAX_COUNT || pixels == NULL) {
        return -EINVAL;
    }

    uint32_t core_hz = ws2812_cycle_rate_hz();
    s_ws2812_cycle_hz = core_hz;
    if (core_hz < WS2812_MIN_CORE_CLOCK_HZ) {
        return -ERANGE;
    }

    uint32_t t0h_cycles = (uint32_t)(((uint64_t)core_hz * WS2812_T0H_NS + 999999999U) / 1000000000U);
    uint32_t t1h_cycles = (uint32_t)(((uint64_t)core_hz * WS2812_T1H_NS + 999999999U) / 1000000000U);
    uint32_t bit_cycles = (uint32_t)(((uint64_t)core_hz * WS2812_BIT_PERIOD_NS + 999999999U) / 1000000000U);
    uint32_t wire_word;

    int ret = IOMuxManager_PinForce(ACTUATOR_PAD, WS2812_PIN, HAL_IOMUX_FORCE_OFF);
    if (ret == CSK_DRIVER_OK) {
        ret = actuator_pin_gpio_low(WS2812_PIN);
    }
    if (ret != 0) {
        return -EIO;
    }
    SysTick_Delay_Us(WS2812_RESET_US);

    taskENTER_CRITICAL();
    uint32_t next_rise = ws2812_cycle_now() + bit_cycles;
    for (uint32_t pixel = 0U; pixel < count; ++pixel) {
        wire_word = pixels[pixel];
        for (uint32_t mask = 1UL << 23; mask != 0U; mask >>= 1U) {
            uint32_t high_cycles = (wire_word & mask) ? t1h_cycles : t0h_cycles;

            /* Select the bit before its scheduled edge.  SET and CLEAR are
             * issued from deadlines on the same absolute timeline, so their
             * similar APB delays cancel without extending the wire period. */
            ws2812_wait_until(next_rise);
            IP_GPIOA->REG_DOUTSET.all = WS2812_PIN_MASK;
            ws2812_wait_until(next_rise + high_cycles);
            IP_GPIOA->REG_DOUTCLEAR.all = WS2812_PIN_MASK;
            next_rise += bit_cycles;
        }
    }
    IP_GPIOA->REG_DOUTCLEAR.all = WS2812_PIN_MASK;
    taskEXIT_CRITICAL();

    SysTick_Delay_Us(WS2812_RESET_US);
    return 0;
}

static int ws2812_send_pattern_locked(uint8_t count, uint8_t red, uint8_t green, uint8_t blue,
                                      uint8_t brightness_percent, bool eyes)
{
    if (count < SERVICE_V2_RGB_STRIP_MIN_COUNT || count > SERVICE_V2_RGB_STRIP_MAX_COUNT ||
        brightness_percent > SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT) return -EINVAL;
    uint32_t word = rgb_calibration_wire_word(red, green, blue, brightness_percent);
    uint32_t pixels[SERVICE_V2_RGB_STRIP_MAX_COUNT];
    for (uint32_t i = 0; i < count; ++i) pixels[i] = service_v2_rgb_pixel_word(i, word, eyes);
    return ws2812_send_pixels_locked(count, pixels);
}

static int ws2812_send_uniform_locked(uint8_t count, uint8_t red, uint8_t green,
                                      uint8_t blue, uint8_t brightness_percent)
{
    return ws2812_send_pattern_locked(count, red, green, blue, brightness_percent, false);
}

static int _FAST_FUNC_SRAM __attribute__((noinline)) ws2812_scope_train_locked(bool one)
{
    uint32_t core_hz = s_ws2812_cycle_hz;
    if (core_hz == 0U) {
        core_hz = ws2812_cycle_rate_hz();
        s_ws2812_cycle_hz = core_hz;
    }
    if (core_hz < WS2812_MIN_CORE_CLOCK_HZ) {
        return -ERANGE;
    }

    uint32_t high_ns = one ? WS2812_T1H_NS : WS2812_T0H_NS;
    uint32_t high_cycles = (uint32_t)(((uint64_t)core_hz * high_ns + 999999999U) / 1000000000U);
    uint32_t bit_cycles = (uint32_t)(((uint64_t)core_hz * WS2812_BIT_PERIOD_NS + 999999999U) / 1000000000U);

    IP_GPIOA->REG_DOUTCLEAR.all = WS2812_PIN_MASK;
    SysTick_Delay_Us(WS2812_RESET_US);

    /* Use a long diagnostic train so the reset/yield gap occupies only a small
     * part of the capture; the previous 4096-bit train made AUTO trigger jump
     * visibly between the data burst and its gap. */
    taskENTER_CRITICAL();
    uint32_t next_rise = ws2812_cycle_now() + bit_cycles;
    for (uint32_t i = 0U; i < WS2812_SCOPE_TRAIN_BITS; ++i) {
        ws2812_wait_until(next_rise);
        IP_GPIOA->REG_DOUTSET.all = WS2812_PIN_MASK;
        ws2812_wait_until(next_rise + high_cycles);
        IP_GPIOA->REG_DOUTCLEAR.all = WS2812_PIN_MASK;
        next_rise += bit_cycles;
    }
    IP_GPIOA->REG_DOUTCLEAR.all = WS2812_PIN_MASK;
    taskEXIT_CRITICAL();

    SysTick_Delay_Us(WS2812_RESET_US);
    return 0;
}

static void ws2812_scope_task(void *arg)
{
    (void)arg;

    int ret = 0;
    while (s_ws2812_scope_running) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        if (s_ws2812_scope_running) {
            ret = ws2812_scope_train_locked(s_ws2812_scope_one);
        }
        lisa_mutex_unlock(s_lock);

        if (ret != 0) {
            break;
        }

        /* Block for one scheduler tick so USB/ADB and the shell can always run
         * and execute the stop command.  The 20.97 ms train keeps this short
         * scheduling/reset gap to a small part of an AUTO-trigger capture. */
        lisa_thread_mdelay(WS2812_SCOPE_YIELD_MS);
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    IP_GPIOA->REG_DOUTCLEAR.all = WS2812_PIN_MASK;
    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "continuous WS2812 scope output failed: %d", ret);
    }
    s_ws2812_scope_running = false;
    s_ws2812_scope_task_alive = false;
}

static int ws2812_scope_stop_wait(void)
{
    s_ws2812_scope_running = false;
    for (uint32_t waited_ms = 0U; s_ws2812_scope_task_alive && waited_ms < WS2812_SCOPE_STOP_MS; ++waited_ms) {
        lisa_thread_mdelay(1U);
    }

    return s_ws2812_scope_task_alive ? -ETIMEDOUT : 0;
}

static bool servo_easing_valid(service_v2_servo_easing_t easing)
{
    return easing >= SERVICE_V2_SERVO_EASING_LINEAR && easing <= SERVICE_V2_SERVO_EASING_EASE_OUT;
}

static uint32_t servo_eased_progress(service_v2_servo_easing_t easing, uint32_t elapsed_ms, uint32_t duration_ms)
{
    if (duration_ms == 0U || elapsed_ms >= duration_ms) {
        return SERVO_EASING_SCALE;
    }

    uint32_t x = (uint32_t)(((uint64_t)elapsed_ms * SERVO_EASING_SCALE) / duration_ms);
    uint32_t x2 = (uint32_t)(((uint64_t)x * x) / SERVO_EASING_SCALE);

    switch (easing) {
    case SERVICE_V2_SERVO_EASING_SMOOTH: {
        uint32_t x3 = (uint32_t)(((uint64_t)x2 * x) / SERVO_EASING_SCALE);
        return 3U * x2 - 2U * x3;
    }
    case SERVICE_V2_SERVO_EASING_EASE_IN:
        return x2;
    case SERVICE_V2_SERVO_EASING_EASE_OUT:
        return 2U * x - x2;
    case SERVICE_V2_SERVO_EASING_LINEAR:
    default:
        return x;
    }
}

static int servo_frame_start_locked(const uint16_t pulse_us[SERVICE_V2_SERVO_COUNT], uint32_t duration_ms,
                                    service_v2_servo_easing_t easing)
{
    if (pulse_us == NULL || duration_ms > SERVICE_V2_SERVO_FRAME_MAX_DURATION_MS || !servo_easing_valid(easing)) {
        return -EINVAL;
    }
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if (pulse_us[i] < SERVICE_V2_SERVO_MIN_PULSE_US || pulse_us[i] > SERVICE_V2_SERVO_MAX_PULSE_US) {
            return -EINVAL;
        }
    }
    if (s_servo_sweep_task_alive) {
        return -EBUSY;
    }

    servo_frame_state_t previous_frame = s_servo_frame;
    uint16_t previous_us[SERVICE_V2_SERVO_COUNT];
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        previous_us[i] = s_servos[i].pulse_us;
        s_servo_frame.start_us[i] = s_servos[i].pulse_us;
        s_servo_frame.target_us[i] = pulse_us[i];
    }

    int ret = servo_enable_mask_locked(SERVICE_V2_SERVO_ALL_MASK);
    if (ret != 0) {
        s_servo_frame = previous_frame;
        return ret;
    }

    s_usb_idle_released = false;
    s_servo_frame.sequence++;
    s_servo_frame.duration_ms = duration_ms;
    s_servo_frame.elapsed_ms = 0U;
    s_servo_frame.easing = easing;
    s_servo_frame.watchdog_expired = false;
    s_servo_frame.active = false;

    if (duration_ms == 0U) {
        for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
            s_servos[i].pulse_us = pulse_us[i];
        }
        ret = servo_apply_mask_locked(SERVICE_V2_SERVO_ALL_MASK);
        if (ret != 0) {
            for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
                s_servos[i].pulse_us = previous_us[i];
            }
            (void)servo_apply_mask_locked(SERVICE_V2_SERVO_ALL_MASK);
            s_servo_frame = previous_frame;
            return ret;
        }
    } else {
        s_servo_frame.active = true;
    }

    s_servo_frame.watchdog_remaining_ms = s_servo_frame.watchdog_ms == 0U ? 0U : s_servo_frame.watchdog_ms;
    return 0;
}

static void servo_frame_task(void *arg)
{
    (void)arg;

    for (;;) {
        lisa_thread_mdelay(SERVICE_V2_SERVO_FRAME_STEP_MS);

        bool watchdog_expired = false;
        int apply_error = 0;
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);

        uint8_t enabled_mask = servo_enabled_mask_locked();
        if (s_servo_frame.watchdog_ms != 0U && enabled_mask != 0U) {
            if (s_servo_frame.watchdog_remaining_ms <= SERVICE_V2_SERVO_FRAME_STEP_MS) {
                s_servo_frame.watchdog_remaining_ms = 0U;
                s_servo_frame.watchdog_expired = true;
                s_servo_frame.active = false;
                s_servo_frame.sequence++;
                apply_error = servo_disable_all_locked();
                int rgb_ret = rgb_release_locked();
                if (apply_error == 0) {
                    apply_error = rgb_ret;
                }
                watchdog_expired = true;
            } else {
                s_servo_frame.watchdog_remaining_ms -= SERVICE_V2_SERVO_FRAME_STEP_MS;
            }
        } else if (enabled_mask == 0U) {
            s_servo_frame.watchdog_remaining_ms = 0U;
        }

        if (!watchdog_expired && s_servo_frame.active) {
            uint32_t remaining_ms = s_servo_frame.duration_ms - s_servo_frame.elapsed_ms;
            uint32_t step_ms =
                remaining_ms < SERVICE_V2_SERVO_FRAME_STEP_MS ? remaining_ms : SERVICE_V2_SERVO_FRAME_STEP_MS;
            s_servo_frame.elapsed_ms += step_ms;
            uint32_t progress =
                servo_eased_progress(s_servo_frame.easing, s_servo_frame.elapsed_ms, s_servo_frame.duration_ms);

            for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
                int32_t delta = (int32_t)s_servo_frame.target_us[i] - (int32_t)s_servo_frame.start_us[i];
                int32_t value =
                    (int32_t)s_servo_frame.start_us[i] + (int32_t)(((int64_t)delta * progress) / SERVO_EASING_SCALE);
                s_servos[i].pulse_us = (uint16_t)value;
            }

            apply_error = servo_apply_mask_locked(SERVICE_V2_SERVO_ALL_MASK);
            if (apply_error != 0) {
                s_servo_frame.active = false;
                (void)servo_disable_all_locked();
                (void)rgb_release_locked();
            } else if (s_servo_frame.elapsed_ms >= s_servo_frame.duration_ms) {
                s_servo_frame.active = false;
            }
        }

        lisa_mutex_unlock(s_lock);

        if (watchdog_expired) {
            LISA_LOGW(TAG, "servo watchdog expired; all outputs disabled (status=%d)", apply_error);
        } else if (apply_error != 0) {
            LISA_LOGE(TAG, "servo frame update failed: %d; all outputs disabled", apply_error);
        }
    }
}

int service_v2_actuators_init(void)
{
    if (s_ready) {
        return 0;
    }

    s_gpioa = lisa_device_get(ACTUATOR_GPIO_DEVICE);
    if (!lisa_device_ready(s_gpioa)) {
        LISA_LOGE(TAG, "%s is not ready", ACTUATOR_GPIO_DEVICE);
        return -ENODEV;
    }

    /* Getting the LISA device guarantees that GPT0 has already been initialized
     * and powered without duplicating the driver's global init sequence. */
    s_pwm0 = lisa_device_get(ACTUATOR_PWM_DEVICE);
    if (!lisa_device_ready(s_pwm0)) {
        LISA_LOGE(TAG, "%s is not ready", ACTUATOR_PWM_DEVICE);
        return -ENODEV;
    }

    s_gpt = GPT0_PWM();
    if (s_gpt == NULL) {
        LISA_LOGE(TAG, "GPT0 PWM handle is unavailable");
        return -ENODEV;
    }

    if (s_lock == NULL) {
        s_lock = lisa_mutex_create();
        if (s_lock == NULL) {
            LISA_LOGE(TAG, "failed to create actuator lock");
            return -ENOMEM;
        }
    }

    int ret = servo_clock_select();
    if (ret != 0) {
        LISA_LOGE(TAG, "cannot derive a 50 Hz GPT period: %d", ret);
        return ret;
    }

    for (uint32_t i = 0; i < SERVICE_V2_SERVO_COUNT; i++) {
        s_servos[i].pulse_us = SERVICE_V2_SERVO_CENTER_PULSE_US;
        s_servos[i].enabled = false;
        s_servo_frame.start_us[i] = SERVICE_V2_SERVO_CENTER_PULSE_US;
        s_servo_frame.target_us[i] = SERVICE_V2_SERVO_CENTER_PULSE_US;
        ret = actuator_pin_gpio_low(s_servos[i].pin);
        if (ret != 0) {
            LISA_LOGE(TAG, "failed to hold PA%02u low: %d", s_servos[i].pin, ret);
            return -EIO;
        }
    }

    ret = actuator_pin_gpio_low(WS2812_PIN);
    if (ret != 0) {
        LISA_LOGE(TAG, "failed to configure PA08 for RGB data: %d", ret);
        return -EIO;
    }

    /* Do not emit a frame during boot.  A cold-start test already proved that
     * PA08-low leaves this LED off, while a malformed all-zero frame makes it
     * latch white.  Keep boot deterministic and test frames only on command. */
    ret = IOMuxManager_PinForce(ACTUATOR_PAD, WS2812_PIN, HAL_IOMUX_FORCE_OUT_LOW);
    if (ret != CSK_DRIVER_OK) {
        LISA_LOGE(TAG, "failed to force PA08 low: %d", ret);
        return -EIO;
    }

    s_servo_frame.active = false;
    s_servo_frame.watchdog_remaining_ms = 0U;
    s_servo_frame.watchdog_expired = false;
    s_rgb_status.count = SERVICE_V2_RGB_STRIP_DEFAULT_COUNT;
    s_rgb_status.red = 0U;
    s_rgb_status.green = 0U;
    s_rgb_status.blue = 0U;
    s_rgb_status.brightness_percent = SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT;
    s_rgb_status.owner = SERVICE_V2_RGB_OWNER_SYSTEM;
    rgb_calibration_load();

    s_ready = true;
    lisa_thread_attr_t frame_attr = {
        .name = (uint8_t *)"servo_frame",
        .stack_size = SERVO_FRAME_TASK_STACK,
        .priority = LISA_OS_PRIORITY_LOW,
    };
    if (lisa_thread_create(&frame_attr, servo_frame_task, NULL) == NULL) {
        s_ready = false;
        LISA_LOGE(TAG, "failed to create servo frame worker");
        return -ENOMEM;
    }

    LISA_LOGI(TAG,
              "ready: servos PA04-PA07/GPT CH4-CH7 50Hz; "
              "RGB strip PA08 count=%u boot hold=0; servos disabled",
              SERVICE_V2_RGB_STRIP_DEFAULT_COUNT);
    return 0;
}

bool service_v2_actuators_is_ready(void)
{
    return s_ready;
}

int service_v2_actuators_servo_set_us(uint8_t servo, uint16_t pulse_us)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (servo >= SERVICE_V2_SERVO_COUNT || pulse_us < SERVICE_V2_SERVO_MIN_PULSE_US ||
        pulse_us > SERVICE_V2_SERVO_MAX_PULSE_US) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    int ret;
    if (s_servo_frame.active) {
        ret = -EBUSY;
    } else {
        s_usb_idle_released = false;
        s_servos[servo].pulse_us = pulse_us;
        s_servo_frame.start_us[servo] = pulse_us;
        s_servo_frame.target_us[servo] = pulse_us;
        ret = s_servos[servo].enabled ? servo_apply_locked(&s_servos[servo]) : 0;
    }
    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "servo %u pulse update failed: %d", servo, ret);
    }
    return ret;
}

int service_v2_actuators_servo_enable(uint8_t servo)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (servo >= SERVICE_V2_SERVO_COUNT) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    int ret = s_servo_frame.active ? -EBUSY : servo_enable_locked(&s_servos[servo]);
    if (ret == 0 && s_servo_frame.watchdog_ms != 0U) {
        s_servo_frame.watchdog_remaining_ms = s_servo_frame.watchdog_ms;
        s_servo_frame.watchdog_expired = false;
    }
    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "servo %u enable failed: %d", servo, ret);
    }
    return ret;
}

int service_v2_actuators_servo_disable(uint8_t servo)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (servo >= SERVICE_V2_SERVO_COUNT) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    int ret = s_servo_frame.active ? -EBUSY : servo_disable_locked(&s_servos[servo]);
    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "servo %u disable failed: %d", servo, ret);
    }
    return ret;
}

int service_v2_actuators_servo_disable_all(void)
{
    if (!s_ready) {
        return -ENODEV;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_servo_frame.active = false;
    s_servo_frame.watchdog_remaining_ms = 0U;
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        s_servo_frame.start_us[i] = s_servos[i].pulse_us;
        s_servo_frame.target_us[i] = s_servos[i].pulse_us;
    }
    int first_error = servo_disable_all_locked();
    lisa_mutex_unlock(s_lock);
    return first_error;
}

int service_v2_actuators_servo_release_usb_idle(void)
{
    if (!s_ready) return -ENODEV;
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    int ret = -EBUSY;
    /* Check and release under the same lock used by raw frames, sweep reserve
     * and manual watchdog leases. USB may have been removed while acquiring it.
     * A nonzero lease belongs to its caller; do not revoke or refresh it here. */
    if (power_is_usb_plugged() && !s_servo_frame.active &&
        !s_servo_sweep_task_alive && s_servo_frame.watchdog_ms == 0U) {
        uint8_t enabled = servo_enabled_mask_locked();
        ret = enabled == 0U ? 0 : servo_disable_all_locked();
        if (enabled != 0U)
            s_usb_idle_released = ret == 0 && enabled == SERVICE_V2_SERVO_ALL_MASK;
        s_servo_frame.watchdog_remaining_ms = 0U;
    }
    lisa_mutex_unlock(s_lock);
    return ret;
}

int service_v2_actuators_servo_frame(const uint16_t pulse_us[SERVICE_V2_SERVO_COUNT], uint32_t duration_ms,
                                     service_v2_servo_easing_t easing)
{
    if (!s_ready) {
        return -ENODEV;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    int ret = servo_frame_start_locked(pulse_us, duration_ms, easing);
    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "servo frame rejected: %d", ret);
    }
    return ret;
}

int service_v2_actuators_servo_frame_stop(void)
{
    if (!s_ready) {
        return -ENODEV;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    if (s_servo_frame.active) {
        s_servo_frame.sequence++;
    }
    s_servo_frame.active = false;
    s_servo_frame.duration_ms = 0U;
    s_servo_frame.elapsed_ms = 0U;
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        s_servo_frame.start_us[i] = s_servos[i].pulse_us;
        s_servo_frame.target_us[i] = s_servos[i].pulse_us;
    }
    s_servo_frame.watchdog_remaining_ms =
        s_servo_frame.watchdog_ms == 0U || servo_enabled_mask_locked() == 0U ? 0U : s_servo_frame.watchdog_ms;
    lisa_mutex_unlock(s_lock);
    return 0;
}

int service_v2_actuators_servo_get_status(service_v2_servo_status_t *status)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (status == NULL) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    status->sequence = s_servo_frame.sequence;
    status->duration_ms = s_servo_frame.duration_ms;
    status->elapsed_ms = s_servo_frame.elapsed_ms;
    status->watchdog_ms = s_servo_frame.watchdog_ms;
    status->watchdog_remaining_ms = s_servo_frame.watchdog_remaining_ms;
    status->enabled_mask = servo_enabled_mask_locked();
    status->sweep_servo = s_servo_sweep_task_alive ? s_servo_sweep_channel : UINT8_MAX;
    status->easing = s_servo_frame.easing;
    status->frame_active = s_servo_frame.active;
    status->sweep_active = s_servo_sweep_task_alive;
    status->watchdog_expired = s_servo_frame.watchdog_expired;
    status->usb_idle_released = s_usb_idle_released;
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        status->current_us[i] = s_servos[i].pulse_us;
        status->target_us[i] = s_servo_frame.target_us[i];
    }
    lisa_mutex_unlock(s_lock);
    return 0;
}

int service_v2_actuators_servo_watchdog_set(uint32_t timeout_ms)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (timeout_ms != 0U &&
        (timeout_ms < SERVICE_V2_SERVO_WATCHDOG_MIN_MS || timeout_ms > SERVICE_V2_SERVO_WATCHDOG_MAX_MS)) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    if (timeout_ms != 0U && s_servo_sweep_task_alive) {
        lisa_mutex_unlock(s_lock);
        return -EBUSY;
    }
    s_servo_frame.watchdog_ms = timeout_ms;
    s_servo_frame.watchdog_remaining_ms = timeout_ms == 0U || servo_enabled_mask_locked() == 0U ? 0U : timeout_ms;
    s_servo_frame.watchdog_expired = false;
    lisa_mutex_unlock(s_lock);
    return 0;
}

int service_v2_actuators_servo_watchdog_kick(void)
{
    if (!s_ready) {
        return -ENODEV;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    if (s_servo_frame.watchdog_ms != 0U && servo_enabled_mask_locked() != 0U) {
        s_servo_frame.watchdog_remaining_ms = s_servo_frame.watchdog_ms;
        s_servo_frame.watchdog_expired = false;
    }
    lisa_mutex_unlock(s_lock);
    return 0;
}

static void servo_sweep_task(void *arg)
{
    (void)arg;

    const uint32_t span = SERVICE_V2_SERVO_MAX_PULSE_US - SERVICE_V2_SERVO_MIN_PULSE_US;
    const uint8_t servo = s_servo_sweep_channel;
    const uint32_t period_ms = s_servo_sweep_period_ms;
    const uint32_t half_ms = period_ms / 2U;
    uint32_t elapsed_ms = 0U;
    int ret = 0;

    while (s_servo_sweep_running) {
        /* Triangle wave: one end stop to the other and back per period. */
        const uint32_t phase = elapsed_ms % period_ms;
        const uint32_t offset = (phase < half_ms) ? span * phase / half_ms : span * (period_ms - phase) / half_ms;

        ret = service_v2_actuators_servo_set_us(servo, (uint16_t)(SERVICE_V2_SERVO_MIN_PULSE_US + offset));
        if (ret != 0) {
            break;
        }

        lisa_thread_mdelay(SERVO_SWEEP_STEP_MS);
        elapsed_ms += SERVO_SWEEP_STEP_MS;
    }

    (void)service_v2_actuators_servo_disable(servo);

    if (ret != 0) {
        LISA_LOGE(TAG, "servo %u sweep failed: %d", servo, ret);
    }
    s_servo_sweep_running = false;
    s_servo_sweep_task_alive = false;
}

static int servo_sweep_stop_wait(void)
{
    s_servo_sweep_running = false;

    uint32_t waited_ms = 0U;
    for (; s_servo_sweep_task_alive && waited_ms < SERVO_SWEEP_STOP_MS; waited_ms += SERVO_SWEEP_STEP_MS) {
        lisa_thread_mdelay(SERVO_SWEEP_STEP_MS);
    }

    return s_servo_sweep_task_alive ? -ETIMEDOUT : 0;
}

int service_v2_actuators_servo_sweep_start(uint8_t servo, uint16_t period_ms)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (servo >= SERVICE_V2_SERVO_COUNT || period_ms < SERVICE_V2_SERVO_SWEEP_MIN_PERIOD_MS ||
        period_ms > SERVICE_V2_SERVO_SWEEP_MAX_PERIOD_MS) {
        return -EINVAL;
    }
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    bool busy = s_servo_sweep_task_alive || s_servo_frame.active || s_servo_frame.watchdog_ms != 0U;
    if (!busy) {
        /* Reserve the legacy motion owner before releasing the lock so a
         * simultaneous frame cannot start while this channel is prepared. */
        s_servo_sweep_channel = servo;
        s_servo_sweep_period_ms = period_ms;
        s_servo_sweep_task_alive = true;
    }
    lisa_mutex_unlock(s_lock);
    if (busy) {
        return -EBUSY;
    }

    /* Start from centre so the first step is never a full-span slam. */
    int ret = service_v2_actuators_servo_set_us(servo, SERVICE_V2_SERVO_CENTER_PULSE_US);
    if (ret == 0) {
        ret = service_v2_actuators_servo_enable(servo);
    }
    if (ret != 0) {
        s_servo_sweep_task_alive = false;
        return ret;
    }

    s_servo_sweep_running = true;

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"servo_sweep",
        .stack_size = SERVO_SWEEP_TASK_STACK,
        .priority = LISA_OS_PRIORITY_LOW,
    };
    if (lisa_thread_create(&attr, servo_sweep_task, NULL) == NULL) {
        s_servo_sweep_running = false;
        (void)service_v2_actuators_servo_disable(servo);
        s_servo_sweep_task_alive = false;
        return -ENOMEM;
    }

    return 0;
}

int service_v2_actuators_servo_sweep_stop(void)
{
    if (!s_ready) {
        return -ENODEV;
    }

    return servo_sweep_stop_wait();
}

bool service_v2_actuators_servo_sweep_active(uint8_t *servo)
{
    if (!s_servo_sweep_task_alive) {
        return false;
    }
    if (servo != NULL) {
        *servo = s_servo_sweep_channel;
    }
    return true;
}

static bool rgb_strip_params_valid(uint8_t count, uint8_t brightness_percent)
{
    return count >= SERVICE_V2_RGB_STRIP_MIN_COUNT && count <= SERVICE_V2_RGB_STRIP_MAX_COUNT &&
           brightness_percent <= SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT;
}

static int rgb_strip_write_locked(uint8_t count, uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness_percent,
                                  service_v2_rgb_owner_t owner)
{
    if (!rgb_strip_params_valid(count, brightness_percent)) {
        return -EINVAL;
    }

    int ret = 0;
    if (count < s_rgb_status.count) {
        /* Pixels beyond the new logical length would otherwise retain their
         * previous latched colour. Clear the old extent before shrinking. */
        ret = ws2812_send_uniform_locked(s_rgb_status.count, 0U, 0U, 0U, SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT);
    }
    if (ret == 0) {
        ret = ws2812_send_pattern_locked(count, red, green, blue, brightness_percent,
                                         owner != SERVICE_V2_RGB_OWNER_MANUAL);
    }
    if (ret == 0) {
        s_rgb_status.count = count;
        s_rgb_status.red = red;
        s_rgb_status.green = green;
        s_rgb_status.blue = blue;
        s_rgb_status.brightness_percent = brightness_percent;
        s_rgb_status.owner = owner;
    }
    return ret;
}

static int rgb_release_locked(void)
{
    int ret = ws2812_send_uniform_locked(s_rgb_status.count, 0U, 0U, 0U, SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT);
    /* Ownership is a software arbitration contract, so release it even if the
     * wire update fails.  The error still reaches the caller, but system
     * indicators must never remain locked out after an estop/lease expiry. */
    s_rgb_status.red = 0U;
    s_rgb_status.green = 0U;
    s_rgb_status.blue = 0U;
    s_rgb_status.brightness_percent = SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT;
    s_rgb_status.owner = SERVICE_V2_RGB_OWNER_SYSTEM;
    return ret;
}

int service_v2_actuators_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_ready) {
        return -ENODEV;
    }

    int ret = ws2812_scope_stop_wait();
    if (ret == 0) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        if (s_rgb_status.owner == SERVICE_V2_RGB_OWNER_SYSTEM) {
            ret = rgb_strip_write_locked(SERVICE_V2_RGB_STRIP_DEFAULT_COUNT, red, green, blue, SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT,
                                         SERVICE_V2_RGB_OWNER_SYSTEM);
        }
        lisa_mutex_unlock(s_lock);
    }

    if (ret != 0) {
        LISA_LOGE(TAG, "RGB update failed: %d", ret);
    }
    return ret;
}

int service_v2_actuators_rgb_off(void)
{
    return service_v2_actuators_rgb_set(0U, 0U, 0U);
}

int service_v2_actuators_rgb_system_frame(const service_v2_rgb_color_t pixels[5])
{
    if (!s_ready) return -ENODEV;
    if (pixels == NULL) return -EINVAL;
    int ret = 0;
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    if (s_rgb_status.owner == SERVICE_V2_RGB_OWNER_SYSTEM) {
        uint32_t words[SERVICE_V2_RGB_STRIP_DEFAULT_COUNT];
        for (unsigned i = 0; i < SERVICE_V2_RGB_STRIP_DEFAULT_COUNT; ++i) {
            words[i] = rgb_calibration_wire_word(pixels[i].red, pixels[i].green, pixels[i].blue,
                                                SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT);
        }
        ret = ws2812_send_pixels_locked(SERVICE_V2_RGB_STRIP_DEFAULT_COUNT, words);
        if (ret == 0) {
            s_rgb_status.count = SERVICE_V2_RGB_STRIP_DEFAULT_COUNT;
            s_rgb_status.red = pixels[1].red;
            s_rgb_status.green = pixels[1].green;
            s_rgb_status.blue = pixels[1].blue;
            s_rgb_status.brightness_percent = SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT;
        }
    }
    lisa_mutex_unlock(s_lock);
    return ret;
}

int service_v2_actuators_rgb_strip_set(uint8_t count, uint8_t red, uint8_t green, uint8_t blue,
                                       uint8_t brightness_percent)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (!rgb_strip_params_valid(count, brightness_percent)) {
        return -EINVAL;
    }

    int ret = ws2812_scope_stop_wait();
    if (ret == 0) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        ret = rgb_strip_write_locked(count, red, green, blue, brightness_percent, SERVICE_V2_RGB_OWNER_MANUAL);
        lisa_mutex_unlock(s_lock);
    }
    if (ret != 0) {
        LISA_LOGE(TAG, "RGB strip update failed: %d", ret);
    }
    return ret;
}

int service_v2_actuators_rgb_strip_set_count(uint8_t count)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (!rgb_strip_params_valid(count, 0U)) {
        return -EINVAL;
    }

    int ret = ws2812_scope_stop_wait();
    if (ret == 0) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        uint8_t clear_count = count > s_rgb_status.count ? count : s_rgb_status.count;
        ret = ws2812_send_uniform_locked(clear_count, 0U, 0U, 0U, SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT);
        if (ret == 0) {
            s_rgb_status.count = count;
            s_rgb_status.red = 0U;
            s_rgb_status.green = 0U;
            s_rgb_status.blue = 0U;
            s_rgb_status.brightness_percent = SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT;
            s_rgb_status.owner = SERVICE_V2_RGB_OWNER_MANUAL;
        }
        lisa_mutex_unlock(s_lock);
    }
    return ret;
}

int service_v2_actuators_rgb_get_status(service_v2_rgb_status_t *status)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (status == NULL) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    *status = s_rgb_status;
    lisa_mutex_unlock(s_lock);
    return 0;
}

const char *service_v2_actuators_rgb_order_name(service_v2_rgb_order_t order)
{
    switch (order) {
    case SERVICE_V2_RGB_ORDER_RGB:
        return "RGB";
    case SERVICE_V2_RGB_ORDER_RBG:
        return "RBG";
    case SERVICE_V2_RGB_ORDER_GRB:
        return "GRB";
    case SERVICE_V2_RGB_ORDER_GBR:
        return "GBR";
    case SERVICE_V2_RGB_ORDER_BRG:
        return "BRG";
    case SERVICE_V2_RGB_ORDER_BGR:
        return "BGR";
    default:
        return "UNKNOWN";
    }
}

int service_v2_actuators_rgb_calibration_get(service_v2_rgb_calibration_t *calibration, bool *persistent)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (calibration == NULL || persistent == NULL) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    *calibration = s_rgb_calibration;
    *persistent = s_rgb_calibration_persistent;
    lisa_mutex_unlock(s_lock);
    return 0;
}

int service_v2_actuators_rgb_calibration_set(const service_v2_rgb_calibration_t *calibration)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (!rgb_calibration_valid(calibration)) {
        return -EINVAL;
    }

    /* pow() never runs while interrupts are disabled or while the actuator
     * mutex is held. The sender only indexes the finished table. */
    uint8_t gamma_lut[UINT8_MAX + 1U];
    rgb_calibration_build_lut(calibration->gamma_x100, gamma_lut);

    int ret = ws2812_scope_stop_wait();
    if (ret != 0) {
        return ret;
    }
    ret = rgb_calibration_store(calibration);
    if (ret != 0) {
        return ret;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_rgb_calibration = *calibration;
    memcpy(s_rgb_gamma_lut, gamma_lut, sizeof(s_rgb_gamma_lut));
    s_rgb_calibration_persistent = true;

    /* Redraw from the unmodified logical state. This makes shell/host tuning
     * visible immediately without contaminating recorded RGB values. */
    ret = ws2812_send_pattern_locked(s_rgb_status.count, s_rgb_status.red, s_rgb_status.green, s_rgb_status.blue,
                                     s_rgb_status.brightness_percent, s_rgb_status.owner != SERVICE_V2_RGB_OWNER_MANUAL);
    lisa_mutex_unlock(s_lock);
    return ret;
}

int service_v2_actuators_rgb_calibration_reset(void)
{
    return service_v2_actuators_rgb_calibration_set(&s_rgb_calibration_default);
}

int service_v2_actuators_rgb_release(void)
{
    if (!s_ready) {
        return -ENODEV;
    }

    int ret = ws2812_scope_stop_wait();
    if (ret == 0) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        ret = rgb_release_locked();
        lisa_mutex_unlock(s_lock);
    }
    return ret;
}

int service_v2_actuators_motion_frame(const uint16_t pulse_us[SERVICE_V2_SERVO_COUNT], uint32_t duration_ms,
                                      service_v2_servo_easing_t easing, uint8_t count, uint8_t red, uint8_t green,
                                      uint8_t blue, uint8_t brightness_percent)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (pulse_us == NULL || duration_ms > SERVICE_V2_SERVO_FRAME_MAX_DURATION_MS || !servo_easing_valid(easing) ||
        !rgb_strip_params_valid(count, brightness_percent)) {
        return -EINVAL;
    }
    for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
        if (pulse_us[i] < SERVICE_V2_SERVO_MIN_PULSE_US || pulse_us[i] > SERVICE_V2_SERVO_MAX_PULSE_US) {
            return -EINVAL;
        }
    }

    int ret = ws2812_scope_stop_wait();
    if (ret != 0) {
        return ret;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    ret = servo_frame_start_locked(pulse_us, duration_ms, easing);
    if (ret == 0) {
        ret = rgb_strip_write_locked(count, red, green, blue, brightness_percent, SERVICE_V2_RGB_OWNER_ACTION);
        if (ret != 0) {
            /* The frame worker cannot run while this lock is held.  Fail safe:
             * cancel the registered target and remove every servo pulse. */
            s_servo_frame.active = false;
            s_servo_frame.duration_ms = 0U;
            s_servo_frame.elapsed_ms = 0U;
            s_servo_frame.watchdog_remaining_ms = 0U;
            (void)servo_disable_all_locked();
            (void)rgb_release_locked();
            for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
                s_servo_frame.start_us[i] = s_servos[i].pulse_us;
                s_servo_frame.target_us[i] = s_servos[i].pulse_us;
            }
        }
    }
    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "motion frame failed: %d", ret);
    }
    return ret;
}

int service_v2_actuators_rgb_data_level(bool high)
{
    if (!s_ready) {
        return -ENODEV;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    int ret =
        IOMuxManager_PinForce(ACTUATOR_PAD, WS2812_PIN, high ? HAL_IOMUX_FORCE_OUT_HIGH : HAL_IOMUX_FORCE_OUT_LOW);
    lisa_mutex_unlock(s_lock);

    if (ret != CSK_DRIVER_OK) {
        LISA_LOGE(TAG, "cannot force PA08 to %u: %d", high ? 1U : 0U, ret);
        return -EIO;
    }
    return 0;
}

int service_v2_actuators_rgb_gpio_level(bool high, bool *readback_high)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (readback_high == NULL) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);

    int ret = IOMuxManager_PinForce(ACTUATOR_PAD, WS2812_PIN, HAL_IOMUX_FORCE_OFF);
    if (ret == CSK_DRIVER_OK) {
        ret = IOMuxManager_PinConfigure(ACTUATOR_PAD, WS2812_PIN, RGB_GPIO_FUNCTION);
    }
    if (ret == CSK_DRIVER_OK) {
        ret =
            lisa_gpio_configure(s_gpioa, WS2812_PIN, high ? LISA_GPIO_CONFIG_OUTPUT_HIGH : LISA_GPIO_CONFIG_OUTPUT_LOW);
    }
    if (ret == 0) {
        ret = lisa_gpio_write_pin(s_gpioa, WS2812_PIN, high ? LISA_GPIO_HIGH : LISA_GPIO_LOW);
    }
    if (ret == 0) {
        int level = lisa_gpio_read_pin(s_gpioa, WS2812_PIN);
        if (level < 0) {
            ret = level;
        } else {
            *readback_high = level != LISA_GPIO_LOW;
        }
    }

    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "normal GPIO PA08=%u test failed: %d", high ? 1U : 0U, ret);
        return -EIO;
    }
    return 0;
}

int service_v2_actuators_rgb_clock_measure(uint32_t *reported_hz, uint32_t *measured_hz, uint32_t *cached_hz)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (reported_hz == NULL || measured_hz == NULL || cached_hz == NULL) {
        return -EINVAL;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    *reported_hz = CRM_GetCpuFreq();
    *measured_hz = ws2812_delay_derived_cycle_hz();
    *cached_hz = s_ws2812_cycle_hz;
    lisa_mutex_unlock(s_lock);
    return 0;
}

int service_v2_actuators_rgb_scope_start(bool one)
{
    if (!s_ready) {
        return -ENODEV;
    }
    if (s_ws2812_scope_task_alive) {
        return -EBUSY;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    int ret = IOMuxManager_PinForce(ACTUATOR_PAD, WS2812_PIN, HAL_IOMUX_FORCE_OFF);
    if (ret == CSK_DRIVER_OK) {
        ret = actuator_pin_gpio_low(WS2812_PIN);
    }
    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGE(TAG, "WS2812 scope start bit=%u failed: %d", one ? 1U : 0U, ret);
        return ret;
    }

    /* Force the first train to calibrate MCYCLE at the current run-time clock. */
    s_ws2812_cycle_hz = 0U;
    s_ws2812_scope_one = one;
    s_ws2812_scope_running = true;
    s_ws2812_scope_task_alive = true;

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"ws_scope",
        .stack_size = WS2812_SCOPE_TASK_STACK,
        .priority = LISA_OS_PRIORITY_LOW,
    };
    if (lisa_thread_create(&attr, ws2812_scope_task, NULL) == NULL) {
        s_ws2812_scope_running = false;
        s_ws2812_scope_task_alive = false;
        return -ENOMEM;
    }

    return 0;
}

int service_v2_actuators_rgb_scope_stop(void)
{
    if (!s_ready) {
        return -ENODEV;
    }

    return ws2812_scope_stop_wait();
}

int service_v2_actuators_rgb_self_test(void)
{
    static const uint8_t colors[][3] = {
        {WS2812_SELF_TEST_LEVEL, 0U, 0U},
        {0U, WS2812_SELF_TEST_LEVEL, 0U},
        {0U, 0U, WS2812_SELF_TEST_LEVEL},
    };

    if (!s_ready) {
        return -ENODEV;
    }

    service_v2_rgb_status_t status;
    int ret = service_v2_actuators_rgb_get_status(&status);
    if (ret != 0) {
        return ret;
    }

    int sequence_ret = 0;
    for (uint32_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        sequence_ret = service_v2_actuators_rgb_strip_set(status.count, colors[i][0], colors[i][1], colors[i][2],
                                                          SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT);
        if (sequence_ret != 0) {
            break;
        }
        lisa_thread_mdelay(WS2812_SELF_TEST_HOLD_MS);
    }

    int stop_ret = ws2812_scope_stop_wait();
    int restore_ret = stop_ret;
    if (stop_ret == 0) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        restore_ret = rgb_strip_write_locked(status.count, status.red, status.green, status.blue,
                                             status.brightness_percent, status.owner);
        lisa_mutex_unlock(s_lock);
    }
    return sequence_ret != 0 ? sequence_ret : restore_ret;
}
