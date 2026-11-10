#include <stdint.h>
#include <stdbool.h>

#include "arcs_ap.h"
#include "lisa_log.h"

#include "battery.h"
#include "battery_ui.h"
#include "power/power_manager.h"
#include "board.h"
#include "lisa_gpio.h"
#include "lisa_adc.h"
#include "sysutils.h"
#include "lisa_time.h"
#include "FreeRTOS.h"
#include "task.h"

#define TAG "battery"

#define BAT_ADC_PAD CONFIG_BATTERY_COLLECTION_ADC_PAD
#define BAT_ADC_CH  CONFIG_BATTERY_COLLECTION_ADC_CHANNEL

#if defined(BAT_TEMP_ADC_DEVICE_NAME) && defined(BAT_TEMP_ADC_CHANNEL)
#define BAT_TEMP_ADC_ENABLE 1
#else
#define BAT_TEMP_ADC_ENABLE 0
#endif

#ifdef CHARGE_DET_DEVICE_NAME
#define BATTERY_CHARGE_DET_PAD CHARGE_DET_DEVICE_NAME
#else
#define BATTERY_CHARGE_DET_PAD CONFIG_BATTERY_COLLECTION_CHARGE_DETECT_PAD
#endif

#define BATTERY_CHARGE_DET_PIN CHARGE_DET_PIN

#ifndef CHARGE_DET_ACTIVE_LEVEL
#define CHARGE_DET_ACTIVE_LEVEL 1
#endif

#define VBAT_MAX_VOLTAGE                (4350) /* 锂电池理论最大电压 */
#define VBAT_MIN_VOLTAGE                (3500) /* 锂电池理论最小电压 */
#define VBAT_PARTIAL_VOLTAGE_PERCENTAGE (40)   /* 硬件电池分压2/5, 百分比为40 */

/* Confirm elapsed time, independent of shell/UI query frequency. */
#define LOW_VOLTAGE_SHUTDOWN_CONFIRM_MS 5000U
#define POWER_INPUT_DEBOUNCE_MS        3000U
/* This is a signal confirmation interval, not an estimate of charge time. */
#define CHARGE_DONE_CONFIRM_MS         30000U

/* 低于该电压认为未接入电池，单位mV */
#define VBAT_PRESENT_THRESHOLD (2000)

#define VOLTAGE_FILTER_WINDOW_SIZE 5

static lisa_device_t *bat_adc_dev __psram_bss__ = NULL;
#if BAT_TEMP_ADC_ENABLE
static lisa_device_t *bat_temp_adc_dev __psram_bss__ = NULL;
#endif
static lisa_device_t *charge_det_dev __psram_bss__ = NULL;
static bool bat_adc_configured __psram_bss__;
static bool charge_det_configured __psram_bss__;

// 移动平均滤波器变量
static uint16_t voltage_history[VOLTAGE_FILTER_WINDOW_SIZE] __psram_bss__ = {0};
static uint8_t voltage_index __psram_bss__ = 0;
static bool voltage_buffer_full __psram_bss__ = false;
typedef struct {
    bool started;
    bool candidate;
    bool stable;
    bool confirmed;
    uint32_t since_ms;
} battery_input_t;

static battery_input_t usb_input __psram_bss__;
static battery_input_t charge_input __psram_bss__;
static battery_snapshot_t latest_sample __psram_bss__;
static bool sample_available __psram_bss__;

static void battery_input_update(battery_input_t *input, bool raw, uint32_t now_ms)
{
    if (!input->started || input->candidate != raw) {
        input->started = true;
        input->candidate = raw;
        input->confirmed = false;
        input->since_ms = now_ms;
    } else if ((uint32_t)(now_ms - input->since_ms) >= POWER_INPUT_DEBOUNCE_MS) {
        input->stable = raw;
        input->confirmed = true;
    }
}

bool battery_get_snapshot(battery_snapshot_t *snapshot)
{
    if (!snapshot) {
        return false;
    }
    taskENTER_CRITICAL();
    *snapshot = latest_sample;
    bool available = sample_available;
    taskEXIT_CRITICAL();
    if (!available) {
        snapshot->status = BATTERY_STATUS_UNKNOWN;
        snapshot->charger_level = -1;
    }
    return available;
}

bool battery_usb_plugged_stable_get(void)
{
    battery_snapshot_t sample;
    return battery_get_snapshot(&sample) && sample.usb_stable;
}

