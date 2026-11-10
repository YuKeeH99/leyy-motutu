#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "battery/battery.h"
#include "battery/battery_ui.h"
#include "lisa_device.h"
#include "lisa_gpio.h"
#include "board.h"
#include "pa_manager.h"
#include "voice_player_comm.h"
#include "lisa_i2c.h"
#include "lisa_thread.h"
#include "app_netcfg_portal.h"
#include "service_v2_actuators.h"
#include "service_v2_gait.h"
#include "service_v2_imu_interaction.h"
#include "service_v2_low_battery.h"
#include "service_v2_nfc_peer.h"
#include "service_v2_sensors.h"
#include "service_v2_voice_standby.h"
#include "service_voice_action.h"
#include "service_volume.h"
#include "shell.h"
#include "sys_network_manager.h"
#include "sys_wifi.h"
#include "ota_manager.h"

#define V2_SERVO_TEST_DEFAULT_HOLD_MS 500U
#define V2_SERVO_TEST_MAX_HOLD_MS     2000U
#define V2_SENSOR_I2C_BUS             0U
#define V2_STANDBY_ENTER_TIMEOUT_MS 20000U
#define V2_STANDBY_POLL_MS            50U

#if SHELL_PARAMETER_MAX_NUMBER < 14
#error "v2hw motion frame requires SHELL_PARAMETER_MAX_NUMBER >= 14"
#endif

static bool v2_parse_u32(const char *text, uint32_t min_value, uint32_t max_value, uint32_t *value)
{
    char *end = NULL;

    if (text == NULL || value == NULL || *text == '\0') {
        return false;
    }

    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min_value || parsed > max_value) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static void v2_shell_help(void)
{
    shellPrint(shellGetCurrent(), "v2hw status\n"
                                  "v2hw power\n"
                                  "v2hw audio-status\n"
                                  "v2hw gait music\n"
                                  "v2hw selftest\n"
                                  "v2hw touch\n"
                                  "v2hw touch pa13|all\n"
                                  "v2hw touch watch [duration_ms]\n"
                                  "v2hw touch watch pa13|all [duration_ms]\n"
                                  "v2hw mpu\n"
                                  "v2hw interaction status\n"
                                  "v2hw battery-alert status|clear\n"
                                  "v2hw battery-alert simulate <0..100> <idle|active|sleep>\n"
                                  "v2hw standby enter|status\n"
                                  "v2hw i2c scan <0|1>\n"
                                  "v2hw i2c status <0|1>\n"
                                  "v2hw i2c probe <0|1> <7-bit-address>\n"
                                  "v2hw nfc info\n"
                                  "v2hw nfc scan [timeout_ms]\n"
                                  "v2hw nfc card-read [timeout_ms]\n"
                                  "v2hw nfc target [timeout_ms]\n"
                                  "v2hw nfc peer start|stop|status\n"
                                  "v2hw nfc peer once reader|target\n"
                                  "v2hw rgb off|test\n"
                                  "v2hw rgb set <red> <green> <blue>\n"
                                  "v2hw rgb strip <count> <red> <green> <blue> <brightness_percent>\n"
                                  "v2hw rgb count <1..64>\n"
                                  "v2hw rgb status|release\n"
                                  "v2hw rgb calibration status|reset\n"
                                  "v2hw rgb calibration set <RGB|RBG|GRB|GBR|BRG|BGR> "
                                  "<r_gain> <g_gain> <b_gain> <gamma_x100>\n"
                                  "v2hw rgb level <0|1>\n"
                                  "v2hw rgb gpio-level <0|1>\n"
                                  "v2hw rgb clock\n"
                                  "v2hw rgb scope <0|1|stop>\n"
                                  "v2hw servo all-off\n"
                                  "v2hw servo frame <duration_ms> <linear|smooth|ease-in|ease-out> "
                                  "<us0> <us1> <us2> <us3>\n"
                                  "v2hw servo state|stop\n"
                                  "v2hw servo watchdog <0|200..5000>\n"
                                  "v2hw servo test <0..3> [pulse_us] [hold_ms]\n"
                                  "v2hw servo sweep <0..3> [period_ms]\n"
                                  "v2hw servo sweep-stop|sweep-status\n"
                                  "v2hw motion frame <duration_ms> <easing> <us0> <us1> <us2> <us3> "
                                  "<count> <red> <green> <blue> <brightness_percent>\n"
                                  "v2hw vol\n"
                                  "v2hw vol set <0..100>\n"
                                  "v2hw vol mute|unmute\n"
                                  "v2hw netcfg start|stop|status\n"
                                  "v2hw gait stretch|pushup|backflip|assembly|standby-fold|stand|prone|flip-rise|flip|roll|full-roll|kick|serve|tennis-serve|off|fwd|back|runfwd|runback|left|right|stop "
                                  "[cycles; roll counts 180-degree half-turns]\n"
                                  "v2hw gait status\n"
                                  "v2hw gait say <utterance>\n"
                                  "v2hw gait code <cloud-code>\n");
}

static int v2_shell_standby_print_status(void)
{
    service_v2_servo_status_t servo = {0};
    service_v2_voice_standby_state_t state = service_v2_voice_standby_get_state();
    int servo_ret = service_v2_actuators_servo_get_status(&servo);

    shellPrint(shellGetCurrent(),
               "standby state=%s servo_status=%d servo_enabled=0x%02x wifi_radio=%s radio_blocked=%u servo_supply=shared-vcc\n",
               service_v2_voice_standby_state_name(state), servo_ret,
               servo_ret == 0 ? servo.enabled_mask : 0xffU,
               sys_wifi_radio_is_off() ? "off" : "on", sys_wifi_radio_suspended() ? 1U : 0U);
    return servo_ret;
}

static int v2_shell_standby(int argc, char **argv)
{
    if (argc != 3) {
        return -EINVAL;
    }

    if (strcmp(argv[2], "status") == 0) {
        return v2_shell_standby_print_status();
    }
    if (strcmp(argv[2], "enter") != 0) {
        return -EINVAL;
    }

    service_v2_voice_standby_state_t state = service_v2_voice_standby_get_state();
    if (state == SERVICE_V2_VOICE_STANDBY_ASLEEP) {
        shellPrint(shellGetCurrent(), "standby request=already-asleep\n");
        return v2_shell_standby_print_status();
    }

    int ret = service_v2_voice_standby_request();
    if (ret != 0) {
        shellPrint(shellGetCurrent(), "standby request=failed error=%d\n", ret);
        return ret;
    }
    shellPrint(shellGetCurrent(), "standby request=accepted\n");

    bool transition_started = false;
    for (uint32_t waited_ms = 0U; waited_ms < V2_STANDBY_ENTER_TIMEOUT_MS;
         waited_ms += V2_STANDBY_POLL_MS) {
        state = service_v2_voice_standby_get_state();
        if (state != SERVICE_V2_VOICE_STANDBY_ACTIVE) {
            transition_started = true;
        }
        if (state == SERVICE_V2_VOICE_STANDBY_ASLEEP) {
            ret = v2_shell_standby_print_status();
            return ret;
        }
        if (transition_started && state == SERVICE_V2_VOICE_STANDBY_ACTIVE) {
            shellPrint(shellGetCurrent(), "standby request=aborted\n");
            (void)v2_shell_standby_print_status();
            return -EIO;
        }
        vTaskDelay(pdMS_TO_TICKS(V2_STANDBY_POLL_MS));
    }

    shellPrint(shellGetCurrent(), "standby request=timeout waited_ms=%u\n",
               (unsigned)V2_STANDBY_ENTER_TIMEOUT_MS);
    (void)v2_shell_standby_print_status();
    return -ETIMEDOUT;
}

static void v2_shell_print_volume(void)
{
    shellPrint(shellGetCurrent(), "volume=%d min=%d muted=%s\n", service_volume_get(), service_volume_min(),
               service_volume_is_muted() ? "yes" : "no");
}

static int v2_shell_volume(int argc, char *argv[])
{
    if (argc == 2) {
        v2_shell_print_volume();
        return 0;
    }

    if (strcmp(argv[2], "mute") == 0 && argc == 3) {
        service_volume_mute(true);
        v2_shell_print_volume();
        return 0;
    }

    if (strcmp(argv[2], "unmute") == 0 && argc == 3) {
        service_volume_mute(false);
        v2_shell_print_volume();
        return 0;
    }

    if (strcmp(argv[2], "set") == 0 && argc == 4) {
        uint32_t level = 0U;
        if (!v2_parse_u32(argv[3], 0U, 100U, &level)) {
            shellPrint(shellGetCurrent(), "invalid volume: %s (0..100)\n", argv[3]);
            return -EINVAL;
        }
        service_volume_set((int)level);
        v2_shell_print_volume();
        return 0;
    }

    v2_shell_help();
    return -EINVAL;
}

static int v2_shell_touch_channel(service_v2_touch_channel_t channel, const char *pad)
{
    bool touched = false;
    int ret = service_v2_touch_init();
    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_touch_read_channel(channel, &touched);
    }

    shellPrint(shellGetCurrent(), "touch %s: status=%d (%s) level=%u\n", pad, ret, service_v2_sensors_strerror(ret),
               touched ? 1U : 0U);
    return ret;
}

static int v2_shell_touch(void)
{
    return v2_shell_touch_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, "PA09");
}

static int v2_shell_touch_all(void)
{
    int pa09_ret = v2_shell_touch_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, "PA09");
    int pa13_ret = v2_shell_touch_channel(SERVICE_V2_TOUCH_CHANNEL_PA13, "PA13");
    return pa09_ret != SERVICE_V2_SENSORS_OK ? pa09_ret : pa13_ret;
}

