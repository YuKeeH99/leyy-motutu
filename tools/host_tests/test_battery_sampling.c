#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "battery_stub.h"
#include "battery.h"
#include "battery_ui.h"

static lisa_device_t adc = {1}, gpio = {2};
static lisa_timer_t sample_timer;
static uint64_t now_ms;
static uint16_t adc_value = 386; /* 3392mV: below 10% even on discharge curve. */
static int adc_error, charger_level = 1;
static bool usb;
static int adc_reads, shutdown_calls, charge_pull;
static voice_msg_battery_info_t published;

lisa_device_t *lisa_device_get(const char *name) { return !strcmp(name, "adc0") ? &adc : &gpio; }
bool lisa_device_ready(lisa_device_t *device) { return device != NULL; }
int lisa_adc_channel_setup(lisa_device_t *device, uint32_t channel, const lisa_adc_channel_config_t *config)
{
    assert(device == &adc && channel == 3);
    assert(config->reference == LISA_ADC_REF_VDD_3V6 && config->resolution == 10);
    return 0;
}
int lisa_adc_read(lisa_device_t *device, uint32_t channel, uint16_t *value)
{
    assert(device == &adc && channel == 3);
    ++adc_reads;
    *value = adc_value;
    return adc_error;
}
int lisa_gpio_configure(lisa_device_t *device, uint32_t pin, uint32_t flags)
{
    assert(device == &gpio && pin == 8);
    charge_pull = (int)flags;
    return 0;
}
int lisa_gpio_read_pin(lisa_device_t *device, uint32_t pin)
{
    assert(device == &gpio && pin == 8);
    return charger_level;
}
uint64_t lisa_os_get_tick_ms(void) { return now_ms; }
bool power_is_usb_plugged(void) { return usb; }
void power_shutdown(void) { ++shutdown_calls; }
bool power_request_shutdown(void) { ++shutdown_calls; return true; }
lisa_timer_t *lisa_timer_create(uint32_t period, lisa_timercb_t callback, void *arg)
{
    assert(period == 1000 && arg == NULL);
    sample_timer.cb = callback;
    return &sample_timer;
}
int lisa_timer_start(lisa_timer_t *timer) { assert(timer == &sample_timer); return 0; }
int lisa_kv_get_int(const char *key, int *value)
{
    assert(!strcmp(key, "bat_ui_pct"));
    *value = 90; /* The battery was discharged/swapped while firmware was off. */
    return 0;
}
int lisa_kv_set_int(const char *key, int value) { (void)key; (void)value; return 0; }
int voice_msg_pub(int event, const void *message, size_t size)
{
    assert(event == VOICE_MSG_POWER_BATTERY_UPDATE && size == sizeof(published));
    memcpy(&published, message, size);
    return 0;
}
static void sample_at(uint64_t time_ms)
{
    now_ms = time_ms;
    assert(sample_timer.cb);
    sample_timer.cb(&sample_timer);
}
static void seconds(unsigned end)
{
    for (unsigned second = 0; second <= end; ++second) sample_at(second * 1000U);
}
static void query_many_times(void)
{
    voice_msg_battery_info_t info;
    for (unsigned i = 0; i < 30; ++i) {
        (void)battery_get_voltage_mv();
        (void)battery_get_pct_raw();
        (void)battery_get_status();
        (void)battery_usb_plugged_stable_get();
        (void)battery_ui_get_info(&info);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    voice_msg_battery_info_t info;
    if (!strcmp(scenario, "startup")) {
        usb = true; charger_level = 0;
        battery_init();
        assert(!battery_ui_get_info(&info)); /* KV90 must never become measurement. */
        seconds(4);
        assert(battery_ui_get_info(&info));
        assert(info.level <= 15 && info.status == VOICE_MSG_BATTERY_STATUS_CHARGING);
    } else if (!strcmp(scenario, "polarity")) {
        adc_value = 455;
        battery_init(); seconds(6);
        assert(battery_get_status() == BATTERY_STATUS_NOT_CONNECT);
        assert(charge_pull == LISA_GPIO_CONFIG_INPUT_PULLUP);
        charger_level = 0;
        for (unsigned i = 7; i <= 11; ++i) sample_at(i * 1000U);
        assert(battery_get_status() == BATTERY_STATUS_CHARGING);
    } else if (!strcmp(scenario, "queries")) {
        usb = true; charger_level = 0;
        battery_init(); sample_at(0);
        int reads = adc_reads;
        assert(reads == 1); /* Status and percentage must share one ADC conversion. */
        query_many_times();
        assert(adc_reads == reads); /* Getters cannot sample, advance time or trigger shutdown. */
        assert(!battery_usb_plugged_stable_get());
        for (unsigned i = 0; i < 20; ++i) sample_at(100);
        assert(!battery_usb_plugged_stable_get());
        sample_at(2999);
        assert(!battery_usb_plugged_stable_get());
        sample_at(3000);
        assert(battery_usb_plugged_stable_get());
    } else if (!strcmp(scenario, "charging_high")) {
        usb = true; charger_level = 0; adc_value = 478; /* 4200mV during CV charging. */
        battery_init(); seconds(60);
        assert(battery_get_status() == BATTERY_STATUS_CHARGING);
        assert(battery_get_pct_raw() < 100);
    } else if (!strcmp(scenario, "full_confirm")) {
        usb = true; adc_value = 478;
        battery_init(); seconds(10);
        assert(battery_get_status() != BATTERY_STATUS_CHARGE_DONE);
        for (unsigned i = 11; i <= 40; ++i) sample_at(i * 1000U);
        assert(battery_get_status() == BATTERY_STATUS_CHARGE_DONE);
        charger_level = 0; sample_at(41000);
        assert(battery_get_status() != BATTERY_STATUS_CHARGE_DONE);
    } else if (!strcmp(scenario, "adc_error")) {
        adc_error = -1;
        battery_init(); seconds(10); query_many_times();
        assert(shutdown_calls == 0);
        assert(battery_get_status() == BATTERY_STATUS_UNKNOWN);
        assert(!battery_ui_get_info(&info));
        adc_error = 0; sample_at(11000);
        assert(battery_ui_get_info(&info) && info.level <= 15);
    } else if (!strcmp(scenario, "invalid_raw")) {
        adc_value = 0xFFFF;
        battery_init(); seconds(10);
        assert(!battery_ui_get_info(&info));
        assert(shutdown_calls == 0);
    } else if (!strcmp(scenario, "low_shutdown")) {
        adc_value = 330; /* 2900mV, physically present but below discharge cutoff. */
        battery_init(); sample_at(0); query_many_times();
        assert(shutdown_calls == 0);
        sample_at(4999); assert(shutdown_calls == 0);
        sample_at(5000); assert(shutdown_calls == 1);
        sample_at(6000); assert(shutdown_calls == 1);
    } else if (!strcmp(scenario, "usb_stops_shutdown")) {
        adc_value = 330;
        battery_init(); seconds(4);
        usb = true; sample_at(5000);
        assert(shutdown_calls == 0); /* Raw USB must cancel protection before debounce settles. */
    } else if (!strcmp(scenario, "low_ui")) {
        adc_value = 455;
        battery_init(); seconds(10);
        adc_value = 386;
        for (unsigned i = 11; i <= 20; ++i) sample_at(i * 1000U);
        assert(battery_ui_get_info(&info) && info.level <= 15);
    } else if (!strcmp(scenario, "plug_in_preserves_measured_low")) {
        battery_init(); seconds(10);
        assert(battery_ui_get_info(&info) && info.level <= 15);
        usb = true; charger_level = 0; adc_value = 473; /* Charging terminal voltage rises abruptly. */
        for (unsigned i = 11; i <= 15; ++i) sample_at(i * 1000U);
        assert(battery_ui_get_info(&info) && info.status == VOICE_MSG_BATTERY_STATUS_CHARGING);
        assert(info.level <= 15); /* Debouncing USB cannot erase this boot's valid low estimate. */
    } else if (!strcmp(scenario, "diagnostics")) {
        usb = true; charger_level = 0;
        battery_init(); seconds(4);
        battery_snapshot_t snapshot;
        assert(battery_get_snapshot(&snapshot));
        assert(snapshot.valid && snapshot.sampled_at_ms == 4000);
        assert(snapshot.adc_raw == 386 && snapshot.raw_voltage_mv == 3392 && snapshot.voltage_mv == 3392);
        assert(snapshot.usb_raw && snapshot.usb_stable && snapshot.charge_valid && snapshot.charge_active);
        assert(snapshot.charger_level == 0 && snapshot.raw_pct == 3 && snapshot.status == BATTERY_STATUS_CHARGING);
        assert(battery_ui_get_info(&info) && info.level == 3);
        assert(published.level == 3 && published.status == VOICE_MSG_BATTERY_STATUS_CHARGING);
        adc_value = 478;
        query_many_times();
        assert(battery_get_voltage_mv() == 3392 && battery_get_pct_raw() == 3);
        adc_error = -1; sample_at(5000);
        assert(battery_get_snapshot(&snapshot) && !snapshot.valid);
        assert(snapshot.status == BATTERY_STATUS_UNKNOWN && battery_get_voltage_mv() == 0);
        assert(!battery_ui_get_info(&info));
        assert(published.status == VOICE_MSG_BATTERY_STATUS_UNKNOWN);
    } else if (!strcmp(scenario, "full_interrupted")) {
        usb = true; adc_value = 478;
        battery_init(); seconds(25);
        adc_error = -1; sample_at(26000);
        adc_error = 0;
        for (unsigned i = 27; i <= 56; ++i) sample_at(i * 1000U);
        assert(battery_get_status() != BATTERY_STATUS_CHARGE_DONE);
        sample_at(57000);
        assert(battery_get_status() == BATTERY_STATUS_CHARGE_DONE);
        usb = false; sample_at(58000);
        assert(battery_get_status() != BATTERY_STATUS_CHARGE_DONE);
    } else if (!strcmp(scenario, "low_error_resets")) {
        adc_value = 330;
        battery_init(); seconds(4);
        adc_error = -1; sample_at(5000);
        assert(shutdown_calls == 0);
        adc_error = 0; sample_at(6000); sample_at(10999);
        assert(shutdown_calls == 0);
        sample_at(11000);
        assert(shutdown_calls == 1);
    } else if (!strcmp(scenario, "charger_error")) {
        usb = true; adc_value = 478; charger_level = -1;
        battery_init(); seconds(60);
        assert(battery_get_status() == BATTERY_STATUS_CHARGING);
        assert(battery_get_pct_raw() == 99);
        battery_snapshot_t snapshot;
        assert(battery_get_snapshot(&snapshot) && snapshot.valid && !snapshot.charge_valid);
        assert(snapshot.charger_level == -1 && !snapshot.charge_active);
    } else if (!strcmp(scenario, "no_battery")) {
        adc_value = 0;
        battery_init(); seconds(10);
        assert(battery_get_status() == BATTERY_STATUS_NO_BATTERY);
        assert(shutdown_calls == 0);
    } else if (!strcmp(scenario, "overvoltage")) {
        usb = true; adc_value = 600; /* Valid 10-bit number but impossible cell voltage. */
        battery_init(); seconds(10);
        assert(!battery_ui_get_info(&info) && battery_get_status() == BATTERY_STATUS_UNKNOWN);
        assert(shutdown_calls == 0);
    } else if (!strcmp(scenario, "clock_wrap")) {
        usb = true; charger_level = 0;
        battery_init(); sample_at(UINT32_MAX - 999U);
        sample_at(UINT32_MAX); assert(!battery_usb_plugged_stable_get());
        sample_at((uint64_t)UINT32_MAX + 2000U);
        assert(!battery_usb_plugged_stable_get());
        sample_at((uint64_t)UINT32_MAX + 2001U);
        assert(battery_usb_plugged_stable_get());
    } else {
        assert(!"unknown scenario");
    }
    printf("PASS battery %s\n", scenario);
    return 0;
}