// 电池电压百分比查找表 (按10%步进，从0%到100%)
// 请根据实际电池特性填充对应的电压值 (单位: mV)
//
// 建议测量方法：
// 1. 将电池充满到100%，记录电压值
// 2. 以10%为步长，逐步放电并记录对应电压值
// 3. 确保测量时电池处于静置状态（非充放电状态）
// 4. 多次测量取平均值以提高准确性
//
// 数据来源：240032-7-6/7/8 实测恒流放电（155mA）曲线，3组取平均值
// 截止电压约 3.0V，满电开路电压约 4.20V（155mA 负载下测量值）
static const uint16_t battery_voltage_table_discharge[11] = {
    3010, // 0%  - 实测截止电压（约3.0V）
    3440, // 10%
    3576, // 20%
    3719, // 30%
    3781, // 40%
    3820, // 50%
    3860, // 60%
    3925, // 70%
    3973, // 80%
    4013, // 90%
    4160  // 100% - 满电端电压（155mA 负载）
};

// 充电电压百分比查找表 (按10%步进，从0%到100%)
// 数据来源：240032-7-6/7/8 实测恒流恒压充电（500mA CC / 4.2V CV）曲线，3组取平均值
// 说明：CC 阶段端电压上升较快（0%→10% 区间）；CV 阶段（10%→100%）电压在 4.04~4.20V 缓慢上升
static const uint16_t battery_voltage_table_charge[11] = {
    3257, // 0%  - 充电起始端电压（近空电池，500mA 充电开始）
    3677, // 10%
    3757, // 20%
    3850, // 30%
    3943, // 40%
    4002, // 50%
    4039, // 60%
    4071, // 70%
    4103, // 80%
    4150, // 90%
    4200  // 100% - CV 截止电压
};

/**
 * @brief 移动平均滤波器 - 对电压进行平滑滤波
 * @param new_voltage 新的电压值 (mV)
 * @return 滤波后的电压值 (mV)
 */
static uint16_t voltage_moving_average_filter(uint16_t new_voltage)
{
    // 将新电压值存入循环缓冲区
    voltage_history[voltage_index] = new_voltage;
    voltage_index = (voltage_index + 1) % VOLTAGE_FILTER_WINDOW_SIZE;

    // 检查缓冲区是否已满
    if (!voltage_buffer_full && voltage_index == 0) {
        voltage_buffer_full = true;
    }

    // 计算平均值
    uint32_t sum = 0;
    uint8_t count = voltage_buffer_full ? VOLTAGE_FILTER_WINDOW_SIZE : voltage_index;

    for (uint8_t i = 0; i < count; i++) {
        sum += voltage_history[i];
    }

    return (uint16_t)(sum / count);
}

static void voltage_filter_reset(uint16_t voltage)
{
    for (uint8_t i = 0; i < VOLTAGE_FILTER_WINDOW_SIZE; i++) {
        voltage_history[i] = voltage;
    }
    voltage_index = 0;
    voltage_buffer_full = true;
}

/**
 * @brief 基于查找表的电池百分比计算
 * @param voltage 当前电池电压 (mV)
 * @param table 电压-百分比查找表
 * @return 电池电量百分比 (0-100)
 */
static uint8_t voltage_to_percentage_by_table(uint16_t voltage, const uint16_t *table)
{
    // 边界处理
    if (voltage <= table[0]) {
        return 0;
    }
    if (voltage >= table[10]) {
        return 100;
    }

    // 在查找表中寻找合适的区间
    for (uint8_t i = 0; i < 10; i++) {
        if (voltage >= table[i] && voltage <= table[i + 1]) {
            // 线性插值计算精确百分比
            uint16_t voltage_diff = table[i + 1] - table[i];
            uint16_t current_diff = voltage - table[i];

            // 避免除零错误
            if (voltage_diff == 0) {
                return i * 10;
            }

            // 计算插值百分比
            uint8_t base_percentage = i * 10;
            uint8_t interpolated_percentage = (uint8_t)((current_diff * 10) / voltage_diff);

            return base_percentage + interpolated_percentage;
        }
    }

    // 如果没有找到合适区间，使用线性计算作为备选
    return (uint8_t)((voltage - VBAT_MIN_VOLTAGE) * 100 / (VBAT_MAX_VOLTAGE - VBAT_MIN_VOLTAGE));
}