static const char *v2_battery_status_name(voice_msg_battery_status_t status)
{
    switch (status) {
    case VOICE_MSG_BATTERY_STATUS_NO_BATTERY:
        return "no-battery";
    case VOICE_MSG_BATTERY_STATUS_NOT_CONNECT:
        return "not-charging";
    case VOICE_MSG_BATTERY_STATUS_CHARGING:
        return "charging";
    case VOICE_MSG_BATTERY_STATUS_CHARGE_DONE:
        return "charged";
    case VOICE_MSG_BATTERY_STATUS_UNKNOWN:
    default:
        return "unknown";
    }
}

static int v2_shell_audio_status(void)
{
    lisa_device_t *pa = lisa_device_get(CONFIG_PA_DEVICE_NAME);
    int level = pa ? lisa_gpio_read_pin(pa, PA_EN_PIN) : -1;
    shellPrint(shellGetCurrent(),
               "audio pa_on=%d refs=%d pin=%s:%u level=%d "
               "tone=%d tts=%d music=%d alert=%d beat=%d active=%d\n",
               pa_manager_get_state(), pa_manager_get_ref_count(),
               CONFIG_PA_DEVICE_NAME, (unsigned)PA_EN_PIN, level,
               tone_player ? (int)app_player_get_state(tone_player) : -1,
               tts_player ? (int)app_player_get_state(tts_player) : -1,
               music_player ? (int)app_player_get_state(music_player) : -1,
               alert_player ? (int)app_player_get_state(alert_player) : -1,
               beat_player ? (int)app_player_get_state(beat_player) : -1,
               voice_player_is_audio_active());
    return 0;
}

static int v2_shell_power(void)
{
    voice_msg_battery_info_t info = {0};
    uint16_t voltage_mv = battery_get_voltage_mv();
    battery_snapshot_t sample = {0};
    bool sampled = battery_get_snapshot(&sample);

    shellPrint(shellGetCurrent(),
               "battery sample=%u valid=%u at_ms=%u adc=%u raw_mv=%u filtered_mv=%u "
               "usb_raw=%u usb_stable=%u chrg_level=%d chrg_valid=%u charging=%u raw_pct=%u\n",
               sampled, sample.valid, (unsigned)sample.sampled_at_ms, sample.adc_raw,
               sample.raw_voltage_mv, sample.voltage_mv, sample.usb_raw, sample.usb_stable,
               sample.charger_level, sample.charge_valid, sample.charge_active, sample.raw_pct);

    if (!battery_ui_get_info(&info)) {
        return -EIO;
    }

    if (info.status == VOICE_MSG_BATTERY_STATUS_NO_BATTERY) {
        shellPrint(shellGetCurrent(), "VBAT/servo pin2=%umV level=n/a battery=%s\n", voltage_mv,
                   v2_battery_status_name(info.status));
    } else {
        shellPrint(shellGetCurrent(), "VBAT/servo pin2=%umV level=%u%% battery=%s\n", voltage_mv, info.level,
                   v2_battery_status_name(info.status));
    }
    return voltage_mv == 0U ? -EIO : 0;
}

static int v2_shell_touch_watch_channel(service_v2_touch_channel_t channel, const char *pad, uint32_t duration_ms)
{
    bool touched = false;
    bool previous = false;
    int ret = service_v2_touch_init();
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    ret = service_v2_touch_read_channel(channel, &previous);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (channel == SERVICE_V2_TOUCH_CHANNEL_PA09) {
        shellPrint(shellGetCurrent(), "touch watch: 0ms level=%u\n", previous ? 1U : 0U);
    } else {
        shellPrint(shellGetCurrent(), "touch watch %s: 0ms level=%u\n", pad, previous ? 1U : 0U);
    }

    for (uint32_t elapsed_ms = 50U; elapsed_ms <= duration_ms; elapsed_ms += 50U) {
        lisa_thread_mdelay(50U);
        ret = service_v2_touch_read_channel(channel, &touched);
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
        if (touched != previous) {
            if (channel == SERVICE_V2_TOUCH_CHANNEL_PA09) {
                shellPrint(shellGetCurrent(), "touch watch: %ums level=%u\n", elapsed_ms, touched ? 1U : 0U);
            } else {
                shellPrint(shellGetCurrent(), "touch watch %s: %ums level=%u\n", pad, elapsed_ms, touched ? 1U : 0U);
            }
            previous = touched;
        }
    }

    if (channel == SERVICE_V2_TOUCH_CHANNEL_PA09) {
        shellPrint(shellGetCurrent(), "touch watch done: level=%u\n", previous ? 1U : 0U);
    } else {
        shellPrint(shellGetCurrent(), "touch watch %s done: level=%u\n", pad, previous ? 1U : 0U);
    }
    return 0;
}

static int v2_shell_touch_watch(uint32_t duration_ms)
{
    return v2_shell_touch_watch_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, "PA09", duration_ms);
}

static int v2_shell_touch_watch_all(uint32_t duration_ms)
{
    bool pa09 = false;
    bool pa13 = false;
    bool previous_pa09 = false;
    bool previous_pa13 = false;
    int ret = service_v2_touch_init();
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    ret = service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, &previous_pa09);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    ret = service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA13, &previous_pa13);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    shellPrint(shellGetCurrent(), "touch watch: 0ms PA09=%u PA13=%u\n", previous_pa09 ? 1U : 0U,
               previous_pa13 ? 1U : 0U);

    for (uint32_t elapsed_ms = 50U; elapsed_ms <= duration_ms; elapsed_ms += 50U) {
        lisa_thread_mdelay(50U);
        ret = service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, &pa09);
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
        ret = service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA13, &pa13);
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
        if (pa09 != previous_pa09 || pa13 != previous_pa13) {
            shellPrint(shellGetCurrent(), "touch watch: %ums PA09=%u PA13=%u\n", elapsed_ms, pa09 ? 1U : 0U,
                       pa13 ? 1U : 0U);
            previous_pa09 = pa09;
            previous_pa13 = pa13;
        }
    }

    shellPrint(shellGetCurrent(), "touch watch done: PA09=%u PA13=%u\n", previous_pa09 ? 1U : 0U,
               previous_pa13 ? 1U : 0U);
    return 0;
}

static int v2_shell_require_nfc_peer_stopped(void)
{
    service_v2_nfc_peer_status_t status = {0};
    int ret = service_v2_nfc_peer_get_status(&status);

    if (ret == SERVICE_V2_SENSORS_ERR_NOT_READY ||
        (ret == SERVICE_V2_SENSORS_OK && !status.enabled && status.role == SERVICE_V2_NFC_PEER_ROLE_IDLE)) {
        return SERVICE_V2_SENSORS_OK;
    }
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    shellPrint(shellGetCurrent(), "nfc peer discovery is active or stopping; run "
                                  "'v2hw nfc peer stop' before manual PN532 diagnostics\n");
    return SERVICE_V2_SENSORS_ERR_BUSY;
}

static int v2_shell_i2c_scan(uint32_t bus)
{
    if (bus == V2_SENSOR_I2C_BUS) {
        int peer_ret = v2_shell_require_nfc_peer_stopped();
        if (peer_ret != SERVICE_V2_SENSORS_OK) {
            return peer_ret;
        }
    }

    const char *device_name = bus == 0U ? "i2c0" : "i2c1";
    lisa_device_t *device = lisa_device_get(device_name);
    if (!lisa_device_ready(device)) {
        shellPrint(shellGetCurrent(), "%s is not ready\n", device_name);
        return -ENODEV;
    }

    lisa_i2c_config_t config = LISA_I2C_CONFIG_DEFAULT();
    int ret = lisa_i2c_configure(device, &config);
    if (ret != LISA_DEVICE_OK) {
        shellPrint(shellGetCurrent(), "%s configure failed: %d\n", device_name, ret);
        return ret;
    }

    uint32_t found = 0U;
    shellPrint(shellGetCurrent(), "%s scan:", device_name);
    for (uint32_t address = 0x08U; address <= 0x77U; ++address) {
        lisa_i2c_msg_t message = {
            .addr = (uint16_t)address,
            .flags = LISA_I2C_FLAG_NONE,
            .len = 0U,
            .buf = NULL,
        };
        ret = lisa_i2c_transfer(device, &message, 1U);
        if (ret == LISA_DEVICE_OK) {
            shellPrint(shellGetCurrent(), " 0x%02x", address);
            ++found;
        }
    }
    shellPrint(shellGetCurrent(), " (%u found)\n", found);
    return 0;
}

static void v2_shell_i2c_print_snapshot(const char *device_name, const char *phase,
                                        const lisa_i2c_debug_snapshot_t *snapshot)
{
    shellPrint(shellGetCurrent(),
               "%s %s: raw_status=0x%08x scl=%u sda=%u busbusy=%u "
               "ack=%u cmpl=%u addrhit=%u latched=%u driver_busy=%u "
               "ctrl=0x%08x inten=0x%08x setup=0x%08x\n",
               device_name, phase, snapshot->raw_status, (snapshot->raw_status >> 13U) & 1U,
               (snapshot->raw_status >> 14U) & 1U, (snapshot->raw_status >> 11U) & 1U,
               (snapshot->raw_status >> 10U) & 1U, (snapshot->raw_status >> 9U) & 1U, (snapshot->raw_status >> 3U) & 1U,
               snapshot->address_ack_latched ? 1U : 0U, snapshot->transfer_busy ? 1U : 0U, snapshot->raw_ctrl,
               snapshot->raw_inten, snapshot->raw_setup);
}

static int v2_shell_i2c_get_device(uint32_t bus, const char **device_name, lisa_device_t **device)
{
    *device_name = bus == 0U ? "i2c0" : "i2c1";
    *device = lisa_device_get(*device_name);
    if (!lisa_device_ready(*device)) {
        shellPrint(shellGetCurrent(), "%s is not ready\n", *device_name);
        return -ENODEV;
    }
    return 0;
}

