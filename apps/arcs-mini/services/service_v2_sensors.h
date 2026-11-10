#ifndef SERVICE_V2_SENSORS_H
#define SERVICE_V2_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ARCS MINI V2 sensor service hardware contract:
 *
 * - TTP223 OUT: PA09 (U10/U12) and PA13 (U7/U9) GPIO inputs. The V2.1
 *   schematic straps both STG pins high, selecting toggle/latched outputs;
 *   the optional SLH stuffing resistors are marked NC.
 * - MPU6050 and PN532 share I2C0 on PB06/PB07. The controller driver
 *   serializes transfers.
 * - MPU6050: 7-bit address 0x68 (AD0 low), optional INT on PA12.
 * - PN532: 7-bit address 0x24 (the 8-bit write address on the wire is 0x48).
 *   The board hard-wires PN532 I2C mode with I0=0 and I1=1.
 *
 * Board pinmux and the corresponding GPIO/I2C Kconfig options must be ready
 * before these APIs are called. All APIs are intended for FreeRTOS task
 * context; GPIO interrupt handlers only set internal pending flags.
 */

#define SERVICE_V2_PN532_UID_MAX_LEN               10U
#define SERVICE_V2_PN532_ATS_MAX_LEN               32U
#define SERVICE_V2_PN532_HISTORICAL_MAX_LEN        15U
#define SERVICE_V2_PN532_INITIATOR_COMMAND_MAX_LEN 53U
#define SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN    32U
#define SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS        1000U
#define SERVICE_V2_PN532_MAX_TIMEOUT_MS            5000U
#define SERVICE_V2_PN532_TARGET_DEFAULT_TIMEOUT_MS 10000U
#define SERVICE_V2_PN532_TARGET_MAX_TIMEOUT_MS     30000U

typedef enum {
    SERVICE_V2_SENSORS_OK = 0,
    SERVICE_V2_SENSORS_ERR_INVALID = -1,
    SERVICE_V2_SENSORS_ERR_NOT_READY = -2,
    SERVICE_V2_SENSORS_ERR_IO = -3,
    SERVICE_V2_SENSORS_ERR_TIMEOUT = -4,
    SERVICE_V2_SENSORS_ERR_PROTOCOL = -5,
    SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE = -6,
    SERVICE_V2_SENSORS_ERR_NO_TARGET = -7,
    SERVICE_V2_SENSORS_ERR_BUSY = -8,
    SERVICE_V2_SENSORS_ERR_NO_MEMORY = -9,
    SERVICE_V2_SENSORS_ERR_SELF_TEST = -10,
    SERVICE_V2_SENSORS_ERR_OVERFLOW = -11,
} service_v2_sensors_error_t;

typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t temperature;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} service_v2_mpu6050_raw_t;

typedef struct {
    uint8_t ic;
    uint8_t version;
    uint8_t revision;
    uint8_t support;
} service_v2_pn532_firmware_t;

typedef struct {
    uint8_t target_number;
    uint16_t sens_res;
    uint8_t sel_res;
    uint8_t uid_len;
    uint8_t uid[SERVICE_V2_PN532_UID_MAX_LEN];
    /* Complete ATS, including its leading TL/length byte. */
    uint8_t ats_len;
    uint8_t ats[SERVICE_V2_PN532_ATS_MAX_LEN];
} service_v2_pn532_target_t;

typedef struct {
    uint8_t mode;
    uint8_t initiator_command_len;
    uint8_t initiator_command[SERVICE_V2_PN532_INITIATOR_COMMAND_MAX_LEN];
} service_v2_pn532_activation_t;

typedef struct {
    int touch_status;
    int mpu6050_status;
    int pn532_status;
    bool touch_active;
    uint8_t mpu6050_who_am_i;
    service_v2_mpu6050_raw_t mpu6050_raw;
    service_v2_pn532_firmware_t pn532_firmware;
    int touch_pa13_status;
    bool touch_pa13_active;
} service_v2_sensors_self_test_result_t;

typedef enum {
    SERVICE_V2_TOUCH_CHANNEL_PA09 = 0,
    SERVICE_V2_TOUCH_CHANNEL_PA13,
    SERVICE_V2_TOUCH_CHANNEL_COUNT,
} service_v2_touch_channel_t;

/* TTP223: both channels support polling. The compatibility read/IRQ APIs below
 * continue to address PA09; use read_channel() to select either input. */
int service_v2_touch_init(void);
int service_v2_touch_read(bool *touched);
int service_v2_touch_read_channel(service_v2_touch_channel_t channel, bool *touched);
int service_v2_touch_set_irq_enabled(bool enable);
int service_v2_touch_take_irq_event(bool *pending, bool *touched);