static void battery_sample_pin_init(void)
{
    // Vbat voltage adc sample
    bat_adc_dev = lisa_device_get("adc0");
    if (lisa_device_ready(bat_adc_dev)) {
        lisa_adc_channel_config_t adc_ch_cfg = {
            .reference = LISA_ADC_REF_VDD_3V6,
            .resolution = LISA_ADC_RESOLUTION_10BIT,
        };
        bat_adc_configured = lisa_adc_channel_setup(bat_adc_dev, BAT_ADC_CH, &adc_ch_cfg) == LISA_DEVICE_OK;
        if (!bat_adc_configured) {
            LISA_LOGW(TAG, "Battery ADC channel setup failed");
        }
    } else {
        LISA_LOGW(TAG, "ADC device adc0 not ready");
    }

#if BAT_TEMP_ADC_ENABLE
    // Battery temperature ADC sample from CH32V003 exadc.
    bat_temp_adc_dev = lisa_device_get(BAT_TEMP_ADC_DEVICE_NAME);
    if (!lisa_device_ready(bat_temp_adc_dev)) {
        LISA_LOGW(TAG, "CH32 ADC device %s not ready", BAT_TEMP_ADC_DEVICE_NAME);
    }
#endif

    // charge status
    charge_det_dev = lisa_device_get(BATTERY_CHARGE_DET_PAD);
    if (lisa_device_ready(charge_det_dev)) {
        uint32_t flags = CHARGE_DET_ACTIVE_LEVEL ? LISA_GPIO_CONFIG_INPUT_PULLDOWN : LISA_GPIO_CONFIG_INPUT_PULLUP;
        charge_det_configured = lisa_gpio_configure(charge_det_dev, BATTERY_CHARGE_DET_PIN, flags) == LISA_DEVICE_OK;
        if (!charge_det_configured) {
            LISA_LOGW(TAG, "Charge detect GPIO setup failed");
        }
    } else {
        LISA_LOGW(TAG, "Charge detect device %s not ready", BATTERY_CHARGE_DET_PAD);
    }
}

void battery_init(void)
{
    static bool init_flag __psram_bss__ = false;

    if (init_flag) {
        return;
    }

    battery_sample_pin_init();
    battery_ui_init();

    init_flag = true;
}

uint16_t battery_get_voltage_mv(void)
{
    battery_snapshot_t sample;
    return battery_get_snapshot(&sample) && sample.valid ? sample.voltage_mv : 0;
}

uint16_t battery_get_temp_adc_raw(void)
{
#if BAT_TEMP_ADC_ENABLE
    uint16_t adc_raw_value = LISA_ADC_VALUE_INVALID;

    if (!lisa_device_ready(bat_temp_adc_dev)) {
        LISA_LOGW(TAG, "CH32 ADC device %s not ready", BAT_TEMP_ADC_DEVICE_NAME);
        return LISA_ADC_VALUE_INVALID;
    }

    int ret = lisa_adc_read(bat_temp_adc_dev, BAT_TEMP_ADC_CHANNEL, &adc_raw_value);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGW(TAG, "CH32 ADC read failed: dev=%s, ch=%d, ret=%d",
                  BAT_TEMP_ADC_DEVICE_NAME, BAT_TEMP_ADC_CHANNEL, ret);
        return LISA_ADC_VALUE_INVALID;
    }

    LISA_LOGI(TAG, "ch32 adc raw: dev=%s, ch=%d, raw=%u",
              BAT_TEMP_ADC_DEVICE_NAME, BAT_TEMP_ADC_CHANNEL, (unsigned int)adc_raw_value);
    return adc_raw_value;
#else
    return LISA_ADC_VALUE_INVALID;
#endif
}

uint8_t battery_get_pct_raw(void)
{
    battery_snapshot_t sample;
    return battery_get_snapshot(&sample) && sample.valid ? sample.raw_pct : 0;
}

battery_status_t battery_get_status(void)
{
    battery_snapshot_t sample;
    return battery_get_snapshot(&sample) && sample.valid ? sample.status : BATTERY_STATUS_UNKNOWN;
}