static int v2_shell_i2c_status(uint32_t bus)
{
    const char *device_name;
    lisa_device_t *device;
    int ret = v2_shell_i2c_get_device(bus, &device_name, &device);
    if (ret != 0) {
        return ret;
    }

    lisa_i2c_debug_status_t status = {0};
    ret = lisa_i2c_get_debug_status(device, &status);
    if (ret != LISA_DEVICE_OK) {
        return ret;
    }
    v2_shell_i2c_print_snapshot(device_name, "current", &status.current);
    if (status.last_timeout_valid) {
        v2_shell_i2c_print_snapshot(device_name, "timeout-pre-abort", &status.last_timeout);
    }
    return 0;
}

static int v2_shell_i2c_probe(uint32_t bus, uint32_t address)
{
    if (bus == V2_SENSOR_I2C_BUS) {
        int peer_ret = v2_shell_require_nfc_peer_stopped();
        if (peer_ret != SERVICE_V2_SENSORS_OK) {
            return peer_ret;
        }
    }

    const char *device_name;
    lisa_device_t *device;
    int ret = v2_shell_i2c_get_device(bus, &device_name, &device);
    if (ret != 0) {
        return ret;
    }

    lisa_i2c_config_t config = LISA_I2C_CONFIG_DEFAULT();
    ret = lisa_i2c_configure(device, &config);
    if (ret != LISA_DEVICE_OK) {
        return ret;
    }

    lisa_i2c_debug_status_t status = {0};
    ret = lisa_i2c_get_debug_status(device, &status);
    if (ret != LISA_DEVICE_OK) {
        return ret;
    }
    v2_shell_i2c_print_snapshot(device_name, "before", &status.current);

    uint8_t value = 0xFFU;
    int probe_ret = lisa_i2c_read(device, (uint16_t)address, &value, 1U);
    shellPrint(shellGetCurrent(), "%s read1 0x%02x: status=%d data=0x%02x\n", device_name, address, probe_ret, value);

    memset(&status, 0, sizeof(status));
    ret = lisa_i2c_get_debug_status(device, &status);
    if (ret == LISA_DEVICE_OK) {
        if (status.last_timeout_valid) {
            v2_shell_i2c_print_snapshot(device_name, "timeout-pre-abort", &status.last_timeout);
        }
        v2_shell_i2c_print_snapshot(device_name, "after", &status.current);
    }
    return probe_ret;
}

static int v2_shell_mpu(void)
{
    uint8_t who_am_i = 0U;
    service_v2_mpu6050_raw_t sample = {0};
    int ret = service_v2_mpu6050_init(false);

    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_mpu6050_who_am_i(&who_am_i);
    }
    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_mpu6050_read_raw(&sample);
    }

    shellPrint(shellGetCurrent(),
               "mpu6050 i2c0/0x68: status=%d (%s) who=0x%02x "
               "accel=%d,%d,%d temp=%d gyro=%d,%d,%d\n",
               ret, service_v2_sensors_strerror(ret), who_am_i, sample.accel_x, sample.accel_y, sample.accel_z,
               sample.temperature, sample.gyro_x, sample.gyro_y, sample.gyro_z);
    return ret;
}

static int v2_shell_nfc_info(void)
{
    service_v2_pn532_firmware_t firmware = {0};
    int ret = v2_shell_require_nfc_peer_stopped();
    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_pn532_init();
    }

    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_pn532_get_firmware_version(&firmware);
    }

    shellPrint(shellGetCurrent(),
               "pn532 shared-i2c0/0x24: status=%d (%s) ic=0x%02x fw=%u.%u "
               "support=0x%02x\n",
               ret, service_v2_sensors_strerror(ret), firmware.ic, firmware.version, firmware.revision,
               firmware.support);
    return ret;
}

static int v2_shell_nfc_scan(uint32_t timeout_ms)
{
    service_v2_pn532_target_t target = {0};
    int ret = v2_shell_require_nfc_peer_stopped();
    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_pn532_init();
    }
    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_pn532_list_passive_target(&target, timeout_ms);
    }

    if (ret != SERVICE_V2_SENSORS_OK) {
        shellPrint(shellGetCurrent(), "nfc scan: status=%d (%s)\n", ret, service_v2_sensors_strerror(ret));
        return ret;
    }

    shellPrint(shellGetCurrent(), "nfc target=%u uid=", target.target_number);
    for (uint32_t i = 0U; i < target.uid_len; ++i) {
        shellPrint(shellGetCurrent(), "%02X", target.uid[i]);
    }
    shellPrint(shellGetCurrent(), " sens_res=0x%04x sel_res=0x%02x ats=", target.sens_res, target.sel_res);
    for (uint32_t i = 0U; i < target.ats_len; ++i) {
        shellPrint(shellGetCurrent(), "%02X", target.ats[i]);
    }
    shellPrint(shellGetCurrent(), "\n");
    return 0;
}

static const char *v2_shell_nfc_card_name(const service_v2_pn532_target_t *target)
{
    static const uint8_t card_a_uid[] = {0x08U, 0xA1U, 0x00U, 0x01U};
    static const uint8_t card_b_uid[] = {0x08U, 0xB2U, 0x00U, 0x02U};

    if (target->uid_len == sizeof(card_a_uid) && memcmp(target->uid, card_a_uid, sizeof(card_a_uid)) == 0) {
        return "CARD-A";
    }
    if (target->uid_len == sizeof(card_b_uid) && memcmp(target->uid, card_b_uid, sizeof(card_b_uid)) == 0) {
        return "CARD-B";
    }
    return "UNKNOWN";
}

static void v2_shell_nfc_format_uid(const service_v2_pn532_target_t *target, char uid_text[21])
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t uid_len = target->uid_len;
    if (uid_len > SERVICE_V2_PN532_UID_MAX_LEN) {
        uid_len = SERVICE_V2_PN532_UID_MAX_LEN;
    }

    for (uint32_t i = 0U; i < uid_len; ++i) {
        uid_text[i * 2U] = hex[target->uid[i] >> 4U];
        uid_text[i * 2U + 1U] = hex[target->uid[i] & 0x0FU];
    }
    uid_text[uid_len * 2U] = '\0';
}

/*
 * One machine-readable card timing sample for the upper-computer dashboard.
 * The peer worker is paused only for the bounded scan and is restored before
 * this command returns, so invoking this repeatedly cannot change the board's
 * ID-bound reader/target role.
 */
static int v2_shell_nfc_card_read(uint32_t timeout_ms)
{
    service_v2_nfc_peer_status_t peer_status = {0};
    service_v2_pn532_target_t target = {0};
    bool restore_peer = false;
    int ret = service_v2_nfc_peer_get_status(&peer_status);

    if (ret == SERVICE_V2_SENSORS_OK) {
        restore_peer = peer_status.enabled;
        if (peer_status.enabled) {
            ret = service_v2_nfc_peer_stop();
        } else if (peer_status.role != SERVICE_V2_NFC_PEER_ROLE_IDLE) {
            ret = SERVICE_V2_SENSORS_ERR_BUSY;
        }
    } else if (ret == SERVICE_V2_SENSORS_ERR_NOT_READY) {
        /* Diagnostics can still own PN532 before the peer service starts. */
        ret = SERVICE_V2_SENSORS_OK;
    }

    if (ret != SERVICE_V2_SENSORS_OK) {
        int restore_ret = restore_peer ? service_v2_nfc_peer_start() : SERVICE_V2_SENSORS_OK;
        shellPrint(shellGetCurrent(),
                   "NFC_CARD_SCAN event=error card=NONE uid=NONE "
                   "elapsed_ms=0 scan_status=%d peer_restored=%u "
                   "restore_status=%d\n",
                   ret, restore_peer && restore_ret == SERVICE_V2_SENSORS_OK, restore_ret);
        return ret;
    }

    ret = service_v2_pn532_init();
    if (ret != SERVICE_V2_SENSORS_OK) {
        int restore_ret = restore_peer ? service_v2_nfc_peer_start() : SERVICE_V2_SENSORS_OK;
        shellPrint(shellGetCurrent(),
                   "NFC_CARD_SCAN event=error card=NONE uid=NONE "
                   "elapsed_ms=0 scan_status=%d peer_restored=%u "
                   "restore_status=%d\n",
                   ret, restore_peer && restore_ret == SERVICE_V2_SENSORS_OK, restore_ret);
        return ret;
    }

    shellPrint(shellGetCurrent(), "NFC_CARD_SCAN event=armed timeout_ms=%u\n", timeout_ms);
    TickType_t start_tick = xTaskGetTickCount();
    ret = service_v2_pn532_list_passive_target(&target, timeout_ms);
    uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - start_tick);

    int restore_ret = restore_peer ? service_v2_nfc_peer_start() : SERVICE_V2_SENSORS_OK;
    bool restored = restore_peer && restore_ret == SERVICE_V2_SENSORS_OK;

    if (ret == SERVICE_V2_SENSORS_OK) {
        char uid_text[21] = {0};
        v2_shell_nfc_format_uid(&target, uid_text);
        shellPrint(shellGetCurrent(),
                   "NFC_CARD_SCAN event=found card=%s uid=%s elapsed_ms=%u "
                   "scan_status=0 peer_restored=%u restore_status=%d\n",
                   v2_shell_nfc_card_name(&target), uid_text, elapsed_ms, restored, restore_ret);
    } else {
        const char *event =
            ret == SERVICE_V2_SENSORS_ERR_NO_TARGET || ret == SERVICE_V2_SENSORS_ERR_TIMEOUT ? "timeout" : "error";
        shellPrint(shellGetCurrent(),
                   "NFC_CARD_SCAN event=%s card=NONE uid=NONE elapsed_ms=%u "
                   "scan_status=%d peer_restored=%u restore_status=%d\n",
                   event, elapsed_ms, ret, restored, restore_ret);
    }

    if (restore_ret != SERVICE_V2_SENSORS_OK) {
        return restore_ret;
    }
    /* An empty RF window is an expected monitoring sample, not a shell error. */
    return ret == SERVICE_V2_SENSORS_ERR_NO_TARGET || ret == SERVICE_V2_SENSORS_ERR_TIMEOUT ? SERVICE_V2_SENSORS_OK
                                                                                            : ret;
}

