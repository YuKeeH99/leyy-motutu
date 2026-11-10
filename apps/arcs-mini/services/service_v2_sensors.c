#include "service_v2_sensors.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "lisa_device.h"
#include "lisa_gpio.h"
#include "lisa_i2c.h"
#include "lisa_log.h"
#include "lisa_mutex.h"
#include "lisa_thread.h"

#define TAG "v2.sensors"

#define SERVICE_LOCK_TIMEOUT_MS 1000U

#define TOUCH_GPIO_DEVICE   "gpioa"
#define TOUCH_GPIO_PIN_PA09 9U
#define TOUCH_GPIO_PIN_PA13 13U

#define MPU6050_I2C_DEVICE      MPU6050_I2C_DEVICE_NAME
#define MPU6050_I2C_ADDRESS     0x68U
#define MPU6050_INT_GPIO_DEVICE "gpioa"
#define MPU6050_INT_GPIO_PIN    12U

#define MPU6050_REG_SMPLRT_DIV   0x19U
#define MPU6050_REG_CONFIG       0x1AU
#define MPU6050_REG_GYRO_CONFIG  0x1BU
#define MPU6050_REG_ACCEL_CONFIG 0x1CU
#define MPU6050_REG_INT_PIN_CFG  0x37U
#define MPU6050_REG_INT_ENABLE   0x38U
#define MPU6050_REG_ACCEL_XOUT_H 0x3BU
#define MPU6050_REG_PWR_MGMT_1   0x6BU
#define MPU6050_REG_WHO_AM_I     0x75U
#define MPU6050_WHO_AM_I_VALUE   0x68U
#define MPU6050_RESET_DELAY_MS   100U
#define MPU6050_WAKE_DELAY_MS    10U

#define PN532_I2C_DEVICE                       PN532_I2C_DEVICE_NAME
/* LISA takes a 7-bit address. PN532's corresponding 8-bit write byte is 0x48. */
#define PN532_I2C_ADDRESS                      0x24U
#define PN532_I2C_READY                        0x01U
#define PN532_POLL_INTERVAL_MS                 5U
#define PN532_I2C_RECOVERY_SETTLE_MS           3U
#define PN532_COMMAND_WRITE_RETRY_MS           20U
#define PN532_RESYNC_SETTLE_MS                 20U
#define PN532_ACK_WIRE_LEN                     7U
#define PN532_RESPONSE_WIRE_MAX                64U
#define PN532_RESPONSE_DATA_MAX                56U
#define PN532_COMMAND_FRAME_MAX                64U
#define PN532_TYPE_A_TARGET_DEFAULT_WINDOW_MS  600U
#define PN532_TYPE_A_TARGET_MAX_WINDOW_MS      1600U
#define PN532_TYPE_A_TARGET_READY_GUARD_MS     100U
#define PN532_TYPE_A_INITIATOR_DATA_TIMEOUT_MS 2500U
#define PN532_TYPE_A_TARGET_DATA_TIMEOUT_MS    1500U
#define PN532_TYPE_A_RELEASE_TIMEOUT_MS        500U
#define PN532_TYPE_A_RELEASE_POLL_MS           10U
#define PN532_TYPE_A_RELEASE_SETTLE_MS         80U
#define PN532_TYPE_A_POST_EXCHANGE_COOLDOWN_MS 30U

#define PN532_PREAMBLE      0x00U
#define PN532_START_CODE_1  0x00U
#define PN532_START_CODE_2  0xFFU
#define PN532_HOST_TO_PN532 0xD4U
#define PN532_PN532_TO_HOST 0xD5U

#define PN532_CMD_GET_FIRMWARE_VERSION   0x02U
#define PN532_CMD_WRITE_REGISTER         0x08U
#define PN532_CMD_SET_PARAMETERS         0x12U
#define PN532_CMD_SAM_CONFIGURATION      0x14U
#define PN532_CMD_RF_CONFIGURATION       0x32U
#define PN532_CMD_IN_DATA_EXCHANGE       0x40U
#define PN532_CMD_IN_LIST_PASSIVE_TARGET 0x4AU
#define PN532_CMD_IN_RELEASE             0x52U
#define PN532_CMD_TG_GET_DATA            0x86U
#define PN532_CMD_TG_GET_TARGET_STATUS   0x8AU
#define PN532_CMD_TG_INIT_AS_TARGET      0x8CU
#define PN532_CMD_TG_SET_DATA            0x8EU

#define PN532_XRAM_CPU_POWER_MODE_H 0x02U
#define PN532_XRAM_CPU_POWER_MODE_L 0xFCU
#define PN532_CPU_POWER_MODE_NORMAL 0x00U

#define PN532_TARGET_STATE_PICC_RELEASED   0x80U
#define PN532_TARGET_STATE_PICC_ACTIVATED  0x81U
#define PN532_TARGET_STATE_PICC_DESELECTED 0x82U

#define PN532_BRTY_ISO14443A_106_KBPS           0x00U
#define PN532_TARGET_MODE_PASSIVE_ONLY          (1U << 0)
#define PN532_TARGET_MODE_PICC_ONLY             (1U << 2)
#define PN532_TARGET_SEL_RES_ISO14443_4         0x20U
#define PN532_TARGET_ACTIVATION_TYPE_A_PICC_106 0x08U
#define PN532_TARGET_ACTIVATION_MODE_MASK       0x7FU

typedef struct {
    lisa_device_t *dev;
    bool initialized;
    bool irq_enabled;
    volatile bool irq_pending;
} touch_context_t;

typedef struct {
    lisa_device_t *i2c_dev;
    lisa_device_t *irq_gpio_dev;
    lisa_mutex_t *lock;
    bool bus_ready;
    bool initialized;
    bool irq_enabled;
    bool sleeping;
    volatile bool data_ready;
} mpu6050_context_t;

typedef struct {
    lisa_device_t *i2c_dev;
    lisa_mutex_t *lock;
    bool bus_ready;
    bool initialized;
    bool firmware_valid;
    service_v2_pn532_firmware_t firmware;
} pn532_context_t;

static touch_context_t s_touch;
static mpu6050_context_t s_mpu6050;
static pn532_context_t s_pn532;

static void pn532_log_strap_hint(int error);

static const uint8_t s_touch_gpio_pins[SERVICE_V2_TOUCH_CHANNEL_COUNT] = {
    [SERVICE_V2_TOUCH_CHANNEL_PA09] = TOUCH_GPIO_PIN_PA09,
    [SERVICE_V2_TOUCH_CHANNEL_PA13] = TOUCH_GPIO_PIN_PA13,
};

static int service_error_from_lisa(int error)
{
    switch (error) {
    case LISA_DEVICE_OK:
        return SERVICE_V2_SENSORS_OK;
    case LISA_DEVICE_ERR_INVALID:
    case LISA_DEVICE_ERR_RANGE:
        return SERVICE_V2_SENSORS_ERR_INVALID;
    case LISA_DEVICE_ERR_NOT_FOUND:
    case LISA_DEVICE_ERR_NOT_READY:
    case LISA_DEVICE_ERR_INIT_FAIL:
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    case LISA_DEVICE_ERR_TIMEOUT:
        return SERVICE_V2_SENSORS_ERR_TIMEOUT;
    case LISA_DEVICE_ERR_BUSY:
        return SERVICE_V2_SENSORS_ERR_BUSY;
    case LISA_DEVICE_ERR_NO_MEM:
        return SERVICE_V2_SENSORS_ERR_NO_MEMORY;
    case LISA_DEVICE_ERR_OVERFLOW:
        return SERVICE_V2_SENSORS_ERR_OVERFLOW;
    default:
        return SERVICE_V2_SENSORS_ERR_IO;
    }
}

static int service_lock(lisa_mutex_t *lock)
{
    if (!lock) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    return lisa_mutex_lock(lock, (int32_t)SERVICE_LOCK_TIMEOUT_MS) == 0 ? SERVICE_V2_SENSORS_OK
                                                                        : SERVICE_V2_SENSORS_ERR_BUSY;
}

static int configure_i2c_bus(const char *device_name, lisa_device_t **device)
{
    if (!device || !device_name) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    if (!*device) {
        *device = lisa_device_get(device_name);
    }
    if (!*device || !lisa_device_ready(*device)) {
        LISA_LOGE(TAG, "%s is not ready", device_name);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    /* Conservative 100 kHz works with the board's relatively weak pull-ups. */
    lisa_i2c_config_t config = LISA_I2C_CONFIG_DEFAULT();
    int ret = lisa_i2c_configure(*device, &config);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "%s configure failed: %d", device_name, ret);
        return service_error_from_lisa(ret);
    }

    return SERVICE_V2_SENSORS_OK;
}

/* -------------------------------------------------------------------------- */
/* TTP223                                                                     */
/* -------------------------------------------------------------------------- */

static void touch_irq_handler(uint32_t pin, void *user_data)
{
    (void)user_data;
    if (pin == TOUCH_GPIO_PIN_PA09) {
        s_touch.irq_pending = true;
    }
}

int service_v2_touch_init(void)
{
    if (s_touch.initialized) {
        return SERVICE_V2_SENSORS_OK;
    }

    if (!s_touch.dev) {
        s_touch.dev = lisa_device_get(TOUCH_GPIO_DEVICE);
    }
    if (!s_touch.dev || !lisa_device_ready(s_touch.dev)) {
        LISA_LOGE(TAG, "touch %s is not ready", TOUCH_GPIO_DEVICE);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    /* TTP223 already filters the electrode and exposes a stable latched output.
     * The ARCS GPIO debounce block needs an explicit debounce clock, which the
     * LISA GPIO flags API does not configure here; enabling it can freeze the
     * sampled input at its initial level. */
    for (uint32_t channel = 0U; channel < SERVICE_V2_TOUCH_CHANNEL_COUNT; ++channel) {
        uint8_t pin = s_touch_gpio_pins[channel];
        int ret = lisa_gpio_configure(s_touch.dev, pin, LISA_GPIO_INPUT);
        if (ret != LISA_DEVICE_OK) {
            LISA_LOGE(TAG, "touch PA%02u configure failed: %d", pin, ret);
            return service_error_from_lisa(ret);
        }
    }

    s_touch.irq_pending = false;
    s_touch.initialized = true;
    LISA_LOGI(TAG, "TTP223 ready: PA09/PA13 output-state polling");
    return SERVICE_V2_SENSORS_OK;
}

int service_v2_touch_read_channel(service_v2_touch_channel_t channel, bool *touched)
{
    if (!touched || (uint32_t)channel >= SERVICE_V2_TOUCH_CHANNEL_COUNT) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (!s_touch.initialized || !s_touch.dev) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    int level = lisa_gpio_read_pin(s_touch.dev, s_touch_gpio_pins[channel]);
    if (level < 0) {
        return service_error_from_lisa(level);
    }

    *touched = (level == LISA_GPIO_HIGH);
    return SERVICE_V2_SENSORS_OK;
}

int service_v2_touch_read(bool *touched)
{
    return service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, touched);
}