void battery_sample_update(void)
{
    static bool filter_valid __psram_bss__;
    static bool last_external_power __psram_bss__;
    static bool low_pending __psram_bss__;
    static bool shutdown_requested __psram_bss__;
    static uint32_t low_since_ms __psram_bss__;
    static bool full_pending __psram_bss__;
    static uint32_t full_since_ms __psram_bss__;

    battery_snapshot_t sample = {
        .sampled_at_ms = (uint32_t)lisa_os_get_tick_ms(),
        .adc_raw = LISA_ADC_VALUE_INVALID,
        .charger_level = -1,
        .status = BATTERY_STATUS_UNKNOWN,
    };
    sample.usb_raw = power_is_usb_plugged();
    battery_input_update(&usb_input, sample.usb_raw, sample.sampled_at_ms);
    sample.usb_stable = usb_input.stable;

    if (charge_det_configured && lisa_device_ready(charge_det_dev)) {
        int level = lisa_gpio_read_pin(charge_det_dev, BATTERY_CHARGE_DET_PIN);
        if (level == LISA_GPIO_LOW || level == LISA_GPIO_HIGH) {
            sample.charger_level = (int8_t)level;
            sample.charge_valid = true;
        }
    }
    bool charging_raw = sample.charge_valid && sample.charger_level == CHARGE_DET_ACTIVE_LEVEL;
    if (sample.charge_valid) {
        battery_input_update(&charge_input, charging_raw, sample.sampled_at_ms);
    } else {
        charge_input = (battery_input_t){0};
    }
    sample.charge_active = charge_input.stable;
    /* Raw plug-in already selects the charging curve and inhibits shutdown. */
    bool external_power = sample.usb_raw || sample.usb_stable || charging_raw || sample.charge_active;

    if (bat_adc_configured && lisa_device_ready(bat_adc_dev) &&
        lisa_adc_read(bat_adc_dev, BAT_ADC_CH, &sample.adc_raw) == LISA_DEVICE_OK && sample.adc_raw <= 1023U) {
        uint32_t adc_mv = LISA_ADC_RAW_TO_MV(sample.adc_raw, 3600, LISA_ADC_RESOLUTION_10BIT);
        sample.raw_voltage_mv = (uint16_t)(adc_mv * 100U / VBAT_PARTIAL_VOLTAGE_PERCENTAGE);
        sample.valid = sample.raw_voltage_mv <= VBAT_MAX_VOLTAGE;
    }

    if (sample.valid) {
        if (!filter_valid || external_power != last_external_power) {
            voltage_filter_reset(sample.raw_voltage_mv);
        }
        sample.voltage_mv = voltage_moving_average_filter(sample.raw_voltage_mv);
        filter_valid = true;
        last_external_power = external_power;
        const uint16_t *table = external_power ? battery_voltage_table_charge : battery_voltage_table_discharge;
        sample.raw_pct = voltage_to_percentage_by_table(sample.voltage_mv, table);

        if (sample.raw_voltage_mv < VBAT_PRESENT_THRESHOLD) {
            sample.status = BATTERY_STATUS_NO_BATTERY;
            sample.raw_pct = 0;
        } else if (sample.usb_stable || sample.charge_active) {
            sample.status = BATTERY_STATUS_CHARGING;
        } else if (usb_input.confirmed && charge_input.confirmed && !external_power) {
            sample.status = BATTERY_STATUS_NOT_CONNECT;
        }
    } else {
        filter_valid = false;
        LISA_LOGW(TAG, "Battery ADC sample invalid: raw=%u", sample.adc_raw);
    }

    /* CHRG high alone also means no battery, fault or no input power. STDBY is
     * not wired on v2.1, so only report probable completion with all evidence. */
    bool full_candidate = sample.valid && sample.usb_raw && sample.usb_stable &&
                          sample.charge_valid && charge_input.confirmed && !charging_raw &&
                          !sample.charge_active && sample.status == BATTERY_STATUS_CHARGING &&
                          voltage_to_percentage_by_table(sample.raw_voltage_mv, battery_voltage_table_charge) >= 98 &&
                          voltage_to_percentage_by_table(sample.voltage_mv, battery_voltage_table_charge) >= 98;
    if (!full_candidate) {
        full_pending = false;
    } else if (!full_pending) {
        full_pending = true;
        full_since_ms = sample.sampled_at_ms;
    } else if ((uint32_t)(sample.sampled_at_ms - full_since_ms) >= CHARGE_DONE_CONFIRM_MS) {
        sample.status = BATTERY_STATUS_CHARGE_DONE;
        sample.raw_pct = 100;
    }
    if (external_power && sample.status != BATTERY_STATUS_CHARGE_DONE && sample.raw_pct == 100) {
        sample.raw_pct = 99;
    }

    bool low_voltage = sample.valid && sample.charge_valid && !external_power &&
                       sample.raw_voltage_mv >= VBAT_PRESENT_THRESHOLD &&
                       sample.raw_voltage_mv < battery_voltage_table_discharge[0];
    if (!low_voltage) {
        low_pending = false;
        shutdown_requested = false;
    } else if (!low_pending) {
        low_pending = true;
        low_since_ms = sample.sampled_at_ms;
    }

    taskENTER_CRITICAL();
    latest_sample = sample;
    sample_available = true;
    taskEXIT_CRITICAL();

    /* A timer must never execute the blocking shutdown flow itself. */
    if (low_pending && !shutdown_requested &&
        (uint32_t)(sample.sampled_at_ms - low_since_ms) >= LOW_VOLTAGE_SHUTDOWN_CONFIRM_MS) {
        shutdown_requested = power_request_shutdown();
    }
    LISA_LOGD(TAG, "sample: valid=%d adc=%u mv=%u filtered=%u usb=%d/%d chrg=%d/%d pct=%u status=%d",
              sample.valid, sample.adc_raw, sample.raw_voltage_mv, sample.voltage_mv,
              sample.usb_raw, sample.usb_stable, sample.charger_level, sample.charge_active,
              sample.raw_pct, sample.status);
}