static int v2_shell_nfc_target(uint32_t timeout_ms)
{
    service_v2_pn532_activation_t activation = {0};
    int ret = v2_shell_require_nfc_peer_stopped();
    if (ret == SERVICE_V2_SENSORS_OK) {
        ret = service_v2_pn532_init();
    }
    if (ret != SERVICE_V2_SENSORS_OK) {
        shellPrint(shellGetCurrent(), "nfc target: status=%d (%s)\n", ret, service_v2_sensors_strerror(ret));
        return ret;
    }

    shellPrint(shellGetCurrent(),
               "nfc target waiting: type=ISO14443-4 PICC uid=08123456 "
               "timeout=%ums\n",
               timeout_ms);
    ret = service_v2_pn532_wait_for_type_a_activation(&activation, timeout_ms);
    if (ret != SERVICE_V2_SENSORS_OK) {
        shellPrint(shellGetCurrent(), "nfc target: status=%d (%s)\n", ret, service_v2_sensors_strerror(ret));
        return ret;
    }

    shellPrint(shellGetCurrent(), "nfc target activated: mode=0x%02x cmd_len=%u cmd=", activation.mode,
               activation.initiator_command_len);
    for (uint32_t i = 0U; i < activation.initiator_command_len; ++i) {
        shellPrint(shellGetCurrent(), "%02X", activation.initiator_command[i]);
    }
    shellPrint(shellGetCurrent(), "\n");
    return 0;
}

static void v2_shell_print_peer_id(const uint8_t *id)
{
    for (uint32_t i = 0U; i < SERVICE_V2_NFC_PEER_ID_LEN; ++i) {
        shellPrint(shellGetCurrent(), "%02x", id[i]);
    }
}

static int v2_shell_nfc_peer_status(void)
{
    service_v2_nfc_peer_status_t status = {0};
    int ret = service_v2_nfc_peer_get_status(&status);
    if (ret != SERVICE_V2_SENSORS_OK) {
        shellPrint(shellGetCurrent(), "nfc peer: status=%d (%s)\n", ret, service_v2_sensors_strerror(ret));
        return ret;
    }

    shellPrint(shellGetCurrent(), "nfc peer: enabled=%u role=%s local=", status.enabled ? 1U : 0U,
               service_v2_nfc_peer_role_name(status.role));
    v2_shell_print_peer_id(status.local_id);
    shellPrint(shellGetCurrent(), " present=%u peer=", status.present ? 1U : 0U);
    if (status.present) {
        v2_shell_print_peer_id(status.peer_id);
        shellPrint(shellGetCurrent(), " link=iso14443-4a age=%ums", status.last_seen_age_ms);
    } else {
        shellPrint(shellGetCurrent(), "none age=n/a");
    }
    shellPrint(shellGetCurrent(), " seen=%u enter=%u leave=%u advertised=%u last_error=%d\n", status.seen_count,
               status.enter_count, status.leave_count, status.advertise_window_count, status.last_error);
    return SERVICE_V2_SENSORS_OK;
}

static int v2_shell_nfc_peer(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[3], "once") == 0) {
        service_v2_nfc_peer_role_t role;
        if (strcmp(argv[4], "reader") == 0) {
            role = SERVICE_V2_NFC_PEER_ROLE_READER;
        } else if (strcmp(argv[4], "target") == 0) {
            role = SERVICE_V2_NFC_PEER_ROLE_TARGET;
        } else {
            return -EINVAL;
        }

        int ret = service_v2_nfc_peer_run_once(role);
        shellPrint(shellGetCurrent(), "nfc peer once %s: status=%d (%s)\n", service_v2_nfc_peer_role_name(role), ret,
                   service_v2_sensors_strerror(ret));
        (void)v2_shell_nfc_peer_status();
        return ret;
    }
    if (argc != 4) {
        return -EINVAL;
    }
    if (strcmp(argv[3], "start") == 0) {
        int ret = service_v2_nfc_peer_start();
        if (ret == SERVICE_V2_SENSORS_OK) {
            shellPrint(shellGetCurrent(), "nfc peer discovery started\n");
        }
        return ret;
    }
    if (strcmp(argv[3], "stop") == 0) {
        int ret = service_v2_nfc_peer_stop();
        if (ret == SERVICE_V2_SENSORS_OK) {
            shellPrint(shellGetCurrent(), "nfc peer discovery stopped\n");
        }
        return ret;
    }
    if (strcmp(argv[3], "status") == 0) {
        return v2_shell_nfc_peer_status();
    }
    return -EINVAL;
}

static int v2_shell_selftest(void)
{
    int peer_ret = v2_shell_require_nfc_peer_stopped();
    if (peer_ret != SERVICE_V2_SENSORS_OK) {
        return peer_ret;
    }

    service_v2_sensors_self_test_result_t result = {0};
    int ret = service_v2_sensors_self_test(&result);

    shellPrint(shellGetCurrent(), "touch PA09: %d (%s), level=%u\n", result.touch_status,
               service_v2_sensors_strerror(result.touch_status), result.touch_active ? 1U : 0U);
    shellPrint(shellGetCurrent(), "touch PA13: %d (%s), level=%u\n", result.touch_pa13_status,
               service_v2_sensors_strerror(result.touch_pa13_status), result.touch_pa13_active ? 1U : 0U);
    shellPrint(shellGetCurrent(),
               "mpu6050: %d (%s), who=0x%02x, accel=%d,%d,%d, "
               "gyro=%d,%d,%d\n",
               result.mpu6050_status, service_v2_sensors_strerror(result.mpu6050_status), result.mpu6050_who_am_i,
               result.mpu6050_raw.accel_x, result.mpu6050_raw.accel_y, result.mpu6050_raw.accel_z,
               result.mpu6050_raw.gyro_x, result.mpu6050_raw.gyro_y, result.mpu6050_raw.gyro_z);
    shellPrint(shellGetCurrent(), "pn532: %d (%s), ic=0x%02x fw=%u.%u support=0x%02x\n", result.pn532_status,
               service_v2_sensors_strerror(result.pn532_status), result.pn532_firmware.ic,
               result.pn532_firmware.version, result.pn532_firmware.revision, result.pn532_firmware.support);
    return ret;
}

static const char *v2_rgb_owner_name(service_v2_rgb_owner_t owner)
{
    switch (owner) {
    case SERVICE_V2_RGB_OWNER_MANUAL:
        return "manual";
    case SERVICE_V2_RGB_OWNER_ACTION:
        return "action";
    case SERVICE_V2_RGB_OWNER_SYSTEM:
    default:
        return "system";
    }
}

static int v2_shell_rgb_state(void)
{
    service_v2_rgb_status_t status = {0};
    int ret = service_v2_actuators_rgb_get_status(&status);
    if (ret == 0) {
        shellPrint(shellGetCurrent(), "RGB_STATE count=%u rgb=%u,%u,%u brightness=%u owner=%s\n", status.count,
                   status.red, status.green, status.blue, status.brightness_percent, v2_rgb_owner_name(status.owner));
    }
    return ret;
}

static void v2_shell_rgb_ack(const char *action, int status_code)
{
    service_v2_rgb_status_t status = {0};
    int state_ret = service_v2_actuators_rgb_get_status(&status);
    if (state_ret == 0) {
        shellPrint(shellGetCurrent(), "RGB_ACK action=%s status=%d count=%u rgb=%u,%u,%u brightness=%u owner=%s\n",
                   action, status_code, status.count, status.red, status.green, status.blue, status.brightness_percent,
                   v2_rgb_owner_name(status.owner));
    } else {
        shellPrint(shellGetCurrent(), "RGB_ACK action=%s status=%d state_status=%d\n", action, status_code, state_ret);
    }
}

static bool v2_parse_rgb_order(const char *text, service_v2_rgb_order_t *order)
{
    if (text == NULL || order == NULL) {
        return false;
    }
    if (strcmp(text, "RGB") == 0) {
        *order = SERVICE_V2_RGB_ORDER_RGB;
    } else if (strcmp(text, "RBG") == 0) {
        *order = SERVICE_V2_RGB_ORDER_RBG;
    } else if (strcmp(text, "GRB") == 0) {
        *order = SERVICE_V2_RGB_ORDER_GRB;
    } else if (strcmp(text, "GBR") == 0) {
        *order = SERVICE_V2_RGB_ORDER_GBR;
    } else if (strcmp(text, "BRG") == 0) {
        *order = SERVICE_V2_RGB_ORDER_BRG;
    } else if (strcmp(text, "BGR") == 0) {
        *order = SERVICE_V2_RGB_ORDER_BGR;
    } else {
        return false;
    }
    return true;
}

static int v2_shell_rgb_calibration_print(void)
{
    service_v2_rgb_calibration_t calibration = {0};
    bool persistent = false;
    int ret = service_v2_actuators_rgb_calibration_get(&calibration, &persistent);
    if (ret == 0) {
        shellPrint(
            shellGetCurrent(), "RGB_CAL version=%u order=%s r_gain=%u g_gain=%u b_gain=%u gamma=%u persistent=%s\n",
            (unsigned)calibration.version, service_v2_actuators_rgb_order_name(calibration.order),
            (unsigned)calibration.red_gain_permille, (unsigned)calibration.green_gain_permille,
            (unsigned)calibration.blue_gain_permille, (unsigned)calibration.gamma_x100, persistent ? "true" : "false");
    }
    return ret;
}