int service_v2_touch_set_irq_enabled(bool enable)
{
    int ret = service_v2_touch_init();
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    if (enable == s_touch.irq_enabled) {
        return SERVICE_V2_SENSORS_OK;
    }

    if (enable) {
        ret = lisa_gpio_configure_irq(s_touch.dev, TOUCH_GPIO_PIN_PA09, LISA_GPIO_IRQ_EDGE_BOTH, touch_irq_handler, NULL);
        if (ret == LISA_DEVICE_OK) {
            ret = lisa_gpio_enable_irq(s_touch.dev, TOUCH_GPIO_PIN_PA09);
        }
        if (ret != LISA_DEVICE_OK) {
            LISA_LOGE(TAG, "touch PA09 IRQ enable failed: %d", ret);
            return service_error_from_lisa(ret);
        }
        s_touch.irq_pending = false;
        s_touch.irq_enabled = true;
    } else {
        ret = lisa_gpio_disable_irq(s_touch.dev, TOUCH_GPIO_PIN_PA09);
        if (ret != LISA_DEVICE_OK) {
            return service_error_from_lisa(ret);
        }
        s_touch.irq_enabled = false;
        s_touch.irq_pending = false;
    }

    return SERVICE_V2_SENSORS_OK;
}

int service_v2_touch_take_irq_event(bool *pending, bool *touched)
{
    if (!pending || !touched) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (!s_touch.initialized) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    *pending = s_touch.irq_pending;
    if (!*pending) {
        *touched = false;
        return SERVICE_V2_SENSORS_OK;
    }

    int ret = service_v2_touch_read(touched);
    if (ret == SERVICE_V2_SENSORS_OK) {
        s_touch.irq_pending = false;
    }
    return ret;
}

/* -------------------------------------------------------------------------- */
/* MPU6050                                                                    */
/* -------------------------------------------------------------------------- */

static int mpu6050_prepare(void)
{
    if (!s_mpu6050.lock) {
        s_mpu6050.lock = lisa_mutex_create();
        if (!s_mpu6050.lock) {
            return SERVICE_V2_SENSORS_ERR_NO_MEMORY;
        }
    }

    if (!s_mpu6050.bus_ready) {
        int ret = configure_i2c_bus(MPU6050_I2C_DEVICE, &s_mpu6050.i2c_dev);
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
        s_mpu6050.bus_ready = true;
    }

    return SERVICE_V2_SENSORS_OK;
}

static int mpu6050_write_register_locked(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    int ret = lisa_i2c_write(s_mpu6050.i2c_dev, MPU6050_I2C_ADDRESS, data, sizeof(data));
    return service_error_from_lisa(ret);
}

static int mpu6050_read_registers_locked(uint8_t reg, uint8_t *data, uint16_t length)
{
    if (!data || length == 0U) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    lisa_i2c_msg_t messages[2] = {
        {
            .addr = MPU6050_I2C_ADDRESS,
            .flags = LISA_I2C_FLAG_NO_STOP,
            .len = 1U,
            .buf = &reg,
        },
        {
            .addr = MPU6050_I2C_ADDRESS,
            .flags = LISA_I2C_FLAG_READ,
            .len = length,
            .buf = data,
        },
    };

    int ret = lisa_i2c_transfer(s_mpu6050.i2c_dev, messages, 2U);
    return service_error_from_lisa(ret);
}

static void mpu6050_irq_handler(uint32_t pin, void *user_data)
{
    (void)user_data;
    if (pin == MPU6050_INT_GPIO_PIN) {
        s_mpu6050.data_ready = true;
    }
}

static int mpu6050_set_irq_locked(bool enable)
{
    int ret;

    if (enable == s_mpu6050.irq_enabled) {
        return SERVICE_V2_SENSORS_OK;
    }

    if (!enable) {
        ret = mpu6050_write_register_locked(MPU6050_REG_INT_ENABLE, 0x00U);
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
        if (s_mpu6050.irq_gpio_dev) {
            int gpio_ret = lisa_gpio_disable_irq(s_mpu6050.irq_gpio_dev, MPU6050_INT_GPIO_PIN);
            if (gpio_ret != LISA_DEVICE_OK) {
                return service_error_from_lisa(gpio_ret);
            }
        }
        s_mpu6050.irq_enabled = false;
        s_mpu6050.data_ready = false;
        return SERVICE_V2_SENSORS_OK;
    }

    if (!s_mpu6050.irq_gpio_dev) {
        s_mpu6050.irq_gpio_dev = lisa_device_get(MPU6050_INT_GPIO_DEVICE);
    }
    if (!s_mpu6050.irq_gpio_dev || !lisa_device_ready(s_mpu6050.irq_gpio_dev)) {
        LISA_LOGE(TAG, "MPU6050 INT %s is not ready", MPU6050_INT_GPIO_DEVICE);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    ret = lisa_gpio_configure(s_mpu6050.irq_gpio_dev, MPU6050_INT_GPIO_PIN, LISA_GPIO_INPUT);
    if (ret == LISA_DEVICE_OK) {
        ret = lisa_gpio_configure_irq(s_mpu6050.irq_gpio_dev, MPU6050_INT_GPIO_PIN, LISA_GPIO_IRQ_EDGE_RISING,
                                      mpu6050_irq_handler, NULL);
    }
    if (ret == LISA_DEVICE_OK) {
        ret = lisa_gpio_enable_irq(s_mpu6050.irq_gpio_dev, MPU6050_INT_GPIO_PIN);
    }
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "MPU6050 PA12 IRQ configure failed: %d", ret);
        return service_error_from_lisa(ret);
    }

    s_mpu6050.data_ready = false;
    ret = mpu6050_write_register_locked(MPU6050_REG_INT_ENABLE, 0x01U);
    if (ret != SERVICE_V2_SENSORS_OK) {
        lisa_gpio_disable_irq(s_mpu6050.irq_gpio_dev, MPU6050_INT_GPIO_PIN);
        return ret;
    }

    s_mpu6050.irq_enabled = true;
    return SERVICE_V2_SENSORS_OK;
}