/*
 * MPU6050 init selects +/-2 g, +/-250 dps, DLPF=3 and a 200 Hz sample rate.
 * Passing true enables the PA12 data-ready GPIO interrupt; no business callback
 * runs in interrupt context. A later init call may change the IRQ setting.
 */
int service_v2_mpu6050_init(bool enable_data_ready_irq);
int service_v2_mpu6050_set_sleep(bool sleep);
int service_v2_mpu6050_who_am_i(uint8_t *who_am_i);
int service_v2_mpu6050_read_raw(service_v2_mpu6050_raw_t *sample);
int service_v2_mpu6050_take_data_ready(bool *pending);

/* PN532 I2C transport and basic ISO/IEC 14443-A discovery. */
int service_v2_pn532_init(void);
int service_v2_pn532_get_firmware_version(service_v2_pn532_firmware_t *firmware);
int service_v2_pn532_list_passive_target(service_v2_pn532_target_t *target, uint32_t timeout_ms);
int service_v2_pn532_wait_for_type_a_activation(service_v2_pn532_activation_t *activation, uint32_t timeout_ms);
int service_v2_pn532_wait_for_type_a_activation_with_identity(const uint8_t nfcid1t[3], const uint8_t *historical_bytes,
                                                              uint8_t historical_bytes_len,
                                                              service_v2_pn532_activation_t *activation,
                                                              uint32_t timeout_ms);
/* Advertise an ATS for a bounded window without consuming the target-mode
 * host response. This is the safe path when P70_IRQ is not wired. */
int service_v2_pn532_advertise_type_a_identity(const uint8_t nfcid1t[3], const uint8_t *historical_bytes,
                                               uint8_t historical_bytes_len, uint32_t duration_ms);

/*
 * Exchange one application payload in each direction through an
 * ISO/IEC 14443-4 Type-A session at 106 kbit/s.  The initiator activates the
 * PICC with InListPassiveTarget, exchanges one I-block with InDataExchange,
 * and releases it with InRelease.  The target is activated by TgInitAsTarget
 * and services that I-block with TgGetData/TgSetData.
 *
 * local_len must not exceed SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN.  On
 * success, peer_payload contains the remote payload and *peer_len its length.
 * The initiator also receives the activation data (UID and ATS) in target.
 *
 * A zero activation_timeout_ms selects the implementation default. Because
 * V2.1 does not route PN532 P70_IRQ, the target polls the I2C RDY byte within
 * that bounded RF-listening window and drains the activation response as soon
 * as it becomes available.
 */
int service_v2_pn532_type_a_initiator_exchange(const uint8_t *local_payload, uint8_t local_len, uint8_t *peer_payload,
                                               uint8_t peer_capacity, uint8_t *peer_len,
                                               service_v2_pn532_target_t *target, uint32_t activation_timeout_ms);

/*
 * Compatibility-preserving initiator variant with optional board-side timing.
 * When exchange_elapsed_ms is non-NULL and the exchange succeeds, it receives
 * the RTOS-tick duration from a successful InListPassiveTarget activation
 * response through a successful InDataExchange response.  The duration does
 * not include shell/ADB work or the subsequent InRelease cleanup.  The
 * original API above remains available and performs no timing output.
 */
int service_v2_pn532_type_a_initiator_exchange_timed(const uint8_t *local_payload, uint8_t local_len,
                                                     uint8_t *peer_payload, uint8_t peer_capacity, uint8_t *peer_len,
                                                     service_v2_pn532_target_t *target, uint32_t activation_timeout_ms,
                                                     uint32_t *exchange_elapsed_ms);
int service_v2_pn532_type_a_target_exchange(const uint8_t nfcid1t[3], const uint8_t *historical_bytes,
                                            uint8_t historical_bytes_len, const uint8_t *local_payload,
                                            uint8_t local_len, uint8_t *peer_payload, uint8_t peer_capacity,
                                            uint8_t *peer_len, uint32_t activation_timeout_ms);

/*
 * Aggregate init brings up TTP223 and MPU6050; PN532 probing is deliberately
 * lazy so a missing/mis-strapped NFC device cannot delay application boot.
 * The explicit self-test probes all three sensors and does not require a card.
 */
int service_v2_sensors_init(void);
int service_v2_sensors_self_test(service_v2_sensors_self_test_result_t *result);
const char *service_v2_sensors_strerror(int error);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_V2_SENSORS_H */