static int v2_shell_rgb_calibration(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[3], "status") == 0) {
        return v2_shell_rgb_calibration_print();
    }
    if (argc == 4 && strcmp(argv[3], "reset") == 0) {
        int ret = service_v2_actuators_rgb_calibration_reset();
        return ret == 0 ? v2_shell_rgb_calibration_print() : ret;
    }
    if (argc == 9 && strcmp(argv[3], "set") == 0) {
        service_v2_rgb_calibration_t calibration = {
            .version = SERVICE_V2_RGB_CALIBRATION_VERSION,
        };
        uint32_t red_gain;
        uint32_t green_gain;
        uint32_t blue_gain;
        uint32_t gamma_x100;
        if (!v2_parse_rgb_order(argv[4], &calibration.order) ||
            !v2_parse_u32(argv[5], SERVICE_V2_RGB_GAIN_MIN_PERMILLE, SERVICE_V2_RGB_GAIN_MAX_PERMILLE, &red_gain) ||
            !v2_parse_u32(argv[6], SERVICE_V2_RGB_GAIN_MIN_PERMILLE, SERVICE_V2_RGB_GAIN_MAX_PERMILLE, &green_gain) ||
            !v2_parse_u32(argv[7], SERVICE_V2_RGB_GAIN_MIN_PERMILLE, SERVICE_V2_RGB_GAIN_MAX_PERMILLE, &blue_gain) ||
            !v2_parse_u32(argv[8], SERVICE_V2_RGB_GAMMA_MIN_X100, SERVICE_V2_RGB_GAMMA_MAX_X100, &gamma_x100)) {
            return -EINVAL;
        }
        calibration.red_gain_permille = (uint16_t)red_gain;
        calibration.green_gain_permille = (uint16_t)green_gain;
        calibration.blue_gain_permille = (uint16_t)blue_gain;
        calibration.gamma_x100 = (uint16_t)gamma_x100;

        int ret = service_v2_actuators_rgb_calibration_set(&calibration);
        return ret == 0 ? v2_shell_rgb_calibration_print() : ret;
    }
    return -EINVAL;
}

static bool v2_shell_rgb_calibration_has_own_success_line(int argc, char **argv)
{
    return argc >= 3 && strcmp(argv[1], "rgb") == 0 && strcmp(argv[2], "calibration") == 0;
}

static int v2_shell_rgb(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[2], "calibration") == 0) {
        return v2_shell_rgb_calibration(argc, argv);
    }
    if (argc == 3 && strcmp(argv[2], "status") == 0) {
        return v2_shell_rgb_state();
    }
    if (argc == 3 && strcmp(argv[2], "release") == 0) {
        int ret = service_v2_actuators_rgb_release();
        v2_shell_rgb_ack("release", ret);
        return ret;
    }
    if (argc == 3 && strcmp(argv[2], "off") == 0) {
        service_v2_rgb_status_t status = {0};
        int ret = service_v2_actuators_rgb_get_status(&status);
        if (ret == 0) {
            ret = service_v2_actuators_rgb_strip_set(status.count, 0U, 0U, 0U, SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT);
        }
        v2_shell_rgb_ack("off", ret);
        return ret;
    }
    if (argc == 3 && strcmp(argv[2], "test") == 0) {
        int ret = service_v2_actuators_rgb_self_test();
        v2_shell_rgb_ack("test", ret);
        return ret;
    }
    if (argc == 6 && strcmp(argv[2], "set") == 0) {
        uint32_t red;
        uint32_t green;
        uint32_t blue;
        if (!v2_parse_u32(argv[3], 0U, UINT8_MAX, &red) || !v2_parse_u32(argv[4], 0U, UINT8_MAX, &green) ||
            !v2_parse_u32(argv[5], 0U, UINT8_MAX, &blue)) {
            return -EINVAL;
        }
        service_v2_rgb_status_t status = {0};
        int ret = service_v2_actuators_rgb_get_status(&status);
        if (ret == 0) {
            ret = service_v2_actuators_rgb_strip_set(status.count, (uint8_t)red, (uint8_t)green, (uint8_t)blue,
                                                     SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT);
        }
        v2_shell_rgb_ack("set", ret);
        return ret;
    }
    if (argc == 8 && strcmp(argv[2], "strip") == 0) {
        uint32_t count;
        uint32_t red;
        uint32_t green;
        uint32_t blue;
        uint32_t brightness;
        if (!v2_parse_u32(argv[3], SERVICE_V2_RGB_STRIP_MIN_COUNT, SERVICE_V2_RGB_STRIP_MAX_COUNT, &count) ||
            !v2_parse_u32(argv[4], 0U, UINT8_MAX, &red) || !v2_parse_u32(argv[5], 0U, UINT8_MAX, &green) ||
            !v2_parse_u32(argv[6], 0U, UINT8_MAX, &blue) ||
            !v2_parse_u32(argv[7], 0U, SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT, &brightness)) {
            v2_shell_rgb_ack("strip", -EINVAL);
            return -EINVAL;
        }
        int ret = service_v2_actuators_rgb_strip_set((uint8_t)count, (uint8_t)red, (uint8_t)green, (uint8_t)blue,
                                                     (uint8_t)brightness);
        v2_shell_rgb_ack("strip", ret);
        return ret;
    }
    if (argc == 4 && strcmp(argv[2], "count") == 0) {
        uint32_t count;
        if (!v2_parse_u32(argv[3], SERVICE_V2_RGB_STRIP_MIN_COUNT, SERVICE_V2_RGB_STRIP_MAX_COUNT, &count)) {
            v2_shell_rgb_ack("count", -EINVAL);
            return -EINVAL;
        }
        int ret = service_v2_actuators_rgb_strip_set_count((uint8_t)count);
        v2_shell_rgb_ack("count", ret);
        return ret;
    }
    if (argc >= 3 && (strcmp(argv[2], "strip") == 0 || strcmp(argv[2], "count") == 0)) {
        v2_shell_rgb_ack(argv[2], -EINVAL);
        return -EINVAL;
    }
    if (argc == 4 && strcmp(argv[2], "level") == 0) {
        uint32_t level;
        if (!v2_parse_u32(argv[3], 0U, 1U, &level)) {
            return -EINVAL;
        }
        int ret = service_v2_actuators_rgb_data_level(level != 0U);
        if (ret == 0) {
            shellPrint(shellGetCurrent(), "PA08/LED1-DI held at %u; measure DI to GND now\n", level);
        }
        return ret;
    }
    if (argc == 4 && strcmp(argv[2], "gpio-level") == 0) {
        uint32_t level;
        bool readback_high = false;
        if (!v2_parse_u32(argv[3], 0U, 1U, &level)) {
            return -EINVAL;
        }
        int ret = service_v2_actuators_rgb_gpio_level(level != 0U, &readback_high);
        if (ret == 0) {
            shellPrint(shellGetCurrent(),
                       "PA08 normal GPIO requested=%u readback=%u; "
                       "measure LED1-DI to GND now\n",
                       level, readback_high ? 1U : 0U);
        }
        return ret;
    }
    if (argc == 3 && strcmp(argv[2], "clock") == 0) {
        uint32_t reported_hz = 0U;
        uint32_t measured_hz = 0U;
        uint32_t cached_hz = 0U;
        int ret = service_v2_actuators_rgb_clock_measure(&reported_hz, &measured_hz, &cached_hz);
        if (ret == 0) {
            shellPrint(shellGetCurrent(), "WS2812 clock cpu=%uHz delay-derived=%uHz sender=%uHz\n", reported_hz,
                       measured_hz, cached_hz);
        }
        return ret;
    }
    if (argc == 4 && strcmp(argv[2], "scope") == 0) {
        if (strcmp(argv[3], "stop") == 0) {
            int ret = service_v2_actuators_rgb_scope_stop();
            if (ret == 0) {
                shellPrint(shellGetCurrent(), "WS2812 continuous scope output stopped\n");
            }
            return ret;
        }

        uint32_t bit;
        if (!v2_parse_u32(argv[3], 0U, 1U, &bit)) {
            return -EINVAL;
        }
        int ret = service_v2_actuators_rgb_scope_start(bit != 0U);
        if (ret == 0) {
            shellPrint(shellGetCurrent(),
                       "WS2812 continuous scope output started: bit=%u; "
                       "use 'v2hw rgb scope stop' to stop\n",
                       bit);
        }
        return ret;
    }
    return -EINVAL;
}

static const char *v2_servo_easing_name(service_v2_servo_easing_t easing)
{
    switch (easing) {
    case SERVICE_V2_SERVO_EASING_SMOOTH:
        return "smooth";
    case SERVICE_V2_SERVO_EASING_EASE_IN:
        return "ease-in";
    case SERVICE_V2_SERVO_EASING_EASE_OUT:
        return "ease-out";
    case SERVICE_V2_SERVO_EASING_LINEAR:
    default:
        return "linear";
    }
}

static bool v2_parse_servo_easing(const char *text, service_v2_servo_easing_t *easing)
{
    if (text == NULL || easing == NULL) {
        return false;
    }
    if (strcmp(text, "linear") == 0) {
        *easing = SERVICE_V2_SERVO_EASING_LINEAR;
    } else if (strcmp(text, "smooth") == 0) {
        *easing = SERVICE_V2_SERVO_EASING_SMOOTH;
    } else if (strcmp(text, "ease-in") == 0) {
        *easing = SERVICE_V2_SERVO_EASING_EASE_IN;
    } else if (strcmp(text, "ease-out") == 0) {
        *easing = SERVICE_V2_SERVO_EASING_EASE_OUT;
    } else {
        return false;
    }
    return true;
}

static const char *v2_servo_mode_name(const service_v2_servo_status_t *status)
{
    if (status->frame_active) {
        return "frame";
    }
    if (status->sweep_active) {
        return "sweep";
    }
    return "idle";
}

