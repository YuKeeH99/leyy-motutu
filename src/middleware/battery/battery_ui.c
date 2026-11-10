/* Battery UI: one timer owns hardware sampling, filtering and publication. */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "arcs_ap.h"
#include "lisa_log.h"
#include "lisa_timer.h"
#include "voice_msg.h"
#include "sysutils.h"
#include "FreeRTOS.h"
#include "task.h"
#include "battery.h"
#include "battery_ui.h"

#define TAG "battery_ui"
#define VBAT_SAMPLE_PERIOD_MS 1000U
#define PERCENTAGE_DEBOUNCE_MS 3000U
#define PERCENTAGE_JUMP_CONFIRM_MS 10000U
#define PERCENTAGE_JUMP_THRESHOLD 8
#define PERCENTAGE_JUMP_TOLERANCE 2
#define LOW_PERCENTAGE_DISPLAY_LIMIT 15

static lisa_timer_t *battery_ui_timer __psram_bss__;
static uint8_t output_pct __psram_data__ = 0xFF;
static uint8_t target_pct __psram_data__ = 0xFF;
static uint8_t candidate_pct __psram_data__ = 0xFF;
static uint32_t candidate_since_ms __psram_bss__;
static uint32_t last_step_ms __psram_bss__;
static voice_msg_battery_info_t last_info __psram_bss__;
static bool last_info_valid __psram_bss__;

static uint8_t percentage_filter(uint8_t percentage, uint32_t now_ms)
{
    /* A previous boot's percentage cannot establish the current battery's SOC.
     * Low readings also cannot remain hidden behind a long downward animation. */
    if (output_pct == 0xFF || (percentage <= LOW_PERCENTAGE_DISPLAY_LIMIT && percentage < output_pct)) {
        output_pct = percentage;
        target_pct = percentage;
        candidate_pct = 0xFF;
        last_step_ms = now_ms;
        return output_pct;
    }

    int difference = abs((int)percentage - target_pct);
    if (difference == 0) {
        candidate_pct = 0xFF;
    } else if (difference > 1 && difference < PERCENTAGE_JUMP_THRESHOLD) {
        target_pct = percentage;
        candidate_pct = 0xFF;
    } else {
        int tolerance = difference >= PERCENTAGE_JUMP_THRESHOLD ? PERCENTAGE_JUMP_TOLERANCE : 0;
        if (candidate_pct == 0xFF || abs((int)percentage - candidate_pct) > tolerance) {
            candidate_pct = percentage;
            candidate_since_ms = now_ms;
        } else {
            uint32_t confirm_ms = difference >= PERCENTAGE_JUMP_THRESHOLD ?
                                      PERCENTAGE_JUMP_CONFIRM_MS : PERCENTAGE_DEBOUNCE_MS;
            if ((uint32_t)(now_ms - candidate_since_ms) >= confirm_ms) {
                target_pct = percentage;
                candidate_pct = 0xFF;
            }
        }
    }
    if ((uint32_t)(now_ms - last_step_ms) >= VBAT_SAMPLE_PERIOD_MS) {
        if (output_pct < target_pct) {
            output_pct++;
        } else if (output_pct > target_pct) {
            output_pct--;
        }
        last_step_ms = now_ms;
    }
    return output_pct;
}

static voice_msg_battery_status_t battery_status_to_msg(battery_status_t status)
{
    switch (status) {
    case BATTERY_STATUS_NO_BATTERY: return VOICE_MSG_BATTERY_STATUS_NO_BATTERY;
    case BATTERY_STATUS_NOT_CONNECT: return VOICE_MSG_BATTERY_STATUS_NOT_CONNECT;
    case BATTERY_STATUS_CHARGING: return VOICE_MSG_BATTERY_STATUS_CHARGING;
    case BATTERY_STATUS_CHARGE_DONE: return VOICE_MSG_BATTERY_STATUS_CHARGE_DONE;
    default: return VOICE_MSG_BATTERY_STATUS_UNKNOWN;
    }
}

bool battery_ui_get_info(voice_msg_battery_info_t *info)
{
    if (!info) {
        return false;
    }
    taskENTER_CRITICAL();
    *info = last_info;
    bool valid = last_info_valid;
    taskEXIT_CRITICAL();
    if (!valid) {
        info->level = 0;
        info->status = VOICE_MSG_BATTERY_STATUS_UNKNOWN;
    }
    return valid;
}

static void battery_voltage_sample_cb(struct lisa_timer *timer)
{
    (void)timer;
    battery_sample_update();
    battery_snapshot_t sample;
    bool valid = battery_get_snapshot(&sample) && sample.valid && sample.status != BATTERY_STATUS_UNKNOWN;
#ifdef CONFIG_LISA_CH32V003_ADC
    (void)battery_get_temp_adc_raw();
#endif
    voice_msg_battery_info_t info = {
        .level = 0,
        .status = VOICE_MSG_BATTERY_STATUS_UNKNOWN,
    };
    if (valid) {
        info.level = percentage_filter(sample.raw_pct, sample.sampled_at_ms);
        info.status = battery_status_to_msg(sample.status);
        if (sample.status == BATTERY_STATUS_NO_BATTERY || sample.status == BATTERY_STATUS_CHARGE_DONE) {
            output_pct = target_pct = info.level = sample.raw_pct;
            candidate_pct = 0xFF;
        } else if (sample.status == BATTERY_STATUS_CHARGING && info.level == 100) {
            output_pct = target_pct = info.level = 99;
        }
    } else if (!sample.valid) {
        /* ADC recovery starts from the next real measurement. USB debounce,
         * however, must retain this boot's measured estimate across the voltage
         * rise caused by connecting a charger. */
        output_pct = target_pct = candidate_pct = 0xFF;
    } else {
        candidate_pct = 0xFF;
    }

    taskENTER_CRITICAL();
    bool changed = info.level != last_info.level || info.status != last_info.status || valid != last_info_valid;
    last_info = info;
    last_info_valid = valid;
    taskEXIT_CRITICAL();
    if (changed) {
        voice_msg_pub(VOICE_MSG_POWER_BATTERY_UPDATE, &info, sizeof(info));
    }
    LISA_LOGD(TAG, "Battery: valid=%d sampled=%u display=%u status=%u", valid, sample.raw_pct, info.level, info.status);
    lisa_timer_start(battery_ui_timer);
}

void battery_ui_init(void)
{
    if (battery_ui_timer) {
        return;
    }
    battery_ui_timer = lisa_timer_create(VBAT_SAMPLE_PERIOD_MS, battery_voltage_sample_cb, NULL);
    if (battery_ui_timer) {
        lisa_timer_start(battery_ui_timer);
    } else {
        LISA_LOGE(TAG, "Failed to create battery UI timer");
    }
}