int service_v2_mpu6050_who_am_i(uint8_t *who_am_i)
{
    if (!who_am_i) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    int ret = mpu6050_prepare();
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    ret = service_lock(s_mpu6050.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    ret = mpu6050_read_registers_locked(MPU6050_REG_WHO_AM_I, who_am_i, 1U);
    lisa_mutex_unlock(s_mpu6050.lock);
    return ret;
}

int service_v2_mpu6050_init(bool enable_data_ready_irq)
{
    int ret = mpu6050_prepare();
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    ret = service_lock(s_mpu6050.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    if (!s_mpu6050.initialized) {
        uint8_t who_am_i = 0U;
        ret = mpu6050_read_registers_locked(MPU6050_REG_WHO_AM_I, &who_am_i, 1U);
        if (ret != SERVICE_V2_SENSORS_OK) {
            LISA_LOGE(TAG, "MPU6050 WHO_AM_I read failed: %d", ret);
            goto out;
        }
        if (who_am_i != MPU6050_WHO_AM_I_VALUE) {
            LISA_LOGE(TAG, "MPU6050 WHO_AM_I mismatch: 0x%02x", who_am_i);
            ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
            goto out;
        }

        ret = mpu6050_write_register_locked(MPU6050_REG_PWR_MGMT_1, 0x80U);
        if (ret != SERVICE_V2_SENSORS_OK) {
            goto out;
        }
        lisa_thread_mdelay(MPU6050_RESET_DELAY_MS);

        ret = mpu6050_write_register_locked(MPU6050_REG_PWR_MGMT_1, 0x01U);
        if (ret != SERVICE_V2_SENSORS_OK) {
            goto out;
        }
        lisa_thread_mdelay(MPU6050_WAKE_DELAY_MS);

        /* 1 kHz / (1 + 4) = 200 Hz when DLPF is enabled. */
        ret = mpu6050_write_register_locked(MPU6050_REG_SMPLRT_DIV, 0x04U);
        if (ret == SERVICE_V2_SENSORS_OK) {
            ret = mpu6050_write_register_locked(MPU6050_REG_CONFIG, 0x03U);
        }
        if (ret == SERVICE_V2_SENSORS_OK) {
            ret = mpu6050_write_register_locked(MPU6050_REG_GYRO_CONFIG, 0x00U);
        }
        if (ret == SERVICE_V2_SENSORS_OK) {
            ret = mpu6050_write_register_locked(MPU6050_REG_ACCEL_CONFIG, 0x00U);
        }
        if (ret == SERVICE_V2_SENSORS_OK) {
            /* Active-high, push-pull; any register read clears data-ready. */
            ret = mpu6050_write_register_locked(MPU6050_REG_INT_PIN_CFG, 0x10U);
        }
        if (ret == SERVICE_V2_SENSORS_OK) {
            ret = mpu6050_write_register_locked(MPU6050_REG_INT_ENABLE, 0x00U);
        }
        if (ret != SERVICE_V2_SENSORS_OK) {
            LISA_LOGE(TAG, "MPU6050 configuration failed: %d", ret);
            goto out;
        }

        s_mpu6050.initialized = true;
        LISA_LOGI(TAG, "MPU6050 ready: i2c0 addr=0x68");
    }

    ret = mpu6050_set_irq_locked(enable_data_ready_irq);

out:
    lisa_mutex_unlock(s_mpu6050.lock);
    return ret;
}

int service_v2_mpu6050_set_sleep(bool sleep)
{
    if (!s_mpu6050.initialized) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    int ret = service_lock(s_mpu6050.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (s_mpu6050.sleeping == sleep) {
        lisa_mutex_unlock(s_mpu6050.lock);
        return SERVICE_V2_SENSORS_OK;
    }

    if (sleep) {
        ret = mpu6050_set_irq_locked(false);
        if (ret == SERVICE_V2_SENSORS_OK) {
            ret = mpu6050_write_register_locked(MPU6050_REG_PWR_MGMT_1, 0x41U);
        }
    } else {
        ret = mpu6050_write_register_locked(MPU6050_REG_PWR_MGMT_1, 0x01U);
        if (ret == SERVICE_V2_SENSORS_OK) {
            lisa_thread_mdelay(MPU6050_WAKE_DELAY_MS);
        }
    }
    if (ret == SERVICE_V2_SENSORS_OK) {
        s_mpu6050.sleeping = sleep;
        s_mpu6050.data_ready = false;
    }
    lisa_mutex_unlock(s_mpu6050.lock);
    return ret;
}

static int16_t decode_be16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

int service_v2_mpu6050_read_raw(service_v2_mpu6050_raw_t *sample)
{
    if (!sample) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (!s_mpu6050.initialized || s_mpu6050.sleeping) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    int ret = service_lock(s_mpu6050.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    uint8_t raw[14];
    ret = mpu6050_read_registers_locked(MPU6050_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    if (ret == SERVICE_V2_SENSORS_OK) {
        sample->accel_x = decode_be16(&raw[0]);
        sample->accel_y = decode_be16(&raw[2]);
        sample->accel_z = decode_be16(&raw[4]);
        sample->temperature = decode_be16(&raw[6]);
        sample->gyro_x = decode_be16(&raw[8]);
        sample->gyro_y = decode_be16(&raw[10]);
        sample->gyro_z = decode_be16(&raw[12]);
        s_mpu6050.data_ready = false;
    }

    lisa_mutex_unlock(s_mpu6050.lock);
    return ret;
}

int service_v2_mpu6050_take_data_ready(bool *pending)
{
    if (!pending) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (!s_mpu6050.initialized) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    *pending = s_mpu6050.data_ready;
    s_mpu6050.data_ready = false;
    return SERVICE_V2_SENSORS_OK;
}

/* -------------------------------------------------------------------------- */
/* PN532                                                                      */
/* -------------------------------------------------------------------------- */

static int pn532_prepare(void)
{
    if (!s_pn532.lock) {
        s_pn532.lock = lisa_mutex_create();
        if (!s_pn532.lock) {
            return SERVICE_V2_SENSORS_ERR_NO_MEMORY;
        }
    }

    if (!s_pn532.bus_ready) {
        int ret = configure_i2c_bus(PN532_I2C_DEVICE, &s_pn532.i2c_dev);
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
        s_pn532.bus_ready = true;
    }

    return SERVICE_V2_SENSORS_OK;
}

/*
 * Use the generic transfer path for every PN532 transaction.  The ARCS
 * lisa_i2c read/write helpers do not drain a completion semaphore left by
 * their interrupt fast path, so a following transaction can consume the old
 * completion and report a false timeout.  lisa_i2c_transfer() drains that
 * semaphore before starting each message and is also the path used by the
 * working MPU6050 driver above.
 */
static int pn532_i2c_transfer_locked(uint8_t *data, uint32_t length, bool read)
{
    lisa_i2c_msg_t message = {
        .addr = PN532_I2C_ADDRESS,
        .flags = read ? LISA_I2C_FLAG_READ : LISA_I2C_FLAG_NONE,
        .len = length,
        .buf = data,
    };

    int ret = lisa_i2c_transfer(s_pn532.i2c_dev, &message, 1U);
    if (ret == LISA_DEVICE_ERR_TIMEOUT) {
        /* wait_for_transfer() has already issued ABORT_TRANSFER and
         * BUS_CLEAR. Re-apply the I2C configuration before the next host
         * transaction: the ARCS controller otherwise occasionally remains
         * unusable after PN532's Power-Down clock stretching. Do not retry
         * this transfer here. A timed-out frame read may already have sent a
         * STOP mid-frame, and replaying it would parse a truncated PN532
         * response as a new one. */
        int recover_ret = configure_i2c_bus(PN532_I2C_DEVICE, &s_pn532.i2c_dev);
        if (recover_ret != SERVICE_V2_SENSORS_OK) {
            s_pn532.bus_ready = false;
            LISA_LOGE(TAG, "PN532 I2C recovery configure failed: %d", recover_ret);
        } else {
            s_pn532.bus_ready = true;
            lisa_thread_mdelay(PN532_I2C_RECOVERY_SETTLE_MS);
            LISA_LOGW(TAG, "PN532 I2C controller recovered after timeout");
        }
    }
    return ret;
}

static int pn532_i2c_read_conditional_locked(uint8_t *data, uint32_t length)
{
    int ret =
        lisa_i2c_read_conditional(s_pn532.i2c_dev, PN532_I2C_ADDRESS, data, length, PN532_I2C_READY, PN532_I2C_READY);
    if (ret == LISA_DEVICE_ERR_TIMEOUT) {
        /* Keep timeout recovery identical to the ordinary transfer path.
         * The conditional primitive has already NACKed/stopped or aborted
         * the transaction before it returns. Never replay a partial frame. */
        int recover_ret = configure_i2c_bus(PN532_I2C_DEVICE, &s_pn532.i2c_dev);
        if (recover_ret != SERVICE_V2_SENSORS_OK) {
            s_pn532.bus_ready = false;
            LISA_LOGE(TAG, "PN532 I2C recovery configure failed: %d", recover_ret);
        } else {
            s_pn532.bus_ready = true;
            lisa_thread_mdelay(PN532_I2C_RECOVERY_SETTLE_MS);
            LISA_LOGW(TAG, "PN532 I2C controller recovered after timeout");
        }
    }
    return ret;
}

/* Keep a stalled host-I2C transaction distinct from the bounded wait for a
 * PN532 RF/command response. Release cleanup may tolerate the latter only
 * after the application exchange completed; transport timeouts remain hard
 * I/O failures. */
static int pn532_transport_error_from_lisa(int error)
{
    int mapped = service_error_from_lisa(error);

    return mapped == SERVICE_V2_SENSORS_ERR_TIMEOUT ? SERVICE_V2_SENSORS_ERR_IO : mapped;
}

/* UM0701-02 section 6.2.4 requires the ready status and response frame to be
 * read in one I2C transaction.  RDY=0 must be followed by NACK+STOP; RDY=1
 * must be ACKed and the frame drained before STOP.  The ARCS conditional-read
 * primitive performs that decision while SCL is held after the first byte. */
static int pn532_read_wire_packet_locked(uint8_t *wire_data, uint32_t wire_length, uint32_t timeout_ms)
{
    if (!wire_data || wire_length < 2U || timeout_ms == 0U) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0U) {
        timeout_ticks = 1U;
    }

    for (;;) {
        memset(wire_data, 0, wire_length);
        int ret = pn532_i2c_read_conditional_locked(wire_data, wire_length);
        if (ret == LISA_DEVICE_OK) {
            return SERVICE_V2_SENSORS_OK;
        }

        /* RDY=0 and a transient address NACK both mean "not ready yet".
         * A controller timeout or any other transport failure remains a hard
         * I/O error and is never hidden as an RF response timeout. */
        if (ret != LISA_DEVICE_ERR_NOT_READY && ret != LISA_DEVICE_ERR_NACK) {
            return pn532_transport_error_from_lisa(ret);
        }

        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            return SERVICE_V2_SENSORS_ERR_TIMEOUT;
        }
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
    }
}

static int pn532_validate_ack(const uint8_t *wire_data, size_t wire_length)
{
    static const uint8_t ack[] = {
        PN532_PREAMBLE, PN532_START_CODE_1, PN532_START_CODE_2, 0x00U, 0xFFU, 0x00U,
    };

    if (!wire_data || wire_length != PN532_ACK_WIRE_LEN || (wire_data[0] & PN532_I2C_READY) == 0U) {
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    return memcmp(&wire_data[1], ack, sizeof(ack)) == 0 ? SERVICE_V2_SENSORS_OK : SERVICE_V2_SENSORS_ERR_PROTOCOL;
}

static int pn532_abort_pending_command_locked(void)
{
    /* UM0701-02 section 6.2.2: writing an ACK frame aborts the command that
     * is currently waiting for an external RF event.  PN532 sends no reply. */
    uint8_t abort_ack[] = {
        PN532_PREAMBLE, PN532_START_CODE_1, PN532_START_CODE_2, 0x00U, 0xFFU, 0x00U,
    };

    TickType_t write_start = xTaskGetTickCount();
    TickType_t write_retry_ticks = pdMS_TO_TICKS(PN532_COMMAND_WRITE_RETRY_MS);
    /* Keep this safe if a downstream board lowers configTICK_RATE_HZ. */
    // cppcheck-suppress knownConditionTrueFalse
    if (write_retry_ticks == 0U) {
        write_retry_ticks = 1U;
    }

    int ret;
    do {
        ret = pn532_i2c_transfer_locked(abort_ack, sizeof(abort_ack), false);
        if (ret == LISA_DEVICE_OK || ret != LISA_DEVICE_ERR_NACK) {
            break;
        }
        lisa_thread_mdelay(1U);
    } while ((xTaskGetTickCount() - write_start) < write_retry_ticks);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGW(TAG, "PN532 command abort failed: %d", ret);
        return pn532_transport_error_from_lisa(ret);
    }
    return SERVICE_V2_SENSORS_OK;
}

static int pn532_parse_response(const uint8_t *wire_data, size_t wire_length, uint8_t command, uint8_t *response,
                                size_t response_capacity, size_t *response_length)
{
    if (!wire_data || !response_length || wire_length < 9U || (wire_data[0] & PN532_I2C_READY) == 0U) {
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    const uint8_t *frame = &wire_data[1];
    size_t frame_available = wire_length - 1U;
    size_t start = frame_available;

    for (size_t i = 0U; i + 2U < frame_available; ++i) {
        if (frame[i] == PN532_PREAMBLE && frame[i + 1U] == PN532_START_CODE_1 && frame[i + 2U] == PN532_START_CODE_2) {
            start = i;
            break;
        }
    }
    if (start == frame_available || start + 7U > frame_available) {
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    uint8_t length = frame[start + 3U];
    uint8_t length_checksum = frame[start + 4U];
    if (length == 0xFFU || length == 0U || (uint8_t)(length + length_checksum) != 0U) {
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    size_t complete_frame_length = 7U + (size_t)length;
    if (start + complete_frame_length > frame_available) {
        return SERVICE_V2_SENSORS_ERR_OVERFLOW;
    }

    const uint8_t *payload = &frame[start + 5U];
    uint8_t checksum = frame[start + 5U + (size_t)length];
    uint8_t sum = checksum;
    for (size_t i = 0U; i < (size_t)length; ++i) {
        sum = (uint8_t)(sum + payload[i]);
    }
    if (sum != 0U || frame[start + 6U + (size_t)length] != 0x00U) {
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    /* PN532's protocol error frame has LEN=1 and payload 0x7F. */
    if (length == 1U && payload[0] == 0x7FU) {
        return SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
    }
    if (length < 2U || payload[0] != PN532_PN532_TO_HOST || payload[1] != (uint8_t)(command + 1U)) {
        return SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
    }

    size_t data_length = (size_t)length - 2U;
    if (data_length > response_capacity) {
        return SERVICE_V2_SENSORS_ERR_OVERFLOW;
    }
    if (data_length > 0U) {
        if (!response) {
            return SERVICE_V2_SENSORS_ERR_INVALID;
        }
        memcpy(response, &payload[2], data_length);
    }
    *response_length = data_length;
    return SERVICE_V2_SENSORS_OK;
}

static int pn532_send_command_locked(uint8_t command, const uint8_t *parameters, size_t parameter_length,
                                     uint32_t timeout_ms)
{
    if (parameter_length > (PN532_COMMAND_FRAME_MAX - 9U) || (parameter_length > 0U && !parameters) ||
        timeout_ms == 0U) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    uint8_t frame[PN532_COMMAND_FRAME_MAX];
    size_t frame_index = 0U;
    uint8_t payload_length = (uint8_t)(parameter_length + 2U);
    uint8_t payload_sum = (uint8_t)(PN532_HOST_TO_PN532 + command);

    frame[frame_index++] = PN532_PREAMBLE;
    frame[frame_index++] = PN532_START_CODE_1;
    frame[frame_index++] = PN532_START_CODE_2;
    frame[frame_index++] = payload_length;
    frame[frame_index++] = (uint8_t)(0U - payload_length);
    frame[frame_index++] = PN532_HOST_TO_PN532;
    frame[frame_index++] = command;
    for (size_t i = 0U; i < parameter_length; ++i) {
        frame[frame_index++] = parameters[i];
        payload_sum = (uint8_t)(payload_sum + parameters[i]);
    }
    frame[frame_index++] = (uint8_t)(0U - payload_sum);
    frame[frame_index++] = 0x00U;

    TickType_t write_start = xTaskGetTickCount();
    TickType_t write_retry_ticks = pdMS_TO_TICKS(PN532_COMMAND_WRITE_RETRY_MS);
    /* Keep this safe if a downstream board lowers configTICK_RATE_HZ. */
    // cppcheck-suppress knownConditionTrueFalse
    if (write_retry_ticks == 0U) {
        write_retry_ticks = 1U;
    }
    int ret;
    do {
        ret = pn532_i2c_transfer_locked(frame, (uint32_t)frame_index, false);
        if (ret == LISA_DEVICE_OK) {
            break;
        }
        if (ret != LISA_DEVICE_ERR_NACK) {
            return pn532_transport_error_from_lisa(ret);
        }
        lisa_thread_mdelay(1U);
    } while ((xTaskGetTickCount() - write_start) < write_retry_ticks);
    if (ret != LISA_DEVICE_OK) {
        return pn532_transport_error_from_lisa(ret);
    }

    uint8_t ack[PN532_ACK_WIRE_LEN];
    uint32_t ack_timeout = timeout_ms < 250U ? timeout_ms : 250U;
    ret = pn532_read_wire_packet_locked(ack, sizeof(ack), ack_timeout);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    ret = pn532_validate_ack(ack, sizeof(ack));
    return ret;
}

static int pn532_exchange_locked(uint8_t command, const uint8_t *parameters, size_t parameter_length, uint8_t *response,
                                 size_t response_capacity, size_t *response_length, uint32_t response_wire_length,
                                 uint32_t timeout_ms)
{
    if (!response_length || response_wire_length > PN532_RESPONSE_WIRE_MAX || response_wire_length < 9U ||
        timeout_ms == 0U) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    int ret = pn532_send_command_locked(command, parameters, parameter_length, timeout_ms);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    uint8_t wire_response[PN532_RESPONSE_WIRE_MAX];
    ret = pn532_read_wire_packet_locked(wire_response, response_wire_length, timeout_ms);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    return pn532_parse_response(wire_response, response_wire_length, command, response, response_capacity,
                                response_length);
}

static int pn532_get_firmware_locked(service_v2_pn532_firmware_t *firmware)
{
    uint8_t response[4];
    size_t response_length = sizeof(response);
    int ret = pn532_exchange_locked(PN532_CMD_GET_FIRMWARE_VERSION, NULL, 0U, response, sizeof(response),
                                    &response_length, 14U, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (response_length != sizeof(response)) {
        return SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
    }

    firmware->ic = response[0];
    firmware->version = response[1];
    firmware->revision = response[2];
    firmware->support = response[3];
    return SERVICE_V2_SENSORS_OK;
}

static int pn532_enter_sam_normal_locked(void)
{
    static const uint8_t sam_config[] = {0x01U};
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    int ret = pn532_exchange_locked(PN532_CMD_SAM_CONFIGURATION, sam_config, sizeof(sam_config), response,
                                    sizeof(response), &response_length, 10U, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    return response_length == 0U ? SERVICE_V2_SENSORS_OK : SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
}

static int pn532_keep_cpu_awake_locked(void)
{
    /* UM0701-02 sections 3.1.3.2 and 7.2.5: bCPUPowerMode at XRAM
     * 0x02FC selects the CPU mode used in Standby. Keep it in Normal mode so
     * the first host command after a released target session does not depend
     * on a Power-Down wake-up/clock-stretch cycle. TgInitAsTarget may still
     * enter Power Down while it waits for an RF field, as required by PN532. */
    static const uint8_t write_parameters[] = {
        PN532_XRAM_CPU_POWER_MODE_H,
        PN532_XRAM_CPU_POWER_MODE_L,
        PN532_CPU_POWER_MODE_NORMAL,
    };
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    int ret = pn532_exchange_locked(PN532_CMD_WRITE_REGISTER, write_parameters, sizeof(write_parameters), response,
                                    sizeof(response), &response_length, 10U, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    return response_length == 0U ? SERVICE_V2_SENSORS_OK : SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
}

static int pn532_configure_reader_retries_locked(void)
{
    /* One passive activation attempt: no-card discovery remains bounded. */
    static const uint8_t retry_config[] = {
        0x05U, /* CfgItem: MaxRetries */
        0x00U, /* MxRtyATR: one try */
        0x01U, /* MxRtyPSL: two tries */
        0x00U, /* MxRtyPassiveActivation: one try */
    };
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    int ret = pn532_exchange_locked(PN532_CMD_RF_CONFIGURATION, retry_config, sizeof(retry_config), response,
                                    sizeof(response), &response_length, 10U, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    return response_length == 0U ? SERVICE_V2_SENSORS_OK : SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
}

static int pn532_resynchronize_reader_locked(bool abort_pending)
{
    /* UM0701-02 sections 3.1.3.8 and 6.3.3.1 require SAMConfiguration
     * Normal to be the first command after reset/power-up: PN532 starts in
     * LowVbat and clock-stretches an I2C host that sends an ordinary command.
     * A raw ACK is therefore not a safe unconditional first transaction.
     *
     * Try the mandatory wake/synchronization command first.  Only an ARCS-only
     * reboot can leave a normal-mode RF command pending; in that case use the
     * ACK abort as a fallback and retry SAMConfiguration. */
    int ret = pn532_enter_sam_normal_locked();
    if (ret != SERVICE_V2_SENSORS_OK && abort_pending) {
        LISA_LOGW(TAG, "PN532 SAM wake/sync failed: %d; trying pending-command abort", ret);
        int abort_ret = pn532_abort_pending_command_locked();
        if (abort_ret == SERVICE_V2_SENSORS_OK) {
            lisa_thread_mdelay(PN532_RESYNC_SETTLE_MS);
            ret = pn532_enter_sam_normal_locked();
        } else {
            LISA_LOGW(TAG, "PN532 pending-command abort failed: %d", abort_ret);
        }
    }
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGE(TAG, "PN532 SAM Normal sync failed: %d", ret);
        return ret;
    }

    ret = pn532_keep_cpu_awake_locked();
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGE(TAG, "PN532 standby Normal-mode config failed: %d", ret);
        return ret;
    }

    service_v2_pn532_firmware_t firmware = {0};
    ret = pn532_get_firmware_locked(&firmware);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGE(TAG, "PN532 GetFirmware sync failed: %d", ret);
        return ret;
    }
    if (firmware.ic != 0x32U) {
        LISA_LOGE(TAG, "PN532 IC mismatch: 0x%02x", firmware.ic);
        return SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
    }

    ret = pn532_configure_reader_retries_locked();
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGE(TAG, "PN532 reader retry config failed: %d", ret);
        return ret;
    }

    s_pn532.firmware = firmware;
    s_pn532.firmware_valid = true;
    return SERVICE_V2_SENSORS_OK;
}

static void pn532_log_strap_hint(int error)
{
    LISA_LOGE(TAG,
              "PN532 init failed: %d; API addr is 7-bit 0x24 on shared i2c0. "
              "Check the fixed I2C-mode wiring (I0=0/I1=1)",
              error);
}

int service_v2_pn532_get_firmware_version(service_v2_pn532_firmware_t *firmware)
{
    if (!firmware) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    int ret = pn532_prepare();
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    ret = service_lock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    ret = pn532_get_firmware_locked(firmware);
    lisa_mutex_unlock(s_pn532.lock);
    return ret;
}

int service_v2_pn532_init(void)
{
    int ret = pn532_prepare();
    if (ret != SERVICE_V2_SENSORS_OK) {
        pn532_log_strap_hint(ret);
        return ret;
    }
    ret = service_lock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    if (s_pn532.initialized) {
        lisa_mutex_unlock(s_pn532.lock);
        return SERVICE_V2_SENSORS_OK;
    }

    ret = pn532_resynchronize_reader_locked(true);
    if (ret != SERVICE_V2_SENSORS_OK) {
        pn532_log_strap_hint(ret);
        goto out;
    }

    s_pn532.initialized = true;
    LISA_LOGI(TAG, "PN532 ready: shared i2c0 7-bit addr=0x24 fw=%u.%u", s_pn532.firmware.version,
              s_pn532.firmware.revision);

out:
    lisa_mutex_unlock(s_pn532.lock);
    return ret;
}

static int pn532_parse_type_a_target_response(const uint8_t *response, size_t response_length,
                                              service_v2_pn532_target_t *target)
{
    if (!response || !target || response_length < 1U) {
        return SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
    }
    if (response[0] == 0U) {
        return SERVICE_V2_SENSORS_ERR_NO_TARGET;
    }
    if (response[0] != 1U || response_length < 6U) {
        return SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
    }

    uint8_t uid_length = response[5];
    if (uid_length == 0U || uid_length > SERVICE_V2_PN532_UID_MAX_LEN) {
        return SERVICE_V2_SENSORS_ERR_OVERFLOW;
    }
    size_t cursor = 6U;
    if (cursor + uid_length > response_length) {
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    memset(target, 0, sizeof(*target));
    target->target_number = response[1];
    target->sens_res = (uint16_t)(((uint16_t)response[2] << 8) | (uint16_t)response[3]);
    target->sel_res = response[4];
    target->uid_len = uid_length;
    memcpy(target->uid, &response[cursor], uid_length);
    cursor += uid_length;

    if (cursor < response_length) {
        uint8_t ats_length = response[cursor];
        if (ats_length == 0U || ats_length > SERVICE_V2_PN532_ATS_MAX_LEN) {
            return SERVICE_V2_SENSORS_ERR_OVERFLOW;
        }
        if (cursor + ats_length > response_length) {
            return SERVICE_V2_SENSORS_ERR_PROTOCOL;
        }
        target->ats_len = ats_length;
        memcpy(target->ats, &response[cursor], ats_length);
    }

    return SERVICE_V2_SENSORS_OK;
}

int service_v2_pn532_list_passive_target(service_v2_pn532_target_t *target, uint32_t timeout_ms)
{
    if (!target || timeout_ms > SERVICE_V2_PN532_MAX_TIMEOUT_MS) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (timeout_ms == 0U) {
        timeout_ms = SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS;
    }

    memset(target, 0, sizeof(*target));
    int ret = service_lock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (!s_pn532.initialized) {
        lisa_mutex_unlock(s_pn532.lock);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    /* Ensure a previous target-mode diagnostic cannot leave automatic RATS
     * disabled when this PN532 returns to initiator/reader mode. */
    static const uint8_t set_parameters[] = {0x34U};
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    ret = pn532_exchange_locked(PN532_CMD_SET_PARAMETERS, set_parameters, sizeof(set_parameters), response,
                                sizeof(response), &response_length, 10U, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK || response_length != 0U) {
        s_pn532.initialized = false;
        int abort_ret = pn532_abort_pending_command_locked();
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
        lisa_mutex_unlock(s_pn532.lock);
        if (abort_ret != SERVICE_V2_SENSORS_OK) {
            return abort_ret;
        }
        return ret == SERVICE_V2_SENSORS_OK ? SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE : ret;
    }

    static const uint8_t parameters[] = {
        0x01U, /* MaxTg */
        PN532_BRTY_ISO14443A_106_KBPS,
    };
    response_length = sizeof(response);
    ret = pn532_exchange_locked(PN532_CMD_IN_LIST_PASSIVE_TARGET, parameters, sizeof(parameters), response,
                                sizeof(response), &response_length, PN532_RESPONSE_WIRE_MAX, timeout_ms);
    bool needs_reinit = false;

    /* InListPassiveTarget leaves a successfully selected PICC active. Release
     * it with the command defined for initiator mode (Tg=0 means all targets),
     * so repeated peer-discovery role changes do not leave the RF session or
     * field active. A failed release is propagated because it leaves both
     * peers' RF state uncertain; the next loop re-enters SAM Normal. */
    if (ret == SERVICE_V2_SENSORS_OK && response_length >= 2U && response[0] > 0U) {
        static const uint8_t release_parameters[] = {0x00U};
        uint8_t release_response[1] = {0xFFU};
        size_t release_response_length = sizeof(release_response);
        int release_ret = pn532_exchange_locked(PN532_CMD_IN_RELEASE, release_parameters, sizeof(release_parameters),
                                                release_response, sizeof(release_response), &release_response_length,
                                                11U, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
        if (release_ret != SERVICE_V2_SENSORS_OK || release_response_length != 1U ||
            (release_response[0] & 0x3FU) != 0U) {
            needs_reinit = true;
            LISA_LOGW(TAG, "PN532 InRelease failed: ret=%d status=0x%02x", release_ret,
                      release_response_length == 1U ? release_response[0] : 0xFFU);
            ret = release_ret != SERVICE_V2_SENSORS_OK ? release_ret : SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
        } else {
            /* Allow the target to process S(DESELECT) and the reader to turn
             * off its RF field before this PN532 starts another role. */
            lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
        }
    } else if (ret != SERVICE_V2_SENSORS_OK) {
        /* A host timeout can leave InListPassiveTarget pending. */
        int abort_ret = pn532_abort_pending_command_locked();
        if (abort_ret != SERVICE_V2_SENSORS_OK) {
            ret = abort_ret;
        } else if (ret == SERVICE_V2_SENSORS_ERR_TIMEOUT) {
            /* InListPassiveTarget normally remains pending while no RF
             * target is present.  If the host timeout was followed by a
             * successful protocol-level abort, this is a clean discovery
             * miss rather than an I2C fault.  Report NO_TARGET so the peer
             * worker continues its reader window instead of backing off for
             * one second after every empty 80 ms poll. */
            ret = SERVICE_V2_SENSORS_ERR_NO_TARGET;
        }
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
        needs_reinit = true;
    }

    /* A clean no-target response or successful InRelease leaves the existing
     * SAM/RF configuration usable. Reinitialize only after an uncertain
     * timeout/abort or a failed release. */
    if (needs_reinit) {
        s_pn532.initialized = false;
    }
    lisa_mutex_unlock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    return pn532_parse_type_a_target_response(response, response_length, target);
}

static int pn532_configure_type_a_target_locked(void)
{
    /* Automatic ATR_RES/RATS and ISO/IEC 14443-4 PICC emulation enabled,
     * without DID/NAD. */
    static const uint8_t set_parameters[] = {0x34U};
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    int ret = pn532_exchange_locked(PN532_CMD_SET_PARAMETERS, set_parameters, sizeof(set_parameters), response,
                                    sizeof(response), &response_length, 10U, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    return response_length == 0U ? SERVICE_V2_SENSORS_OK : SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
}

static size_t pn532_build_type_a_target_parameters(const uint8_t nfcid1t[3], const uint8_t *historical_bytes,
                                                   uint8_t historical_bytes_len,
                                                   uint8_t target_parameters[PN532_COMMAND_FRAME_MAX - 9U])
{
    size_t target_parameter_length = 0U;

    target_parameters[target_parameter_length++] = PN532_TARGET_MODE_PASSIVE_ONLY | PN532_TARGET_MODE_PICC_ONLY;
    target_parameters[target_parameter_length++] = 0x04U;
    target_parameters[target_parameter_length++] = 0x00U;
    memcpy(&target_parameters[target_parameter_length], nfcid1t, 3U);
    target_parameter_length += 3U;
    target_parameters[target_parameter_length++] = PN532_TARGET_SEL_RES_ISO14443_4;

    static const uint8_t felica_and_nfcid3[] = {
        0x01U, 0xFEU, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0xA6U, 0xA7U, 0xC0U, 0xC1U, 0xC2U, 0xC3U, 0xC4U, 0xC5U,
        0xC6U, 0xC7U, 0xFFU, 0xFFU, 0xAAU, 0x99U, 0x88U, 0x77U, 0x66U, 0x55U, 0x44U, 0x33U, 0x22U, 0x11U,
    };
    memcpy(&target_parameters[target_parameter_length], felica_and_nfcid3, sizeof(felica_and_nfcid3));
    target_parameter_length += sizeof(felica_and_nfcid3);
    target_parameters[target_parameter_length++] = 0x00U; /* GtLen */
    target_parameters[target_parameter_length++] = historical_bytes_len;
    if (historical_bytes_len > 0U) {
        memcpy(&target_parameters[target_parameter_length], historical_bytes, historical_bytes_len);
        target_parameter_length += historical_bytes_len;
    }
    return target_parameter_length;
}

static int pn532_finish_type_a_target_window_locked(bool *command_settled, bool *activated, uint8_t *activation_mode,
                                                    uint32_t timeout_ms)
{
    if (!command_settled || !activated || timeout_ms == 0U) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    *command_settled = false;
    *activated = false;
    if (activation_mode) {
        *activation_mode = 0U;
    }

    /* Keep AutoColl armed for the whole bounded window. Conditional reads
     * poll RDY without splitting a ready frame: an activation is drained as
     * soon as it arrives, while an idle target remains armed until timeout. */
    uint8_t wire_response[PN532_RESPONSE_WIRE_MAX];
    int ret = pn532_read_wire_packet_locked(wire_response, sizeof(wire_response), timeout_ms);
    if (ret == SERVICE_V2_SENSORS_ERR_TIMEOUT) {
        int abort_ret = pn532_abort_pending_command_locked();
        *command_settled = abort_ret == SERVICE_V2_SENSORS_OK;
        return abort_ret;
    }
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    /* Reading the response has completed the pending command even if its
     * contents later fail validation, so do not send a second abort frame. */
    *command_settled = true;
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    ret = pn532_parse_response(wire_response, sizeof(wire_response), PN532_CMD_TG_INIT_AS_TARGET, response,
                               sizeof(response), &response_length);
    if (ret == SERVICE_V2_SENSORS_OK && response_length >= 1U) {
        *activated = true;
        if (activation_mode) {
            *activation_mode = response[0];
        }
    } else if (ret == SERVICE_V2_SENSORS_OK) {
        ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
    }
    return ret;
}

static int pn532_receive_command_response_locked(uint8_t command, uint8_t *response, size_t response_capacity,
                                                 size_t *response_length, uint32_t timeout_ms,
                                                 bool *command_may_be_pending)
{
    if (!response_length || !command_may_be_pending || timeout_ms == 0U) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    uint8_t wire_response[PN532_RESPONSE_WIRE_MAX];
    int ret = pn532_read_wire_packet_locked(wire_response, sizeof(wire_response), timeout_ms);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    /* Once the wire response has been drained, the PN532 command is no
     * longer pending even if frame validation below rejects its contents. */
    *command_may_be_pending = false;
    return pn532_parse_response(wire_response, sizeof(wire_response), command, response, response_capacity,
                                response_length);
}

static int pn532_tracked_command_locked(uint8_t command, const uint8_t *parameters, size_t parameter_length,
                                        uint8_t *response, size_t response_capacity, size_t *response_length,
                                        uint32_t timeout_ms, bool *command_may_be_pending)
{
    if (!command_may_be_pending) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    /* Set this before sending: a timeout while reading the ACK can still
     * mean that PN532 accepted the command and is waiting for an RF event. */
    *command_may_be_pending = true;
    int ret = pn532_send_command_locked(command, parameters, parameter_length, timeout_ms);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }

    return pn532_receive_command_response_locked(command, response, response_capacity, response_length, timeout_ms,
                                                 command_may_be_pending);
}

static int pn532_type_a_status_error(uint8_t status)
{
    uint8_t error = status & 0x3FU;
    if (error == 0U) {
        /* This bounded Type-A API does not expose chained I-blocks or NAD.
         * SetParameters leaves fNADUsed disabled, so either flag is an
         * application/protocol mismatch rather than part of the payload. */
        if ((status & 0x40U) != 0U) {
            return SERVICE_V2_SENSORS_ERR_OVERFLOW;
        }
        return (status & 0x80U) == 0U ? SERVICE_V2_SENSORS_OK : SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    switch (error) {
    case 0x01U: /* RF timeout */
    case 0x0AU: /* RF field not activated in time */
    case 0x29U: /* target released by its initiator */
        return SERVICE_V2_SENSORS_ERR_NO_TARGET;
    case 0x07U: /* receive buffer too small */
    case 0x09U: /* RF receive buffer overflow */
    case 0x0EU: /* internal buffer overflow */
        return SERVICE_V2_SENSORS_ERR_OVERFLOW;
    case 0x10U: /* invalid parameter */
        return SERVICE_V2_SENSORS_ERR_INVALID;
    case 0x25U: /* invalid device state */
    case 0x27U: /* command not acceptable in the current context */
    default:
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }
}

static int pn532_type_a_finalize_locked(bool command_may_be_pending, int operation_result, bool session_released)
{
    if (command_may_be_pending) {
        int abort_ret = pn532_abort_pending_command_locked();
        if (abort_ret != SERVICE_V2_SENSORS_OK) {
            /* The command may still own the PN532 host interface. Do not send
             * SAMConfiguration behind a failed abort: that is precisely the
             * ordering that can leave this no-IRQ I2C module stuck. */
            s_pn532.initialized = false;
            return abort_ret;
        } else if (operation_result == SERVICE_V2_SENSORS_ERR_TIMEOUT) {
            /* A bounded RF command followed by a successful protocol abort
             * is a clean discovery miss, not a wedged host I2C bus. */
            operation_result = SERVICE_V2_SENSORS_ERR_NO_TARGET;
        }
    }

    /* A clean no-target response/abort and a protocol/checksum error are
     * reported only after the PN532 response frame (or the abort ACK) has
     * been drained, so there is no host command left pending when this
     * function is called with command_may_be_pending=false.  Re-running the
     * full SAM/firmware/retry configuration for every ordinary RF miss adds
     * several one-second waits and turns a normal approach into a visible
     * multi-second NFC delay.  Keep the PN532 session alive and let the
     * caller retry the bounded exchange immediately.  Transport errors are
     * deliberately excluded: a stalled I2C transaction still needs the full
     * resynchronization path below.
     */
    if (!command_may_be_pending &&
        (operation_result == SERVICE_V2_SENSORS_ERR_PROTOCOL || operation_result == SERVICE_V2_SENSORS_ERR_NO_TARGET)) {
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
        return operation_result;
    }

    /* InRelease/TgGetTargetStatus has already confirmed that the RF session
     * ended.  The PN532 is back in its normal host state at that point; the
     * full SAM + firmware + retry probe is only needed after an uncertain or
     * failed cleanup.  Avoiding that probe on the success path makes the
     * identity counter available to the host immediately instead of several
     * hundred milliseconds later. */
    if (!command_may_be_pending && session_released && operation_result == SERVICE_V2_SENSORS_OK) {
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
        return operation_result;
    }

    /* Give PN532 time to leave PICC/PCD AutoColl before SAMConfiguration. The
     * resync also verifies GetFirmwareVersion and restores reader retries. */
    lisa_thread_mdelay(PN532_RESYNC_SETTLE_MS);
    int sync_ret = pn532_resynchronize_reader_locked(false);
    s_pn532.initialized = sync_ret == SERVICE_V2_SENSORS_OK;
    return sync_ret == SERVICE_V2_SENSORS_OK ? operation_result : sync_ret;
}

static int pn532_configure_type_a_session_locked(bool *command_may_be_pending)
{
    static const uint8_t set_parameters[] = {0x34U};
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    int ret = pn532_tracked_command_locked(PN532_CMD_SET_PARAMETERS, set_parameters, sizeof(set_parameters), response,
                                           sizeof(response), &response_length, SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS,
                                           command_may_be_pending);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    return response_length == 0U ? SERVICE_V2_SENSORS_OK : SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
}

static int pn532_abort_tracked_command_locked(bool *command_may_be_pending, int operation_result)
{
    if (!command_may_be_pending || !*command_may_be_pending) {
        return operation_result;
    }

    int abort_ret = pn532_abort_pending_command_locked();
    if (abort_ret != SERVICE_V2_SENSORS_OK) {
        return abort_ret;
    }
    *command_may_be_pending = false;
    lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
    return operation_result == SERVICE_V2_SENSORS_ERR_TIMEOUT ? SERVICE_V2_SENSORS_ERR_NO_TARGET : operation_result;
}

int service_v2_pn532_type_a_initiator_exchange_timed(const uint8_t *local_payload, uint8_t local_len,
                                                     uint8_t *peer_payload, uint8_t peer_capacity, uint8_t *peer_len,
                                                     service_v2_pn532_target_t *target, uint32_t activation_timeout_ms,
                                                     uint32_t *exchange_elapsed_ms)
{
    if ((local_len > 0U && !local_payload) || !peer_payload || !peer_len || !target || peer_capacity == 0U ||
        local_len > SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN ||
        activation_timeout_ms > SERVICE_V2_PN532_MAX_TIMEOUT_MS) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (activation_timeout_ms == 0U) {
        activation_timeout_ms = SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS;
    }
    *peer_len = 0U;
    if (exchange_elapsed_ms) {
        *exchange_elapsed_ms = 0U;
    }
    memset(target, 0, sizeof(*target));

    int ret = service_lock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (!s_pn532.initialized) {
        lisa_mutex_unlock(s_pn532.lock);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    bool command_may_be_pending = false;
    bool target_is_active = false;
    bool abort_cleanup_failed = false;
    bool exchange_complete = false;
    bool session_released = false;
    bool release_cleanup_only = false;
    uint8_t target_number = 0U;
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    service_v2_pn532_target_t activated_target = {0};
    uint8_t received_payload[SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN];
    size_t received_length = 0U;
    TickType_t exchange_start_tick = 0U;
    bool exchange_timing_started = false;

    ret = pn532_configure_type_a_session_locked(&command_may_be_pending);
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    static const uint8_t list_parameters[] = {
        0x01U, /* MaxTg */
        PN532_BRTY_ISO14443A_106_KBPS,
    };
    response_length = sizeof(response);
    ret = pn532_tracked_command_locked(PN532_CMD_IN_LIST_PASSIVE_TARGET, list_parameters, sizeof(list_parameters),
                                       response, sizeof(response), &response_length, activation_timeout_ms,
                                       &command_may_be_pending);
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    /* Remember that RF activation occurred before parsing the variable-length
     * response. Even a malformed UID/ATS response must be released below. */
    if (response_length >= 1U && response[0] > 0U) {
        target_is_active = true;
        if (response_length >= 2U) {
            target_number = response[1];
        }
    }
    ret = pn532_parse_type_a_target_response(response, response_length, &activated_target);
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }
    target_number = activated_target.target_number;
    if ((activated_target.sel_res & PN532_TARGET_SEL_RES_ISO14443_4) == 0U || activated_target.ats_len < 2U) {
        ret = SERVICE_V2_SENSORS_ERR_PROTOCOL;
        goto out;
    }

    /* Start only after a complete, valid InListPassiveTarget activation
     * response. The scheduling guard below is intentionally part of the
     * board-side recognition latency. */
    if (exchange_elapsed_ms) {
        exchange_start_tick = xTaskGetTickCount();
        exchange_timing_started = true;
    }

    LISA_LOGI(TAG,
              "PN532 peer INLIST_OK: tg=%u uid_len=%u ats_len=%u; "
              "waiting %ums for target host",
              target_number, activated_target.uid_len, activated_target.ats_len, PN532_TYPE_A_TARGET_READY_GUARD_MS);

    /* Give the target host enough time to consume TgInitAsTarget and enter
     * TgGetData before sending the first ISO-DEP I-block. Conditional RDY
     * polling normally completes within one 5 ms interval; this scheduling
     * guard remains comfortably larger without adding the old 450 ms delay. */
    lisa_thread_mdelay(PN532_TYPE_A_TARGET_READY_GUARD_MS);

    uint8_t exchange_parameters[1U + SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN];
    exchange_parameters[0] = target_number;
    if (local_len > 0U) {
        memcpy(&exchange_parameters[1], local_payload, local_len);
    }
    response_length = sizeof(response);
    LISA_LOGI(TAG, "PN532 peer INDATA_BEGIN: tx_len=%u", local_len);
    ret = pn532_tracked_command_locked(PN532_CMD_IN_DATA_EXCHANGE, exchange_parameters, (size_t)local_len + 1U,
                                       response, sizeof(response), &response_length,
                                       PN532_TYPE_A_INITIATOR_DATA_TIMEOUT_MS, &command_may_be_pending);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGW(TAG, "PN532 Type-A initiator data exchange failed: %d", ret);
        goto out;
    }
    if (response_length < 1U) {
        ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
        goto out;
    }
    ret = pn532_type_a_status_error(response[0]);
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    received_length = response_length - 1U;
    if (received_length > SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN || received_length > peer_capacity) {
        ret = SERVICE_V2_SENSORS_ERR_OVERFLOW;
        goto out;
    }
    if (received_length > 0U) {
        memcpy(received_payload, &response[1], received_length);
    }
    if (exchange_elapsed_ms && exchange_timing_started) {
        *exchange_elapsed_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - exchange_start_tick);
        LISA_LOGI(TAG, "PN532 peer INDATA_TIMING elapsed_ms=%u", *exchange_elapsed_ms);
    }
    LISA_LOGI(TAG, "PN532 peer INDATA_OK: rx_len=%u", (unsigned int)received_length);
    exchange_complete = true;

out:
    if (target_is_active) {
        /* InDataExchange may still be pending after an RF timeout. Abort that
         * host command before issuing InRelease; sending another command first
         * is the sequence that used to leave the PN532 stretching I2C. */
        ret = pn532_abort_tracked_command_locked(&command_may_be_pending, ret);
        abort_cleanup_failed = command_may_be_pending;
        if (!command_may_be_pending) {
            uint8_t release_response[1] = {0xFFU};
            size_t release_response_length = sizeof(release_response);
            int release_ret = pn532_tracked_command_locked(
                PN532_CMD_IN_RELEASE, &target_number, sizeof(target_number), release_response, sizeof(release_response),
                &release_response_length, PN532_TYPE_A_RELEASE_TIMEOUT_MS, &command_may_be_pending);
            uint8_t release_status = 0xFFU;
            if (release_ret == SERVICE_V2_SENSORS_OK) {
                if (release_response_length < 1U) {
                    release_ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
                } else {
                    release_status = release_response[0] & 0x3FU;
                    release_ret = pn532_type_a_status_error(release_response[0]);
                    if (release_ret == SERVICE_V2_SENSORS_OK && release_response_length != 1U) {
                        release_ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
                    }
                }
            }

            /* InDataExchange Status=OK means both application payloads have
             * already crossed the RF link.  InRelease is cleanup from that
             * point onward.  Some PN532 revisions report timeout, 0x25, or
             * 0x27 after the peer has already processed DESELECT.  Accept
             * only those bounded cleanup outcomes (plus a drained short
             * response); abort/resync below must still succeed. */
            bool release_state_ended = release_response_length == 1U &&
                                       (release_status == 0x25U || release_status == 0x27U || release_status == 0x29U);
            session_released = exchange_complete && (release_ret == SERVICE_V2_SENSORS_OK || release_state_ended);
            release_cleanup_only =
                exchange_complete &&
                (release_ret == SERVICE_V2_SENSORS_OK || release_ret == SERVICE_V2_SENSORS_ERR_TIMEOUT ||
                 release_ret == SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE || release_state_ended);
            if (release_ret != SERVICE_V2_SENSORS_OK && !release_cleanup_only) {
                ret = release_ret;
            } else if (release_cleanup_only) {
                ret = SERVICE_V2_SENSORS_OK;
            }
        }
    }

    if (abort_cleanup_failed) {
        s_pn532.initialized = false;
    } else {
        ret = pn532_type_a_finalize_locked(command_may_be_pending, ret, session_released);
    }
    if (ret == SERVICE_V2_SENSORS_OK && exchange_complete) {
        if (received_length > 0U) {
            memcpy(peer_payload, received_payload, received_length);
        }
        *peer_len = (uint8_t)received_length;
        *target = activated_target;
    } else {
        *peer_len = 0U;
        memset(target, 0, sizeof(*target));
    }
    lisa_mutex_unlock(s_pn532.lock);
    return ret;
}

int service_v2_pn532_type_a_initiator_exchange(const uint8_t *local_payload, uint8_t local_len, uint8_t *peer_payload,
                                               uint8_t peer_capacity, uint8_t *peer_len,
                                               service_v2_pn532_target_t *target, uint32_t activation_timeout_ms)
{
    return service_v2_pn532_type_a_initiator_exchange_timed(local_payload, local_len, peer_payload, peer_capacity,
                                                            peer_len, target, activation_timeout_ms, NULL);
}

int service_v2_pn532_type_a_target_exchange(const uint8_t nfcid1t[3], const uint8_t *historical_bytes,
                                            uint8_t historical_bytes_len, const uint8_t *local_payload,
                                            uint8_t local_len, uint8_t *peer_payload, uint8_t peer_capacity,
                                            uint8_t *peer_len, uint32_t activation_timeout_ms)
{
    if (!nfcid1t || historical_bytes_len > SERVICE_V2_PN532_HISTORICAL_MAX_LEN ||
        (historical_bytes_len > 0U && !historical_bytes) || (local_len > 0U && !local_payload) || !peer_payload ||
        !peer_len || peer_capacity == 0U || local_len > SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN ||
        activation_timeout_ms > PN532_TYPE_A_TARGET_MAX_WINDOW_MS) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (activation_timeout_ms == 0U) {
        activation_timeout_ms = PN532_TYPE_A_TARGET_DEFAULT_WINDOW_MS;
    }
    *peer_len = 0U;

    int ret = service_lock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (!s_pn532.initialized) {
        lisa_mutex_unlock(s_pn532.lock);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    bool command_may_be_pending = false;
    bool exchange_complete = false;
    bool session_released = false;
    bool release_cleanup_only = false;
    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    uint8_t received_payload[SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN];
    size_t received_length = 0U;

    /* Match the target-mode configuration path used by the proven manual
     * activation diagnostic. Its short response read avoids carrying reader
     * session framing assumptions into TgInitAsTarget. */
    ret = pn532_configure_type_a_target_locked();
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    uint8_t target_parameters[PN532_COMMAND_FRAME_MAX - 9U];
    size_t target_parameter_length =
        pn532_build_type_a_target_parameters(nfcid1t, historical_bytes, historical_bytes_len, target_parameters);

    /* TgInitAsTarget enters Power Down while no RF field is present. V2.1
     * does not route P70_IRQ, so the host uses the atomic conditional-read
     * primitive to poll RDY safely. The command remains armed for the whole
     * window and its response is drained immediately after RF activation. */
    command_may_be_pending = true;
    ret = pn532_send_command_locked(PN532_CMD_TG_INIT_AS_TARGET, target_parameters, target_parameter_length,
                                    SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    bool target_command_settled = false;
    bool target_activated = false;
    uint8_t activation_mode = 0U;
    ret = pn532_finish_type_a_target_window_locked(&target_command_settled, &target_activated, &activation_mode,
                                                   activation_timeout_ms);
    command_may_be_pending = !target_command_settled;
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }
    if (!target_activated) {
        LISA_LOGI(TAG, "PN532 peer TG_INIT_IDLE: window=%ums", activation_timeout_ms);
        ret = SERVICE_V2_SENSORS_ERR_NO_TARGET;
        goto out;
    }
    if ((activation_mode & PN532_TARGET_ACTIVATION_MODE_MASK) != PN532_TARGET_ACTIVATION_TYPE_A_PICC_106) {
        ret = SERVICE_V2_SENSORS_ERR_PROTOCOL;
        goto out;
    }
    LISA_LOGI(TAG, "PN532 Type-A target activated: mode=0x%02x", activation_mode);

    response_length = sizeof(response);
    LISA_LOGI(TAG, "PN532 peer TGGET_BEGIN");
    ret = pn532_tracked_command_locked(PN532_CMD_TG_GET_DATA, NULL, 0U, response, sizeof(response), &response_length,
                                       PN532_TYPE_A_TARGET_DATA_TIMEOUT_MS, &command_may_be_pending);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGW(TAG, "PN532 target TgGetData failed: %d", ret);
        goto out;
    }
    if (response_length < 1U) {
        ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
        goto out;
    }
    ret = pn532_type_a_status_error(response[0]);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGW(TAG, "PN532 target TgGetData status=0x%02x: %d", response[0], ret);
        goto out;
    }

    received_length = response_length - 1U;
    if (received_length > SERVICE_V2_PN532_TYPE_A_PAYLOAD_MAX_LEN || received_length > peer_capacity) {
        ret = SERVICE_V2_SENSORS_ERR_OVERFLOW;
        goto out;
    }
    if (received_length > 0U) {
        memcpy(received_payload, &response[1], received_length);
    }
    LISA_LOGI(TAG, "PN532 peer TGGET_OK: rx_len=%u", (unsigned int)received_length);

    response_length = sizeof(response);
    LISA_LOGI(TAG, "PN532 peer TGSET_BEGIN: tx_len=%u", local_len);
    ret = pn532_tracked_command_locked(PN532_CMD_TG_SET_DATA, local_payload, local_len, response, sizeof(response),
                                       &response_length, PN532_TYPE_A_TARGET_DATA_TIMEOUT_MS, &command_may_be_pending);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGW(TAG, "PN532 target TgSetData failed: %d", ret);
        goto out;
    }
    if (response_length < 1U) {
        ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
        goto out;
    }
    ret = pn532_type_a_status_error(response[0]);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGW(TAG, "PN532 target TgSetData status=0x%02x: %d", response[0], ret);
        goto out;
    }
    if (response_length != 1U) {
        ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
        goto out;
    }
    LISA_LOGI(TAG, "PN532 peer TGSET_OK");
    exchange_complete = true;

    /* TgSetData=OK means the target's response crossed the RF link.  Do not
     * issue TgGetTargetStatus here: on V2.1 there is no P70_IRQ, and that
     * extra poll can race the reader's InRelease/DESELECT.  A timed-out
     * status poll leaves the PN532 host interface waiting for RF and the
     * following ACK abort can wedge the I2C controller.  Let the reader's
     * InRelease settle, then use the normal SAM resynchronization below. */
    command_may_be_pending = false;
    session_released = false;
    lisa_thread_mdelay(PN532_TYPE_A_RELEASE_SETTLE_MS);
    ret = SERVICE_V2_SENSORS_OK;

out:
    /* Once TgSetData has returned Status=OK, both application payloads have
     * crossed the RF link. The reader owns InRelease; after the short settle
     * above, resynchronize the PN532 directly instead of issuing a second
     * target-mode command that can race DESELECT. */
    release_cleanup_only =
        exchange_complete && (ret == SERVICE_V2_SENSORS_OK || ret == SERVICE_V2_SENSORS_ERR_TIMEOUT ||
                              ret == SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE);
    ret = pn532_type_a_finalize_locked(command_may_be_pending, release_cleanup_only ? SERVICE_V2_SENSORS_OK : ret,
                                       session_released);
    if (ret == SERVICE_V2_SENSORS_OK && exchange_complete) {
        if (received_length > 0U) {
            memcpy(peer_payload, received_payload, received_length);
        }
        *peer_len = (uint8_t)received_length;
        /* The link has been released and the PN532 has been resynchronized.
         * A short post-success guard prevents an immediate TgInitAsTarget
         * re-entry while the reader finishes leaving the RF field. It is not
         * paid on startup or on ordinary no-target scan windows. */
        lisa_thread_mdelay(PN532_TYPE_A_POST_EXCHANGE_COOLDOWN_MS);
    } else {
        *peer_len = 0U;
    }
    lisa_mutex_unlock(s_pn532.lock);
    return ret;
}

int service_v2_pn532_advertise_type_a_identity(const uint8_t nfcid1t[3], const uint8_t *historical_bytes,
                                               uint8_t historical_bytes_len, uint32_t duration_ms)
{
    if (!nfcid1t || historical_bytes_len > SERVICE_V2_PN532_HISTORICAL_MAX_LEN ||
        (historical_bytes_len > 0U && !historical_bytes) || duration_ms > SERVICE_V2_PN532_TARGET_MAX_TIMEOUT_MS) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (duration_ms == 0U) {
        duration_ms = SERVICE_V2_PN532_TARGET_DEFAULT_TIMEOUT_MS;
    }

    int ret = service_lock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (!s_pn532.initialized) {
        lisa_mutex_unlock(s_pn532.lock);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    bool target_command_may_be_pending = false;
    bool resynchronized = false;
    ret = pn532_configure_type_a_target_locked();
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    uint8_t target_parameters[PN532_COMMAND_FRAME_MAX - 9U];
    size_t target_parameter_length =
        pn532_build_type_a_target_parameters(nfcid1t, historical_bytes, historical_bytes_len, target_parameters);

    /* P70_IRQ is not routed on ARCS-MINI V2.1. Use atomic conditional RDY
     * polling so an idle status ends with NACK+STOP while a ready activation
     * response is drained in the same I2C transaction. */
    target_command_may_be_pending = true;
    ret = pn532_send_command_locked(PN532_CMD_TG_INIT_AS_TARGET, target_parameters, target_parameter_length,
                                    SERVICE_V2_PN532_DEFAULT_TIMEOUT_MS);
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    bool target_command_settled = false;
    bool target_activated = false;
    ret = pn532_finish_type_a_target_window_locked(&target_command_settled, &target_activated, NULL, duration_ms);
    target_command_may_be_pending = !target_command_settled;
    if (target_activated) {
        LISA_LOGI(TAG, "PN532 Type-A target activation response drained");
    }
    if (ret == SERVICE_V2_SENSORS_OK) {
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
        ret = pn532_resynchronize_reader_locked(false);
    }
    resynchronized = ret == SERVICE_V2_SENSORS_OK;

out:
    if (target_command_may_be_pending) {
        (void)pn532_abort_pending_command_locked();
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
    }
    s_pn532.initialized = resynchronized;
    lisa_mutex_unlock(s_pn532.lock);
    return ret;
}

int service_v2_pn532_wait_for_type_a_activation_with_identity(const uint8_t nfcid1t[3], const uint8_t *historical_bytes,
                                                              uint8_t historical_bytes_len,
                                                              service_v2_pn532_activation_t *activation,
                                                              uint32_t timeout_ms)
{
    if (!nfcid1t || !activation || historical_bytes_len > SERVICE_V2_PN532_HISTORICAL_MAX_LEN ||
        (historical_bytes_len > 0U && !historical_bytes) || timeout_ms > SERVICE_V2_PN532_TARGET_MAX_TIMEOUT_MS) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (timeout_ms == 0U) {
        timeout_ms = SERVICE_V2_PN532_TARGET_DEFAULT_TIMEOUT_MS;
    }

    memset(activation, 0, sizeof(*activation));
    int ret = service_lock(s_pn532.lock);
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    if (!s_pn532.initialized) {
        lisa_mutex_unlock(s_pn532.lock);
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }
    bool target_command_started = false;
    bool target_response_received = false;

    uint8_t response[PN532_RESPONSE_DATA_MAX];
    size_t response_length = sizeof(response);
    ret = pn532_configure_type_a_target_locked();
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }

    /* TgInitAsTarget parameters (UM0701-02 section 7.3.14):
     * - Mode 0x05: passive-only ISO/IEC 14443-4 PICC.  PICCOnly makes the
     *   first accepted initiator command a RATS, so activation completes on
     *   both the reader and target sides.
     * - SENS_RES 0x0004 (LSB first), caller-supplied three-byte NFCID1t and
     *   SEL_RES 0x20. PN532 prepends 0x08 to the three configurable bytes.
     * - The remaining values provide valid FeliCa/NFCID3 fields even though
     *   this service only accepts passive Type-A activation.
     * - No general bytes. The optional historical bytes are returned in
     *   the automatic ATS and let peer discovery advertise a full identity.
     */
    uint8_t target_parameters[PN532_COMMAND_FRAME_MAX - 9U];
    size_t target_parameter_length =
        pn532_build_type_a_target_parameters(nfcid1t, historical_bytes, historical_bytes_len, target_parameters);

    response_length = sizeof(response);
    target_command_started = true;
    ret = pn532_exchange_locked(PN532_CMD_TG_INIT_AS_TARGET, target_parameters, target_parameter_length, response,
                                sizeof(response), &response_length, PN532_RESPONSE_WIRE_MAX, timeout_ms);
    target_response_received = ret == SERVICE_V2_SENSORS_OK;
    /* This API is deliberately one-shot.  A successful activation leaves the
     * chip in PICC state; a failed wait can leave AutoColl pending.  Force the
     * next public operation through full PN532/SAM initialization either way. */
    s_pn532.initialized = false;
    if (ret != SERVICE_V2_SENSORS_OK) {
        goto out;
    }
    if (response_length < 1U) {
        ret = SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE;
        goto out;
    }

    size_t initiator_command_length = response_length - 1U;
    if (initiator_command_length > SERVICE_V2_PN532_INITIATOR_COMMAND_MAX_LEN) {
        ret = SERVICE_V2_SENSORS_ERR_OVERFLOW;
        goto out;
    }

    activation->mode = response[0];
    activation->initiator_command_len = (uint8_t)initiator_command_length;
    if (initiator_command_length > 0U) {
        memcpy(activation->initiator_command, &response[1], initiator_command_length);
    }

out:
    if (target_command_started || ret != SERVICE_V2_SENSORS_OK) {
        s_pn532.initialized = false;
    }
    if (target_command_started && !target_response_received) {
        int abort_ret = pn532_abort_pending_command_locked();
        if (abort_ret != SERVICE_V2_SENSORS_OK) {
            ret = abort_ret;
        }
        lisa_thread_mdelay(PN532_POLL_INTERVAL_MS);
    }
    lisa_mutex_unlock(s_pn532.lock);
    return ret;
}

int service_v2_pn532_wait_for_type_a_activation(service_v2_pn532_activation_t *activation, uint32_t timeout_ms)
{
    static const uint8_t diagnostic_nfcid1t[3] = {0x12U, 0x34U, 0x56U};

    return service_v2_pn532_wait_for_type_a_activation_with_identity(diagnostic_nfcid1t, NULL, 0U, activation,
                                                                     timeout_ms);
}

/* -------------------------------------------------------------------------- */
/* Aggregate API                                                              */
/* -------------------------------------------------------------------------- */

int service_v2_sensors_init(void)
{
    int first_error = SERVICE_V2_SENSORS_OK;
    int ret = service_v2_touch_init();
    if (ret != SERVICE_V2_SENSORS_OK) {
        first_error = ret;
    }

    ret = service_v2_mpu6050_init(false);
    if (ret != SERVICE_V2_SENSORS_OK && first_error == SERVICE_V2_SENSORS_OK) {
        first_error = ret;
    }

    /* Create the PN532 mutex and bind it to the shared I2C0 controller before the background peer
     * worker starts. This keeps pn532_prepare()'s lazy first-use path from
     * racing a shell command, while the slow hardware probe remains deferred.
     * The LISA controller mutex serializes PN532 and MPU6050 transfers. */
    ret = pn532_prepare();
    if (ret != SERVICE_V2_SENSORS_OK) {
        if (first_error == SERVICE_V2_SENSORS_OK) {
            first_error = ret;
        }
        LISA_LOGW(TAG, "PN532 transport prepare failed: %d", ret);
    } else {
        LISA_LOGI(TAG, "PN532 transport ready; probe deferred (shared i2c0 addr=0x24)");
    }

    return first_error;
}

int service_v2_sensors_self_test(service_v2_sensors_self_test_result_t *result)
{
    if (!result) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    memset(result, 0, sizeof(*result));

    int touch_init_status = service_v2_touch_init();
    result->touch_status = touch_init_status;
    result->touch_pa13_status = touch_init_status;
    if (touch_init_status == SERVICE_V2_SENSORS_OK) {
        result->touch_status =
            service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA09, &result->touch_active);
        result->touch_pa13_status =
            service_v2_touch_read_channel(SERVICE_V2_TOUCH_CHANNEL_PA13, &result->touch_pa13_active);
    }

    result->mpu6050_status = service_v2_mpu6050_init(false);
    if (result->mpu6050_status == SERVICE_V2_SENSORS_OK) {
        result->mpu6050_status = service_v2_mpu6050_who_am_i(&result->mpu6050_who_am_i);
    }
    if (result->mpu6050_status == SERVICE_V2_SENSORS_OK) {
        result->mpu6050_status = service_v2_mpu6050_read_raw(&result->mpu6050_raw);
    }

    result->pn532_status = service_v2_pn532_init();
    if (result->pn532_status == SERVICE_V2_SENSORS_OK) {
        result->pn532_status = service_v2_pn532_get_firmware_version(&result->pn532_firmware);
    }
    if (result->pn532_status != SERVICE_V2_SENSORS_OK) {
        pn532_log_strap_hint(result->pn532_status);
    }

    LISA_LOGI(TAG, "self-test: touch_pa09=%d touch_pa13=%d mpu6050=%d pn532=%d", result->touch_status,
              result->touch_pa13_status, result->mpu6050_status, result->pn532_status);

    if (result->touch_status != SERVICE_V2_SENSORS_OK || result->touch_pa13_status != SERVICE_V2_SENSORS_OK ||
        result->mpu6050_status != SERVICE_V2_SENSORS_OK || result->pn532_status != SERVICE_V2_SENSORS_OK) {
        return SERVICE_V2_SENSORS_ERR_SELF_TEST;
    }

    return SERVICE_V2_SENSORS_OK;
}

const char *service_v2_sensors_strerror(int error)
{
    switch (error) {
    case SERVICE_V2_SENSORS_OK:
        return "ok";
    case SERVICE_V2_SENSORS_ERR_INVALID:
        return "invalid argument";
    case SERVICE_V2_SENSORS_ERR_NOT_READY:
        return "device not ready";
    case SERVICE_V2_SENSORS_ERR_IO:
        return "I/O error";
    case SERVICE_V2_SENSORS_ERR_TIMEOUT:
        return "timeout";
    case SERVICE_V2_SENSORS_ERR_PROTOCOL:
        return "protocol/checksum error";
    case SERVICE_V2_SENSORS_ERR_UNEXPECTED_RESPONSE:
        return "unexpected device response";
    case SERVICE_V2_SENSORS_ERR_NO_TARGET:
        return "no NFC target";
    case SERVICE_V2_SENSORS_ERR_BUSY:
        return "service busy";
    case SERVICE_V2_SENSORS_ERR_NO_MEMORY:
        return "out of memory";
    case SERVICE_V2_SENSORS_ERR_SELF_TEST:
        return "one or more self-tests failed";
    case SERVICE_V2_SENSORS_ERR_OVERFLOW:
        return "response too large";
    default:
        return "unknown error";
    }
}