static void v2_shell_servo_pose(const service_v2_servo_status_t *status)
{
    shellPrint(shellGetCurrent(), "SERVO_POSE seq=%u current=%u,%u,%u,%u target=%u,%u,%u,%u\n", status->sequence,
               status->current_us[0], status->current_us[1], status->current_us[2], status->current_us[3],
               status->target_us[0], status->target_us[1], status->target_us[2], status->target_us[3]);
}

static void v2_shell_servo_lease(const service_v2_servo_status_t *status)
{
    shellPrint(shellGetCurrent(), "SERVO_LEASE seq=%u watchdog_ms=%u watchdog_remaining_ms=%u watchdog_expired=%u\n",
               status->sequence, status->watchdog_ms, status->watchdog_remaining_ms, status->watchdog_expired);
}

static int v2_shell_servo_state(bool kick_watchdog)
{
    int ret = kick_watchdog ? service_v2_actuators_servo_watchdog_kick() : 0;
    service_v2_servo_status_t status = {0};
    if (ret == 0) {
        ret = service_v2_actuators_servo_get_status(&status);
    }
    if (ret != 0) {
        shellPrint(shellGetCurrent(), "SERVO_STATE status=%d seq=0\n", ret);
        return ret;
    }

    shellPrint(shellGetCurrent(), "SERVO_STATE status=0 seq=%u mode=%s active=%u enabled=0x%02x\n", status.sequence,
               v2_servo_mode_name(&status), status.frame_active || status.sweep_active, status.enabled_mask);
    v2_shell_servo_pose(&status);
    shellPrint(shellGetCurrent(), "SERVO_TIMING seq=%u duration_ms=%u elapsed_ms=%u easing=%s sweep_channel=%d\n",
               status.sequence, status.duration_ms, status.elapsed_ms, v2_servo_easing_name(status.easing),
               status.sweep_active ? (int)status.sweep_servo : -1);
    v2_shell_servo_lease(&status);
    return 0;
}

static void v2_shell_servo_ack(const char *action, int status_code)
{
    service_v2_servo_status_t status = {0};
    int state_ret = service_v2_actuators_servo_get_status(&status);
    if (state_ret == 0) {
        shellPrint(shellGetCurrent(), "SERVO_ACK action=%s status=%d seq=%u enabled=0x%02x active=%u\n", action,
                   status_code, status.sequence, status.enabled_mask, status.frame_active || status.sweep_active);
        v2_shell_servo_pose(&status);
        v2_shell_servo_lease(&status);
    } else {
        shellPrint(shellGetCurrent(), "SERVO_ACK action=%s status=%d seq=0 enabled=0x00 active=0\n", action,
                   status_code);
        shellPrint(shellGetCurrent(), "SERVO_ERROR seq=0 state_status=%d\n", state_ret);
    }
}

static int v2_shell_servo(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[2], "frame") == 0 && argc != 9) {
        v2_shell_servo_ack("frame", -EINVAL);
        return -EINVAL;
    }
    if (argc >= 3 && strcmp(argv[2], "watchdog") == 0 && argc != 4) {
        v2_shell_servo_ack("watchdog", -EINVAL);
        return -EINVAL;
    }
    if (argc == 3 && strcmp(argv[2], "all-off") == 0) {
        /* Cancel both motion engines before disabling outputs; release the
         * action light as part of the same emergency-stop command. */
        int frame_ret = service_v2_actuators_servo_frame_stop();
        int sweep_ret = service_v2_actuators_servo_sweep_stop();
        int off_ret = service_v2_actuators_servo_disable_all();
        int rgb_ret = service_v2_actuators_rgb_release();
        int ret = frame_ret != 0 ? frame_ret : (sweep_ret != 0 ? sweep_ret : (off_ret != 0 ? off_ret : rgb_ret));
        v2_shell_servo_ack("all-off", ret);
        return ret;
    }
    if (argc == 3 && strcmp(argv[2], "state") == 0) {
        return v2_shell_servo_state(true);
    }
    if (argc == 3 && strcmp(argv[2], "stop") == 0) {
        int frame_ret = service_v2_actuators_servo_frame_stop();
        int sweep_ret = service_v2_actuators_servo_sweep_stop();
        int ret = frame_ret != 0 ? frame_ret : sweep_ret;
        v2_shell_servo_ack("stop", ret);
        return ret;
    }
    if (argc == 4 && strcmp(argv[2], "watchdog") == 0) {
        uint32_t timeout_ms;
        int ret = -EINVAL;
        if (v2_parse_u32(argv[3], 0U, SERVICE_V2_SERVO_WATCHDOG_MAX_MS, &timeout_ms) &&
            (timeout_ms == 0U || timeout_ms >= SERVICE_V2_SERVO_WATCHDOG_MIN_MS)) {
            ret = service_v2_actuators_servo_watchdog_set(timeout_ms);
        }
        v2_shell_servo_ack("watchdog", ret);
        return ret;
    }
    if (argc == 9 && strcmp(argv[2], "frame") == 0) {
        uint32_t duration_ms;
        uint32_t pulse_us[SERVICE_V2_SERVO_COUNT];
        service_v2_servo_easing_t easing;
        int ret = -EINVAL;
        if (v2_parse_u32(argv[3], 0U, SERVICE_V2_SERVO_FRAME_MAX_DURATION_MS, &duration_ms) &&
            v2_parse_servo_easing(argv[4], &easing) &&
            v2_parse_u32(argv[5], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[0]) &&
            v2_parse_u32(argv[6], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[1]) &&
            v2_parse_u32(argv[7], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[2]) &&
            v2_parse_u32(argv[8], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[3])) {
            uint16_t frame[SERVICE_V2_SERVO_COUNT];
            for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
                frame[i] = (uint16_t)pulse_us[i];
            }
            ret = service_v2_actuators_servo_frame(frame, duration_ms, easing);
        }
        v2_shell_servo_ack("frame", ret);
        return ret;
    }
    if (argc == 3 && strcmp(argv[2], "sweep-stop") == 0) {
        return service_v2_actuators_servo_sweep_stop();
    }
    if (argc == 3 && strcmp(argv[2], "sweep-status") == 0) {
        uint8_t active_channel = 0U;
        bool active = service_v2_actuators_servo_sweep_active(&active_channel);
        if (active) {
            shellPrint(shellGetCurrent(), "servo sweep: running channel=%u\n", active_channel);
        } else {
            shellPrint(shellGetCurrent(), "servo sweep: stopped\n");
        }
        return 0;
    }
    if ((argc == 4 || argc == 5) && strcmp(argv[2], "sweep") == 0) {
        uint32_t channel;
        uint32_t period_ms = SERVICE_V2_SERVO_SWEEP_DEF_PERIOD_MS;
        if (!v2_parse_u32(argv[3], 0U, SERVICE_V2_SERVO_COUNT - 1U, &channel) ||
            (argc == 5 && !v2_parse_u32(argv[4], SERVICE_V2_SERVO_SWEEP_MIN_PERIOD_MS,
                                        SERVICE_V2_SERVO_SWEEP_MAX_PERIOD_MS, &period_ms))) {
            return -EINVAL;
        }

        int ret = service_v2_actuators_servo_sweep_start((uint8_t)channel, (uint16_t)period_ms);
        if (ret == 0) {
            shellPrint(shellGetCurrent(), "servo%u/PA%02u: sweep %u..%uus period=%ums\n", channel, channel + 4U,
                       SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, period_ms);
        }
        return ret;
    }
    if (argc < 4 || argc > 6 || strcmp(argv[2], "test") != 0) {
        return -EINVAL;
    }
    if (service_v2_actuators_servo_sweep_active(NULL)) {
        /* A one-shot hold would fight the sweep task over the same shadow. */
        return -EBUSY;
    }
    service_v2_servo_status_t servo_status = {0};
    int status_ret = service_v2_actuators_servo_get_status(&servo_status);
    if (status_ret != 0) {
        return status_ret;
    }
    if (servo_status.frame_active) {
        return -EBUSY;
    }

    uint32_t channel;
    uint32_t pulse_us = SERVICE_V2_SERVO_CENTER_PULSE_US;
    uint32_t hold_ms = V2_SERVO_TEST_DEFAULT_HOLD_MS;
    if (!v2_parse_u32(argv[3], 0U, SERVICE_V2_SERVO_COUNT - 1U, &channel) ||
        (argc >= 5 &&
         !v2_parse_u32(argv[4], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us)) ||
        (argc == 6 && !v2_parse_u32(argv[5], 50U, V2_SERVO_TEST_MAX_HOLD_MS, &hold_ms))) {
        return -EINVAL;
    }

    int ret = service_v2_actuators_servo_set_us((uint8_t)channel, (uint16_t)pulse_us);
    if (ret == 0) {
        ret = service_v2_actuators_servo_enable((uint8_t)channel);
    }
    if (ret == 0) {
        shellPrint(shellGetCurrent(), "servo%u/PA%02u: pulse=%uus hold=%ums\n", channel, channel + 4U, pulse_us,
                   hold_ms);
        lisa_thread_mdelay(hold_ms);
    }

    int disable_ret = service_v2_actuators_servo_disable((uint8_t)channel);
    return ret != 0 ? ret : disable_ret;
}

static void v2_shell_motion_ack(int status_code)
{
    service_v2_servo_status_t servo = {0};
    service_v2_rgb_status_t rgb = {0};
    int servo_ret = service_v2_actuators_servo_get_status(&servo);
    int rgb_ret = service_v2_actuators_rgb_get_status(&rgb);

    uint32_t sequence = servo_ret == 0 ? servo.sequence : 0U;
    uint8_t enabled_mask = servo_ret == 0 ? servo.enabled_mask : 0U;
    shellPrint(shellGetCurrent(), "MOTION_ACK action=frame status=%d seq=%u enabled=0x%02x\n", status_code, sequence,
               enabled_mask);

    if (servo_ret == 0 && rgb_ret == 0) {
        shellPrint(shellGetCurrent(), "MOTION_TARGET seq=%u target=%u,%u,%u,%u duration_ms=%u easing=%s\n",
                   servo.sequence, servo.target_us[0], servo.target_us[1], servo.target_us[2], servo.target_us[3],
                   servo.duration_ms, v2_servo_easing_name(servo.easing));
        shellPrint(shellGetCurrent(), "RGB_STATE seq=%u count=%u rgb=%u,%u,%u brightness=%u owner=%s\n", servo.sequence,
                   rgb.count, rgb.red, rgb.green, rgb.blue, rgb.brightness_percent, v2_rgb_owner_name(rgb.owner));
    } else {
        shellPrint(shellGetCurrent(), "MOTION_ERROR seq=%u servo_state_status=%d rgb_state_status=%d\n", sequence,
                   servo_ret, rgb_ret);
    }
}

