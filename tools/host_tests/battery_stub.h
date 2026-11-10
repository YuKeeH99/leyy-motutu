#pragma once

/* Only hardware, time, OS and event boundaries are replaced. The tests link the
 * production battery.c and battery_ui.c unchanged. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pinmux.h"

#define __psram_bss__
#define __psram_data__
#define LISA_LOGD(...) ((void)0)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define CONFIG_BATTERY_COLLECTION_ADC_PAD "gpiob"
#define CONFIG_BATTERY_COLLECTION_ADC_CHANNEL 3
#define LISA_DEVICE_OK 0
#define LISA_ADC_VALUE_INVALID 0xFFFFU
#define LISA_ADC_REF_VDD_3V6 1
#define LISA_ADC_RESOLUTION_10BIT 10
#define LISA_ADC_RAW_TO_MV(raw, ref, bits) (((uint32_t)(raw) * (ref)) / (1UL << (bits)))
#define LISA_GPIO_HIGH 1
#define LISA_GPIO_LOW 0
#define LISA_GPIO_CONFIG_INPUT_PULLUP 1
#define LISA_GPIO_CONFIG_INPUT_PULLDOWN 2
#define VOICE_MSG_POWER_BATTERY_UPDATE 1

typedef struct { int id; } lisa_device_t;
typedef struct { int reference; int resolution; } lisa_adc_channel_config_t;
struct lisa_timer;
typedef void (*lisa_timercb_t)(struct lisa_timer *timer);
typedef struct lisa_timer { lisa_timercb_t cb; } lisa_timer_t;
typedef enum {
    VOICE_MSG_BATTERY_STATUS_NO_BATTERY,
    VOICE_MSG_BATTERY_STATUS_NOT_CONNECT,
    VOICE_MSG_BATTERY_STATUS_CHARGING,
    VOICE_MSG_BATTERY_STATUS_CHARGE_DONE,
    VOICE_MSG_BATTERY_STATUS_UNKNOWN,
} voice_msg_battery_status_t;
typedef struct { uint8_t level; uint8_t status; } voice_msg_battery_info_t;

lisa_device_t *lisa_device_get(const char *name);
bool lisa_device_ready(lisa_device_t *device);
int lisa_adc_channel_setup(lisa_device_t *device, uint32_t channel,
                           const lisa_adc_channel_config_t *config);
int lisa_adc_read(lisa_device_t *device, uint32_t channel, uint16_t *value);
int lisa_gpio_configure(lisa_device_t *device, uint32_t pin, uint32_t flags);
int lisa_gpio_read_pin(lisa_device_t *device, uint32_t pin);
uint64_t lisa_os_get_tick_ms(void);
bool power_is_usb_plugged(void);
void power_shutdown(void);
bool power_request_shutdown(void);
lisa_timer_t *lisa_timer_create(uint32_t period, lisa_timercb_t callback, void *arg);
int lisa_timer_start(lisa_timer_t *timer);
int lisa_kv_get_int(const char *key, int *value);
int lisa_kv_set_int(const char *key, int value);
int voice_msg_pub(int event, const void *message, size_t size);