static int v2_shell_motion(int argc, char **argv)
{
    if (argc != 14 || strcmp(argv[2], "frame") != 0) {
        v2_shell_motion_ack(-EINVAL);
        return -EINVAL;
    }

    uint32_t duration_ms;
    uint32_t pulse_us[SERVICE_V2_SERVO_COUNT];
    uint32_t count;
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t brightness;
    service_v2_servo_easing_t easing;
    int ret = -EINVAL;

    if (v2_parse_u32(argv[3], 0U, SERVICE_V2_SERVO_FRAME_MAX_DURATION_MS, &duration_ms) &&
        v2_parse_servo_easing(argv[4], &easing) &&
        v2_parse_u32(argv[5], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[0]) &&
        v2_parse_u32(argv[6], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[1]) &&
        v2_parse_u32(argv[7], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[2]) &&
        v2_parse_u32(argv[8], SERVICE_V2_SERVO_MIN_PULSE_US, SERVICE_V2_SERVO_MAX_PULSE_US, &pulse_us[3]) &&
        v2_parse_u32(argv[9], SERVICE_V2_RGB_STRIP_MIN_COUNT, SERVICE_V2_RGB_STRIP_MAX_COUNT, &count) &&
        v2_parse_u32(argv[10], 0U, UINT8_MAX, &red) && v2_parse_u32(argv[11], 0U, UINT8_MAX, &green) &&
        v2_parse_u32(argv[12], 0U, UINT8_MAX, &blue) &&
        v2_parse_u32(argv[13], 0U, SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT, &brightness)) {
        uint16_t frame[SERVICE_V2_SERVO_COUNT];
        for (uint32_t i = 0U; i < SERVICE_V2_SERVO_COUNT; ++i) {
            frame[i] = (uint16_t)pulse_us[i];
        }
        ret = service_v2_actuators_motion_frame(frame, duration_ms, easing, (uint8_t)count, (uint8_t)red,
                                                (uint8_t)green, (uint8_t)blue, (uint8_t)brightness);
    }

    v2_shell_motion_ack(ret);
    return ret;
}

/*
 * 配网门户的手动开关。这块板没有屏，正常入口是电源键多击；调试时按不了键，
 * 所以从 shell 也留一条路进去。注意热点一起来 STA 就断了，走 ADB 的这条
 * 连接不受影响（USB 不是 WiFi），但云端会掉线，属预期。
 */
/*
 * 不经过语音直接驱动步态，用来分开定位问题：这条命令能动就说明舵机和关键帧
 * 没问题，动不了再去查云端识别那一侧。
 *
 * `say` 只跑一遍关键词匹配并打印结果，不真的动腿，方便验证词表。
 */
static int v2_shell_gait(int argc, char **argv)
{
    if (argc < 3) {
        return -EINVAL;
    }

    if (strcmp(argv[2], "status") == 0) {
        service_v2_gait_status_t gait = {0};
        service_voice_action_status_t voice = {0};
        int gait_ret = service_v2_gait_get_status(&gait);
        int voice_ret = service_voice_action_get_status(&voice);

        shellPrint(shellGetCurrent(), "gait ready=%s busy=%s action=%s cycles=%u last_error=%d (status=%d)\n",
                   gait.ready ? "yes" : "no", gait.busy ? "yes" : "no", service_v2_gait_action_name(gait.current),
                   (unsigned)gait.cycles_done, gait.last_error, gait_ret);
        shellPrint(shellGetCurrent(), "voice ready=%s matched=%u suppressed=%u last=%s (status=%d)\n",
                   voice.ready ? "yes" : "no", (unsigned)voice.matched_count, (unsigned)voice.suppressed_count,
                   service_v2_gait_action_name(voice.last_match), voice_ret);
        shellPrint(shellGetCurrent(),
                   "cloud action_list recognized=%u unmapped=%u queued=%u active=%s dropped=%u\n",
                   (unsigned)voice.cloud_action_count, (unsigned)voice.cloud_unknown_count,
                   (unsigned)voice.queued_count, voice.action_active ? "yes" : "no",
                   (unsigned)voice.queue_dropped_count);
        return 0;
    }

    /*
     * 验证云端指令码的映射，不连云也不动腿。控制台里"具体下发指令"填了什么，
     * 就用什么当参数：认得出来才说明控制台和固件对上了。
     */
    if (strcmp(argv[2], "code") == 0) {
        if (argc != 4) {
            return -EINVAL;
        }

        service_v2_gait_action_t action;

        if (!service_voice_action_from_cloud_code(argv[3], &action)) {
            shellPrint(shellGetCurrent(), "unmapped code\n");
            return 0;
        }
        shellPrint(shellGetCurrent(), "code %s -> %s\n", argv[3], service_v2_gait_action_name(action));
        return 0;
    }

    if (strcmp(argv[2], "say") == 0) {
        if (argc != 4) {
            return -EINVAL;
        }

        service_v2_gait_action_t action;

        if (!service_voice_action_match(argv[3], &action)) {
            shellPrint(shellGetCurrent(), "no action matched\n");
            return 0;
        }
        shellPrint(shellGetCurrent(), "matched %s\n", service_v2_gait_action_name(action));
        return 0;
    }

    service_v2_gait_action_t action;

    if (!service_v2_gait_action_from_name(argv[2], &action)) {
        return -EINVAL;
    }

    uint32_t cycles = service_v2_gait_default_cycles(action);

    if (argc == 4) {
        /* 走行类的 0 表示持续到下一条命令；翻滚会在 gait 层只接受 1 或 2。 */
        if (!v2_parse_u32(argv[3], 0U, SERVICE_V2_GAIT_MAX_CYCLES, &cycles)) {
            return -EINVAL;
        }
    } else if (argc != 3) {
        return -EINVAL;
    }

    return service_v2_gait_submit(action, cycles);
}

static int v2_shell_interaction(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[2], "status") != 0) {
        return -EINVAL;
    }

    service_v2_imu_interaction_status_t status = {0};
    int ret = service_v2_imu_interaction_get_status(&status);

    shellPrint(shellGetCurrent(),
               "interaction ready=%s inhibited=%s pose=%s last=%s quiet_ms=%u cooldown_ms=%u status=%d\n",
               status.ready ? "yes" : "no", status.inhibited ? "yes" : "no",
               service_v2_imu_interaction_pose_name(status.pose),
               service_v2_imu_interaction_event_name(status.last_event), (unsigned)status.quiet_remaining_ms,
               (unsigned)status.cooldown_remaining_ms, ret);
    shellPrint(shellGetCurrent(), "imu accel=%d,%d,%d gyro=%d,%d,%d samples=%u errors=%u\n", status.accel_x,
               status.accel_y, status.accel_z, status.gyro_x, status.gyro_y, status.gyro_z,
               (unsigned)status.sample_count, (unsigned)status.imu_error_count);
    shellPrint(shellGetCurrent(),
               "events upside=%u reverse_home_suppressed=%u shake=%u shake_peak_gyro_lsb=%u "
               "feedback_played=%u suppressed=%u missing=%u\n",
               (unsigned)status.upside_down_count, (unsigned)status.reverse_home_suppressed_count,
               (unsigned)status.table_shake_count, (unsigned)status.shake_peak_gyro_lsb,
               (unsigned)status.feedback_played_count, (unsigned)status.feedback_suppressed_count,
               (unsigned)status.feedback_missing_count);
    return ret;
}

static int v2_shell_low_battery(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[2], "status") == 0) {
        service_v2_low_battery_status_t status = {0};
        int ret = service_v2_low_battery_get_status(&status);

        shellPrint(shellGetCurrent(),
                   "battery-alert ready=%s level=%u%% external_power=%s simulated=%s "
                   "reminder_allowed=%s warning=%s critical_confirming=%s shutdown=%s "
                   "reminders=%u shutdowns=%u\n",
                   status.ready ? "yes" : "no", (unsigned)status.level,
                   status.external_power ? "yes" : "no",
                   status.simulated ? "yes" : "no",
                   status.reminder_allowed ? "yes" : "no",
                   status.warning_active ? "yes" : "no",
                   status.critical_confirming ? "yes" : "no",
                   status.shutdown_requested ? "yes" : "no",
                   (unsigned)status.reminder_count,
                   (unsigned)status.shutdown_count);
        return ret;
    }

    if (argc == 3 && strcmp(argv[2], "clear") == 0) {
        service_v2_low_battery_clear_simulation();
        shellPrint(shellGetCurrent(), "battery-alert simulation cleared\n");
        return 0;
    }

    if (argc == 5 && strcmp(argv[2], "simulate") == 0) {
        uint32_t level;
        service_v2_low_battery_sim_state_t state;

        if (!v2_parse_u32(argv[3], 0U, 100U, &level)) {
            return -EINVAL;
        }
        if (strcmp(argv[4], "idle") == 0) {
            state = SERVICE_V2_LOW_BATTERY_SIM_IDLE;
        } else if (strcmp(argv[4], "active") == 0) {
            state = SERVICE_V2_LOW_BATTERY_SIM_ACTIVE;
        } else if (strcmp(argv[4], "sleep") == 0) {
            state = SERVICE_V2_LOW_BATTERY_SIM_SLEEP;
        } else {
            return -EINVAL;
        }

        int ret = service_v2_low_battery_simulate((uint8_t)level, state);
        if (ret == 0) {
            shellPrint(shellGetCurrent(),
                       "battery-alert simulation level=%u%% state=%s; policy confirms for 5s\n",
                       (unsigned)level, argv[4]);
        }
        return ret;
    }

    return -EINVAL;
}

static int v2_shell_netcfg(int argc, char **argv)
{
    if (argc != 3) {
        return -EINVAL;
    }

    if (strcmp(argv[2], "start") == 0) {
        return app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST);
    }
    if (strcmp(argv[2], "stop") == 0) {
        return app_netcfg_portal_stop();
    }
    if (strcmp(argv[2], "status") == 0) {
        char ssid[33] = {0};
        sys_network_status_t network = {0};
        bool active = app_netcfg_portal_is_active();
        bool named = app_netcfg_portal_ssid_copy(ssid, sizeof(ssid));
        int network_ret = sys_network_get_status(&network);

        shellPrint(shellGetCurrent(), "netcfg portal=%s ssid=%s ip=%s http=80 dns=53\n", active ? "active" : "idle",
                   named ? ssid : "<unset>", app_netcfg_portal_ip());
        app_netcfg_portal_status_t portal = {0};
        app_netcfg_portal_get_status(&portal);
        shellPrint(shellGetCurrent(), "portal reason=%d ap_up=%d clients_known=%d clients=0x%lx credentials=%d transaction=%d scan=%d closing=%d caller=0x%lx\n",
                   (int)portal.reason, portal.ap_up, portal.clients_known, (unsigned long)portal.clients,
                   portal.credential_active, portal.transaction_busy, portal.scan_busy, portal.closing,
                   (unsigned long)portal.start_caller);
        if (network_ret == 0) {
            shellPrint(shellGetCurrent(),
                       "wifi mode=%d available=%s started=%s connected=%s "
                       "saved_ap=%s switching=%s bearer=%d internet=%s result=%d ota=%d\n",
                       (int)network.mode, network.wifi_available ? "yes" : "no", sys_wifi_is_started() ? "yes" : "no",
                       network.wifi_connected ? "yes" : "no", sys_wifi_has_ap() ? "yes" : "no",
                       network.switching ? "yes" : "no", (int)network.active_bearer, network.connected ? "yes" : "no",
                       (int)sys_wifi_get_connect_result(), (int)ota_manager_get_state());
        } else {
            shellPrint(shellGetCurrent(), "wifi status unavailable: %d\n", network_ret);
        }
        return 0;
    }
    return -EINVAL;
}

static int v2_shell_cmd(int argc, char **argv)
{
    if (argc < 2) {
        v2_shell_help();
        return -EINVAL;
    }

    int ret;
    if (strcmp(argv[1], "status") == 0 && argc == 2) {
        uint8_t sweep_channel = 0U;
        bool sweeping = service_v2_actuators_servo_sweep_active(&sweep_channel);
        shellPrint(shellGetCurrent(),
                   "board=arcs_mini_v2_1 actuators=%s "
                   "servos=disabled-by-default sweep=%s\n",
                   service_v2_actuators_is_ready() ? "ready" : "not-ready", sweeping ? "running" : "stopped");
        if (sweeping) {
            shellPrint(shellGetCurrent(), "servo sweep: running channel=%u\n", sweep_channel);
        }
        shellPrint(shellGetCurrent(), "servo0..3=PA04..PA07 rgb=PA08 touch=PA09,PA13\n");
        shellPrint(shellGetCurrent(), "shared-i2c0: mpu=0x68 pn532=0x24\n");
        return 0;
    }
    if (strcmp(argv[1], "power") == 0 && argc == 2) {
        ret = v2_shell_power();
    } else if (strcmp(argv[1], "audio-status") == 0 && argc == 2) {
        ret = v2_shell_audio_status();
    } else if (strcmp(argv[1], "selftest") == 0 && argc == 2) {
        ret = v2_shell_selftest();
    } else if (strcmp(argv[1], "touch") == 0) {
        if (argc == 2) {
            ret = v2_shell_touch();
        } else if (argc == 3 && strcmp(argv[2], "pa13") == 0) {
            ret = v2_shell_touch_channel(SERVICE_V2_TOUCH_CHANNEL_PA13, "PA13");
        } else if (argc == 3 && strcmp(argv[2], "all") == 0) {
            ret = v2_shell_touch_all();
        } else if (argc >= 3 && argc <= 5 && strcmp(argv[2], "watch") == 0) {
            uint32_t duration_ms = 3000U;
            service_v2_touch_channel_t channel = SERVICE_V2_TOUCH_CHANNEL_PA09;
            bool all = false;
            int duration_arg = 3;

            if (argc >= 4 && strcmp(argv[3], "pa13") == 0) {
                channel = SERVICE_V2_TOUCH_CHANNEL_PA13;
                duration_arg = 4;
            } else if (argc >= 4 && strcmp(argv[3], "all") == 0) {
                all = true;
                duration_arg = 4;
            }

            if (argc > duration_arg + 1 ||
                (argc == duration_arg + 1 && !v2_parse_u32(argv[duration_arg], 100U, 10000U, &duration_ms))) {
                ret = -EINVAL;
            } else if (all) {
                ret = v2_shell_touch_watch_all(duration_ms);
            } else if (channel == SERVICE_V2_TOUCH_CHANNEL_PA13) {
                ret = v2_shell_touch_watch_channel(channel, "PA13", duration_ms);
            } else {
                ret = v2_shell_touch_watch(duration_ms);
            }
        } else {
            ret = -EINVAL;
        }
    } else if (strcmp(argv[1], "mpu") == 0 && argc == 2) {
        ret = v2_shell_mpu();
    } else if (strcmp(argv[1], "i2c") == 0 && argc >= 4) {
        uint32_t bus;
        if (!v2_parse_u32(argv[3], 0U, 1U, &bus)) {
            ret = -EINVAL;
        } else if (strcmp(argv[2], "scan") == 0 && argc == 4) {
            ret = v2_shell_i2c_scan(bus);
        } else if (strcmp(argv[2], "status") == 0 && argc == 4) {
            ret = v2_shell_i2c_status(bus);
        } else if (strcmp(argv[2], "probe") == 0 && argc == 5) {
            uint32_t address;
            ret = v2_parse_u32(argv[4], 0x08U, 0x77U, &address) ? v2_shell_i2c_probe(bus, address) : -EINVAL;
        } else {
            ret = -EINVAL;
        }
    } else if (strcmp(argv[1], "nfc") == 0 && argc >= 3) {
        if (strcmp(argv[2], "info") == 0 && argc == 3) {
            ret = v2_shell_nfc_info();
        } else if (strcmp(argv[2], "scan") == 0 && argc <= 4) {
            uint32_t timeout_ms = SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS;
            if (argc == 4 && !v2_parse_u32(argv[3], 1U, SERVICE_V2_PN532_MAX_TIMEOUT_MS, &timeout_ms)) {
                ret = -EINVAL;
            } else {
                ret = v2_shell_nfc_scan(timeout_ms);
            }
        } else if (strcmp(argv[2], "card-read") == 0 && argc <= 4) {
            uint32_t timeout_ms = SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS;
            if (argc == 4 && !v2_parse_u32(argv[3], 1U, SERVICE_V2_PN532_MAX_TIMEOUT_MS, &timeout_ms)) {
                ret = -EINVAL;
            } else {
                ret = v2_shell_nfc_card_read(timeout_ms);
            }
        } else if (strcmp(argv[2], "target") == 0 && argc <= 4) {
            uint32_t timeout_ms = SERVICE_V2_PN532_TARGET_DEFAULT_TIMEOUT_MS;
            if (argc == 4 && !v2_parse_u32(argv[3], 1U, SERVICE_V2_PN532_TARGET_MAX_TIMEOUT_MS, &timeout_ms)) {
                ret = -EINVAL;
            } else {
                ret = v2_shell_nfc_target(timeout_ms);
            }
        } else if (strcmp(argv[2], "peer") == 0) {
            ret = v2_shell_nfc_peer(argc, argv);
        } else {
            ret = -EINVAL;
        }
    } else if (strcmp(argv[1], "rgb") == 0) {
        ret = v2_shell_rgb(argc, argv);
    } else if (strcmp(argv[1], "servo") == 0) {
        ret = v2_shell_servo(argc, argv);
    } else if (strcmp(argv[1], "motion") == 0) {
        ret = v2_shell_motion(argc, argv);
    } else if (strcmp(argv[1], "vol") == 0) {
        ret = v2_shell_volume(argc, argv);
    } else if (strcmp(argv[1], "netcfg") == 0) {
        ret = v2_shell_netcfg(argc, argv);
    } else if (strcmp(argv[1], "interaction") == 0) {
        ret = v2_shell_interaction(argc, argv);
    } else if (strcmp(argv[1], "battery-alert") == 0) {
        ret = v2_shell_low_battery(argc, argv);
    } else if (strcmp(argv[1], "standby") == 0) {
        ret = v2_shell_standby(argc, argv);
    } else if (strcmp(argv[1], "gait") == 0) {
        ret = v2_shell_gait(argc, argv);
    } else {
        ret = -EINVAL;
    }

    if (ret != 0) {
        shellPrint(shellGetCurrent(), "v2hw command failed: %d\n", ret);
        if (ret == -EINVAL) {
            v2_shell_help();
        }
    } else if (!v2_shell_rgb_calibration_has_own_success_line(argc, argv)) {
        shellPrint(shellGetCurrent(), "ok\n");
    }
    return ret;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, v2hw,
                 v2_shell_cmd, arcs mini v2.1 hardware diagnostics);
