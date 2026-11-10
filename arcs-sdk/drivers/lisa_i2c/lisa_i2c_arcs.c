/*
 * Copyright (c) 2025, LISTENAI
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lisa_i2c_arcs.c
 * @brief LISA I2C ARCS 平台适配层
 *
 * 此文件实现 ARCS 芯片平台的 I2C 硬件适配
 */

#include "lisa_i2c.h"
#include "Driver_I2C.h"
#include "arcs_ap.h"
#include <stddef.h>
#include <string.h>
#include <lisa_mutex.h>
#include <lisa_time.h>
#include "pinmux.h"

#define LOG_TAG "lisa_i2c_arcs"
#include <lisa_log.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define LISA_I2C_ARCS_MAX_TRANSFER_SIZE   256U
#define LISA_I2C_ARCS_CMD_ACK             0x2U
#define LISA_I2C_ARCS_CMD_NACK            0x3U
/* Internal callback flag: the ATCIIC100 received one byte.  The public
 * LISA-I2C event mask has no byte-received bit, but PN532's I2C protocol needs
 * an ACK/NACK decision immediately after its RDY byte. */
#define LISA_I2C_ARCS_EVENT_BYTE_RECEIVED (1UL << 10)

#define DEVICE_LOCK(priv)                                                                                              \
    do {                                                                                                               \
        if (priv->mutex) {                                                                                             \
            lisa_mutex_lock(priv->mutex, LISA_OS_WAIT_FOREVER);                                                        \
        }                                                                                                              \
    } while (0)

#define DEVICE_UNLOCK(priv)                                                                                            \
    do {                                                                                                               \
        if (priv->mutex) {                                                                                             \
            lisa_mutex_unlock(priv->mutex);                                                                            \
        }                                                                                                              \
    } while (0)

/* ===== I2C 设备私有数据 ===== */
typedef struct {
    void *hal_handler;                    /* HAL I2C 句柄 */
    I2C_RegDef *reg;                      /* ATCIIC100 寄存器 */
    lisa_mutex_t *mutex;                 /* 互斥锁 */
    lisa_i2c_config_t config;            /* 当前配置 */
    volatile uint32_t event_flags;        /* 事件标志（用于同步） */
    SemaphoreHandle_t xfer_sem;          /* 传输完成信号量（独立于调用者的 task notification） */
    lisa_i2c_debug_snapshot_t last_timeout;
    bool last_timeout_valid;
    volatile bool conditional_active;
    volatile bool conditional_first_received;
    volatile bool conditional_continue;
    uint8_t conditional_mask;
    uint8_t conditional_value;
    uint8_t conditional_first_byte;
} lisa_i2c_priv_t;

/* ===== I2C 设备静态实例 ===== */
#if CONFIG_LISA_I2C0
static lisa_i2c_priv_t i2c0_priv;
#endif

#if CONFIG_LISA_I2C1
static lisa_i2c_priv_t i2c1_priv;
#endif

/* ===== HAL 事件回调函数 ===== */
static void handle_conditional_first_byte(lisa_i2c_priv_t *priv, uint32_t event)
{
    if (!priv->conditional_active || priv->conditional_first_received ||
        (event & (CSK_I2C_EVENT_TRANSFER_DONE | CSK_I2C_EVENT_ADDRESS_NACK | CSK_I2C_EVENT_ARBITRATION_LOST |
                  CSK_I2C_EVENT_BUS_ERROR)) ||
        ((event & LISA_I2C_ARCS_EVENT_BYTE_RECEIVED) == 0U && priv->reg->REG_STATUS.bit.FIFO_EMPTY)) {
        return;
    }

    uint8_t first_byte = (uint8_t)(priv->reg->REG_DATA.all & I2C_DATA_DATA_Msk);
    bool should_continue = (first_byte & priv->conditional_mask) == (priv->conditional_value & priv->conditional_mask);

    priv->conditional_first_byte = first_byte;
    priv->conditional_continue = should_continue;
    priv->conditional_first_received = true;

    /* ByteRecv disables ATCIIC100 auto-ACK and holds SCL low after the byte.
     * Respond while ByteRecv is still enabled, then restore automatic ACK for
     * the remaining fixed-length receive.  A NACK makes the already-selected
     * STOP phase run immediately, even though DATACNT has not reached zero. */
    priv->reg->REG_CMD.bit.CMD = should_continue ? LISA_I2C_ARCS_CMD_ACK : LISA_I2C_ARCS_CMD_NACK;
    priv->reg->REG_INTEN.bit.BYTERECV = 0U;
}

static void i2c_event_callback(lisa_i2c_priv_t *priv, uint32_t event)
{
    if (!priv) {
        return;
    }

    priv->event_flags |= event;
    handle_conditional_first_byte(priv, event);
#if !CONFIG_ARCS_CP_CORE
    if (priv->xfer_sem) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(priv->xfer_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
#endif
}

#if CONFIG_LISA_I2C0
static void i2c0_event_callback(uint32_t event, void *workspace)
{
    i2c_event_callback((lisa_i2c_priv_t *)workspace, event);
}
#endif

#if CONFIG_LISA_I2C1
static void i2c1_event_callback(uint32_t event, void *workspace)
{
    i2c_event_callback((lisa_i2c_priv_t *)workspace, event);
}
#endif

/* ===== 内部辅助函数 ===== */

/**
 * @brief 验证I2C速度是否在支持范围内
 * @param speed 速度值（Hz）
 * @return true 速度有效, false 速度无效
 */
static bool is_valid_speed(uint32_t speed)
{
    /* 支持的速度范围：100kHz 到 1MHz */
    return (speed >= LISA_I2C_SPEED_STANDARD && speed <= LISA_I2C_SPEED_FAST_PLUS);
}

/**
 * @brief 将 LISA I2C 速度转换为 HAL 速度常量
 * @param speed 速度值（Hz）
 * @return HAL速度常量，如果速度无效返回0
 */
static uint32_t speed_to_hal_speed(uint32_t speed)
{
    if (speed <= 100000) {
        return CSK_I2C_BUS_SPEED_STANDARD;
    } else if (speed <= 400000) {
        return CSK_I2C_BUS_SPEED_FAST;
    } else if (speed <= 1000000) {
        return CSK_I2C_BUS_SPEED_FAST_PLUS;
    } else {
        /* 超出支持范围 */
        return 0;
    }
}

static int capture_debug_snapshot(lisa_i2c_priv_t *priv, lisa_i2c_debug_snapshot_t *snapshot)
{
    CSK_I2C_DEBUG_SNAPSHOT hal_snapshot = {0};
    int32_t ret = I2C_GetDebugSnapshot(priv->hal_handler, &hal_snapshot);
    if (ret != CSK_DRIVER_OK) {
        return LISA_DEVICE_ERR_IO;
    }

    snapshot->raw_status = hal_snapshot.raw_status;
    snapshot->raw_ctrl = hal_snapshot.raw_ctrl;
    snapshot->raw_inten = hal_snapshot.raw_inten;
    snapshot->raw_setup = hal_snapshot.raw_setup;
    snapshot->address_ack_latched = hal_snapshot.address_ack_latched != 0U;
    snapshot->transfer_busy = hal_snapshot.transfer_busy != 0U;
    return LISA_DEVICE_OK;
}

/**
 * @brief 检查传输事件并返回结果
 * @return LISA_DEVICE_OK / ERR_NACK / ERR_IO 表示传输已结束, -1 表示尚未完成
 */
static inline int check_transfer_events(lisa_i2c_priv_t *priv, bool is_probe)
{
    uint32_t events = priv->event_flags;

    /* The HAL may report TRANSFER_DONE together with ADDRESS_NACK. Error
     * events must win, otherwise a missing slave is incorrectly accepted as
     * a successful transfer and upper layers parse stale/zero data. */
    if (events & (CSK_I2C_EVENT_ADDRESS_NACK | CSK_I2C_EVENT_ARBITRATION_LOST | CSK_I2C_EVENT_BUS_ERROR)) {
        priv->event_flags = 0;
        if (events & CSK_I2C_EVENT_ADDRESS_NACK) {
            return LISA_DEVICE_ERR_NACK;
        }
        return LISA_DEVICE_ERR_IO;
    }

    if (events & CSK_I2C_EVENT_TRANSFER_DONE) {
        if (is_probe) {
            priv->event_flags = 0;
            return (events & CSK_I2C_EVENT_ADDRESS_ACK) ?
                   LISA_DEVICE_OK : LISA_DEVICE_ERR_NACK;
        }
        priv->event_flags = 0;
        return LISA_DEVICE_OK;
    }

    return -1; /* 尚未完成 */
}

/**
 * @brief 等待传输完成或错误
 *
 * 基于 FreeRTOS 任务通知：ISR 回调中 vTaskNotifyGiveFromISR 唤醒本任务，
 * 零延迟、零 CPU 空转。
 */
static int wait_for_transfer(lisa_i2c_priv_t *priv, uint32_t timeout_ms, bool is_probe)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t start_ticks = xTaskGetTickCount();
    int ret;

    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    /* HAL 在长传输中会为 FIFO 空/满上报 TRANSFER_INCOMPLETE。
     * 这是正常的中间事件，不能把第一次唤醒当成超时；必须继续
     * 等待 TRANSFER_DONE、明确错误或总超时。
     */
    for (;;) {
#if CONFIG_ARCS_CP_CORE
        /* I2C IRQ/DMA are routed only to the AP core on ARCS.  On CP, poll
         * the HAL IRQ-equivalent path tightly enough to service the 16-byte
         * FIFO during a 100 kHz transfer. */
        (void)I2C_PollEvents(priv->hal_handler);
#endif

        /* 快速路径：事件可能在 HAL 启动函数返回前已由 ISR 置位。 */
        ret = check_transfer_events(priv, is_probe);
        if (ret != -1) {
            return ret;
        }

        TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
        if (elapsed_ticks >= timeout_ticks) {
            break;
        }

#if !CONFIG_ARCS_CP_CORE
        TickType_t remaining_ticks = timeout_ticks - elapsed_ticks;
        if (xSemaphoreTake(priv->xfer_sem, remaining_ticks) != pdTRUE) {
            break;
        }
#endif
    }

    /* 超时边界上 ISR 可能刚好置位了终态，最后再检查一次。 */
    ret = check_transfer_events(priv, is_probe);
    if (ret != -1) {
        return ret;
    }

    /* Preserve the state that caused the timeout before ABORT/RESET clears
     * the controller and destroys the physical-layer evidence. */
    priv->last_timeout_valid = capture_debug_snapshot(priv, &priv->last_timeout) == LISA_DEVICE_OK;

    /* A real timeout leaves the HAL driver's status.busy set. Without an
     * explicit abort every following transfer returns CSK_DRIVER_ERROR_BUSY
     * (-2), and the HAL may also continue using the caller's expired buffer.
     * Abort while the device mutex is still held, then drain any completion
     * notification generated by the controller reset. */
    (void)I2C_Control(priv->hal_handler, CSK_I2C_ABORT_TRANSFER, 0);
    (void)I2C_Control(priv->hal_handler, CSK_I2C_BUS_CLEAR, 0);
    priv->event_flags = 0;
    xSemaphoreTake(priv->xfer_sem, 0);
    return LISA_DEVICE_ERR_TIMEOUT;
}

/* ===== ARCS平台I2C实现函数 ===== */

/**
 * @brief 配置I2C总线
 */
static int arcs_i2c_configure(lisa_device_t *dev, const lisa_i2c_config_t *config)
{
    if (!lisa_device_is_initialized(dev) || !config) {
        return LISA_DEVICE_ERR_INVALID;
    }

    /* 验证速度范围 */
    if (!is_valid_speed(config->speed)) {
        LISA_LOGE(LOG_TAG, "Invalid I2C speed: %lu Hz (supported range: %u-%u Hz)", 
                  config->speed, LISA_I2C_SPEED_STANDARD, LISA_I2C_SPEED_FAST_PLUS);
        return LISA_DEVICE_ERR_INVALID;
    }

    lisa_i2c_priv_t *priv = (lisa_i2c_priv_t *)dev->priv_data;

    DEVICE_LOCK(priv);

    /* 设置总线速度 */
    uint32_t hal_speed = speed_to_hal_speed(config->speed);
    if (hal_speed == 0) {
        DEVICE_UNLOCK(priv);
        LISA_LOGE(LOG_TAG, "Invalid I2C speed: %lu Hz (supported range: %u-%u Hz)", 
                  config->speed, LISA_I2C_SPEED_STANDARD, LISA_I2C_SPEED_FAST_PLUS);
        return LISA_DEVICE_ERR_INVALID;
    }

    if (I2C_Control(priv->hal_handler, CSK_I2C_BUS_SPEED, hal_speed) != 0) {
        DEVICE_UNLOCK(priv);
        LISA_LOGE(LOG_TAG, "Failed to set bus speed");
        return LISA_DEVICE_ERR_IO;
    }

    /* 保存配置（仅在成功设置后） */
    memcpy(&priv->config, config, sizeof(lisa_i2c_config_t));

    /* 如果是从机模式，设置从机地址 */
    if (!config->master_mode) {
        if (I2C_Control(priv->hal_handler, CSK_I2C_OWN_ADDRESS, config->slave_addr) != 0) {
            DEVICE_UNLOCK(priv);
            LISA_LOGE(LOG_TAG, "Failed to set slave address");
            return LISA_DEVICE_ERR_IO;
        }
    }

    DEVICE_UNLOCK(priv);

    LISA_LOGI(LOG_TAG, "I2C configured: speed=%lu Hz, mode=%s", config->speed,
              config->master_mode ? "master" : "slave");

    return LISA_DEVICE_OK;
}

/**
 * @brief 获取I2C总线当前配置
 */
static int arcs_i2c_get_config(lisa_device_t *dev, lisa_i2c_config_t *config)
{
    if (!lisa_device_is_initialized(dev) || !config) {
        return LISA_DEVICE_ERR_INVALID;
    }

    lisa_i2c_priv_t *priv = (lisa_i2c_priv_t *)dev->priv_data;

    DEVICE_LOCK(priv);
    memcpy(config, &priv->config, sizeof(lisa_i2c_config_t));
    DEVICE_UNLOCK(priv);

    /* 验证配置的有效性
     * 如果发现配置无效（可能是之前保存的无效值），返回错误
     * 调用者应该重新调用 configure 来设置有效配置
     */
    if (!is_valid_speed(config->speed)) {
        LISA_LOGE(LOG_TAG, "Current I2C speed is invalid: %lu Hz (supported range: %u-%u Hz). Please reconfigure with a valid speed.", 
                  config->speed, LISA_I2C_SPEED_STANDARD, LISA_I2C_SPEED_FAST_PLUS);
        return LISA_DEVICE_ERR_INVALID;
    }

    return LISA_DEVICE_OK;
}

static int arcs_i2c_get_debug_status(lisa_device_t *dev, lisa_i2c_debug_status_t *status)
{
    if (!lisa_device_is_initialized(dev) || !status) {
        return LISA_DEVICE_ERR_INVALID;
    }

    lisa_i2c_priv_t *priv = (lisa_i2c_priv_t *)dev->priv_data;
    DEVICE_LOCK(priv);
    memset(status, 0, sizeof(*status));
    int ret = capture_debug_snapshot(priv, &status->current);
    status->last_timeout = priv->last_timeout;
    status->last_timeout_valid = priv->last_timeout_valid;
    DEVICE_UNLOCK(priv);
    return ret;
}

/**
 * @brief I2C通用传输接口
 */
static int arcs_i2c_transfer(lisa_device_t *dev, lisa_i2c_msg_t *msgs, uint32_t num_msgs)
{
    if (!lisa_device_is_initialized(dev) || !msgs || num_msgs == 0) {
        return LISA_DEVICE_ERR_INVALID;
    }

    lisa_i2c_priv_t *priv = (lisa_i2c_priv_t *)dev->priv_data;

    DEVICE_LOCK(priv);
    priv->last_timeout_valid = false;

    /* 检查是否为主机模式 */
    if (!priv->config.master_mode) {
        DEVICE_UNLOCK(priv);
        LISA_LOGE(LOG_TAG, "Transfer only supported in master mode");
        return LISA_DEVICE_ERR_NOT_SUPPORT;
    }

    for (uint32_t i = 0; i < num_msgs; i++) {
        lisa_i2c_msg_t *msg = &msgs[i];

        /* 检查消息有效性
         * 允许 len == 0 的情况（用于地址探测）
         * 当 len > 0 时，buf 必须有效
         */
        if (msg->len > 0 && !msg->buf) {
            DEVICE_UNLOCK(priv);
            return LISA_DEVICE_ERR_INVALID;
        }

        /* 准备地址（支持10位地址） */
        uint32_t addr = msg->addr;
        if (msg->flags & LISA_I2C_FLAG_10BIT_ADDR) {
            addr |= CSK_I2C_ADDRESS_10BIT;
        }

        /* 判断是否为读操作（通过 LISA_I2C_FLAG_READ 标志） */
        bool is_write = !(msg->flags & LISA_I2C_FLAG_READ);

        /* 清除事件标志，并排空上一条消息可能残留的信号量
         * 场景：上一条消息的 ISR 在快速路径检查前已完成，
         * wait_for_transfer 通过快速路径返回但未 take 信号量，
         * 导致信号量处于 "given" 状态。若不排空，下一条消息的
         * xSemaphoreTake 会立即返回，造成假超时。
         */
        priv->event_flags = 0;
        xSemaphoreTake(priv->xfer_sem, 0);

        /* 判断是否发送 STOP 条件 */
        bool xfer_pending = (msg->flags & LISA_I2C_FLAG_NO_STOP) ? true : false;

        /* 如果是第一个消息且设置了 NO_START，需要特殊处理 */
        /* 但 HAL 接口不支持 NO_START，所以忽略该标志 */
        if (i == 0 && (msg->flags & LISA_I2C_FLAG_NO_START)) {
            LISA_LOGW(LOG_TAG, "NO_START flag not supported, ignored");
        }

        /* 执行传输
         * HAL 层支持 0 字节传输（用于地址探测/ACK polling）
         * 当 len == 0 时，buf 可以为 NULL
         */
        int32_t hal_ret;
        if (is_write) {
            hal_ret = I2C_MasterTransmit(priv->hal_handler, addr, 
                                        msg->len > 0 ? msg->buf : NULL, 
                                        msg->len, xfer_pending);
        } else {
            hal_ret = I2C_MasterReceive(priv->hal_handler, addr, 
                                       msg->len > 0 ? msg->buf : NULL, 
                                       msg->len, xfer_pending);
        }

        if (hal_ret != 0) {
            /* HAL 启动失败，尝试等待短时间检查是否有事件标志（如 NACK） */
            bool is_probe = (msg->len == 0);  /* 0字节传输为设备探测 */
            int ret = wait_for_transfer(priv, 100, is_probe); /* 100ms 短超时 */
            DEVICE_UNLOCK(priv);
            if (ret == LISA_DEVICE_ERR_NACK) {
                return LISA_DEVICE_ERR_NACK; /* 明确返回 NACK */
            }
            LISA_LOGE(LOG_TAG, "HAL transfer failed: %d", hal_ret);
            return LISA_DEVICE_ERR_IO;
        }

        /* 等待传输完成
         * 对于组合传输（写后读），即使设置了 NO_STOP，也需要等待第一个传输完成
         * 才能启动第二个传输，否则 HAL 会返回 BUSY 错误
         * 只有在最后一个消息时才发送 STOP 条件
         */
        bool is_probe = (msg->len == 0);  /* 0字节传输为设备探测 */
        uint32_t timeout_ms = is_probe ? 50 : 1000;
        int ret = wait_for_transfer(priv, timeout_ms, is_probe);
            if (ret != LISA_DEVICE_OK) {
                DEVICE_UNLOCK(priv);
                return ret;
        }
    }

    DEVICE_UNLOCK(priv);
    return LISA_DEVICE_OK;
}

/**
 * @brief I2C写数据
 */
static int arcs_i2c_write(lisa_device_t *dev, uint16_t addr, const uint8_t *buf, uint32_t len)
{
    if (!lisa_device_is_initialized(dev) || !buf || len == 0) {
        return LISA_DEVICE_ERR_INVALID;
    }

    lisa_i2c_priv_t *priv = (lisa_i2c_priv_t *)dev->priv_data;

    DEVICE_LOCK(priv);
    priv->last_timeout_valid = false;

    /* 检查是否为主机模式 */
    if (!priv->config.master_mode) {
        DEVICE_UNLOCK(priv);
        LISA_LOGE(LOG_TAG, "Write only supported in master mode");
        return LISA_DEVICE_ERR_NOT_SUPPORT;
    }

    /* 清除事件标志 */
    priv->event_flags = 0;

    /* 执行写操作 */
    int32_t hal_ret = I2C_MasterTransmit(priv->hal_handler, addr, (uint8_t *)buf, len, false);
    if (hal_ret != 0) {
        /* HAL 启动失败，尝试等待短时间检查是否有事件标志（如 NACK） */
        int ret = wait_for_transfer(priv, 100, false); /* 100ms 短超时 */
        DEVICE_UNLOCK(priv);
        if (ret == LISA_DEVICE_ERR_NACK) {
            return LISA_DEVICE_ERR_NACK; /* 明确返回 NACK */
        }
        LISA_LOGE(LOG_TAG, "HAL write failed: %d", hal_ret);
        return LISA_DEVICE_ERR_IO;
    }

    /* 等待传输完成（write 不是设备探测） */
    int ret = wait_for_transfer(priv, 1000, false); /* 1秒超时 */

    DEVICE_UNLOCK(priv);
    return ret;
}

/**
 * @brief I2C读数据
 */
static int arcs_i2c_read(lisa_device_t *dev, uint16_t addr, uint8_t *buf, uint32_t len)
{
    if (!lisa_device_is_initialized(dev) || !buf || len == 0) {
        return LISA_DEVICE_ERR_INVALID;
    }

    lisa_i2c_priv_t *priv = (lisa_i2c_priv_t *)dev->priv_data;

    DEVICE_LOCK(priv);
    priv->last_timeout_valid = false;

    /* 检查是否为主机模式 */
    if (!priv->config.master_mode) {
        DEVICE_UNLOCK(priv);
        LISA_LOGE(LOG_TAG, "Read only supported in master mode");
        return LISA_DEVICE_ERR_NOT_SUPPORT;
    }

    /* 清除事件标志 */
    priv->event_flags = 0;

    /* 执行读操作 */
    int32_t hal_ret = I2C_MasterReceive(priv->hal_handler, addr, buf, len, false);
    if (hal_ret != 0) {
        /* HAL 启动失败，尝试等待短时间检查是否有事件标志（如 NACK） */
        int ret = wait_for_transfer(priv, 100, false); /* 100ms 短超时 */
        DEVICE_UNLOCK(priv);
        if (ret == LISA_DEVICE_ERR_NACK) {
            return LISA_DEVICE_ERR_NACK; /* 明确返回 NACK */
        }
        LISA_LOGE(LOG_TAG, "HAL read failed: %d", hal_ret);
        return LISA_DEVICE_ERR_IO;
    }

    /* 等待传输完成（read 不是设备探测） */
    int ret = wait_for_transfer(priv, 1000, false); /* 1秒超时 */

    DEVICE_UNLOCK(priv);
    return ret;
}

/**
 * @brief 在首字节 ACK 位前判断是否继续同一个主机读事务
 */
static int arcs_i2c_read_conditional(lisa_device_t *dev, uint16_t addr, uint8_t *buf, uint32_t len,
                                     uint8_t continue_mask, uint8_t continue_value)
{
    if (!lisa_device_is_initialized(dev) || !buf || len < 2U || len > LISA_I2C_ARCS_MAX_TRANSFER_SIZE ||
        addr > 0x3FFU) {
        return LISA_DEVICE_ERR_INVALID;
    }

    lisa_i2c_priv_t *priv = (lisa_i2c_priv_t *)dev->priv_data;

    DEVICE_LOCK(priv);
    priv->last_timeout_valid = false;

    if (!priv->config.master_mode || !priv->reg) {
        DEVICE_UNLOCK(priv);
        return LISA_DEVICE_ERR_NOT_SUPPORT;
    }

    priv->event_flags = 0U;
    xSemaphoreTake(priv->xfer_sem, 0);

    /* I2C_MasterReceive issues the transaction before returning.  Keep the
     * CPU IRQ masked until ByteRecv is enabled so the first data byte cannot
     * be handled by the normal auto-ACK path because of task preemption. */
    taskENTER_CRITICAL();
    priv->conditional_mask = continue_mask;
    priv->conditional_value = continue_value;
    priv->conditional_first_byte = 0U;
    priv->conditional_first_received = false;
    priv->conditional_continue = false;
    priv->conditional_active = true;

    int32_t hal_ret = I2C_MasterReceive(priv->hal_handler, addr, buf, len, false);
    if (hal_ret == CSK_DRIVER_OK) {
        /* ATCIIC100 holds SCL low after each received byte while ByteRecv is
         * enabled, giving the callback a deterministic ACK/NACK decision
         * point before any following byte is clocked. */
        priv->reg->REG_INTEN.bit.BYTERECV = 1U;
    } else {
        priv->conditional_active = false;
    }
    taskEXIT_CRITICAL();

    if (hal_ret != CSK_DRIVER_OK) {
        DEVICE_UNLOCK(priv);
        LISA_LOGE(LOG_TAG, "HAL conditional read failed: %ld", (long)hal_ret);
        return (hal_ret == CSK_DRIVER_ERROR_BUSY) ? LISA_DEVICE_ERR_BUSY : LISA_DEVICE_ERR_IO;
    }

    int ret = wait_for_transfer(priv, 1000U, false);

    taskENTER_CRITICAL();
    priv->conditional_active = false;
    bool first_received = priv->conditional_first_received;
    bool should_continue = priv->conditional_continue;
    uint8_t first_byte = priv->conditional_first_byte;
    taskEXIT_CRITICAL();

    if (ret != LISA_DEVICE_OK) {
        DEVICE_UNLOCK(priv);
        return ret;
    }

    if (!first_received) {
        DEVICE_UNLOCK(priv);
        LISA_LOGE(LOG_TAG, "Conditional read completed without first byte");
        return LISA_DEVICE_ERR_IO;
    }

    if (!should_continue) {
        buf[0] = first_byte;
        DEVICE_UNLOCK(priv);
        return LISA_DEVICE_ERR_NOT_READY;
    }

    /* The callback consumed the first byte directly from the RX FIFO.  HAL
     * therefore stores the remaining bytes one slot early and may place one
     * harmless empty-FIFO bookkeeping read in the final slot.  Shift the real
     * `len - 1` bytes into their requested positions after completion. */
    memmove(&buf[1], &buf[0], len - 1U);
    buf[0] = first_byte;

    DEVICE_UNLOCK(priv);
    return LISA_DEVICE_OK;
}

/* ===== ARCS I2C API 实例 ===== */
static const lisa_i2c_api_t arcs_i2c_api = {
    .configure = arcs_i2c_configure,
    .get_config = arcs_i2c_get_config,
    .transfer = arcs_i2c_transfer,
    .write = arcs_i2c_write,
    .read = arcs_i2c_read,
    .get_debug_status = arcs_i2c_get_debug_status,
    .read_conditional = arcs_i2c_read_conditional,
};

/* ===== 设备初始化函数 ===== */

#if CONFIG_LISA_I2C0
static int arcs_i2c0_init(void)
{
    /* 清空私有数据 */
    memset(&i2c0_priv, 0, sizeof(lisa_i2c_priv_t));

    /* 获取 HAL I2C0 句柄 */
    i2c0_priv.hal_handler = I2C0();
    if (!i2c0_priv.hal_handler) {
        LISA_LOGE(LOG_TAG, "Failed to get I2C0 handler");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }
    i2c0_priv.reg = IP_I2C0;

    /* 创建互斥锁 */
    i2c0_priv.mutex = lisa_mutex_create();
    if (!i2c0_priv.mutex) {
        LISA_LOGE(LOG_TAG, "Failed to create mutex");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 创建传输完成信号量 */
    i2c0_priv.xfer_sem = xSemaphoreCreateBinary();
    if (!i2c0_priv.xfer_sem) {
        LISA_LOGE(LOG_TAG, "Failed to create I2C0 transfer semaphore");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 初始化 HAL I2C */
    if (I2C_Initialize(i2c0_priv.hal_handler, i2c0_event_callback, &i2c0_priv) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to initialize I2C0");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 使能电源 */
    if (I2C_PowerControl(i2c0_priv.hal_handler, CSK_POWER_FULL) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to power on I2C0");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 设置传输模式为中断模式 */
    if (I2C_Control(i2c0_priv.hal_handler, CSK_I2C_TRANSMIT_MODE, 0) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to set I2C0 transmit mode");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 设置默认总线速度为标准模式 */
    if (I2C_Control(i2c0_priv.hal_handler, CSK_I2C_BUS_SPEED, CSK_I2C_BUS_SPEED_STANDARD) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to set I2C0 bus speed");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 配置 I2C0 引脚复用 */
    lisa_i2c0_pinmux();

    /* Pinmux must be active before resetting/clearing the controller, or the
     * recovery operation is disconnected from the physical SCL/SDA pads. */
    I2C_Control(i2c0_priv.hal_handler, CSK_I2C_BUS_CLEAR, 0);

    /* 设置默认配置 */
    i2c0_priv.config.speed = LISA_I2C_SPEED_STANDARD;
    i2c0_priv.config.master_mode = true;
    i2c0_priv.config.slave_addr = 0;

    LISA_LOGI(LOG_TAG, "I2C0 initialized successfully");

    return LISA_DEVICE_OK;
}
#endif

#if CONFIG_LISA_I2C1
static int arcs_i2c1_init(void)
{
    /* 清空私有数据 */
    memset(&i2c1_priv, 0, sizeof(lisa_i2c_priv_t));

    /* 获取 HAL I2C1 句柄 */
    i2c1_priv.hal_handler = I2C1();
    if (!i2c1_priv.hal_handler) {
        LISA_LOGE(LOG_TAG, "Failed to get I2C1 handler");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }
    i2c1_priv.reg = IP_I2C1;

    /* 创建互斥锁 */
    i2c1_priv.mutex = lisa_mutex_create();
    if (!i2c1_priv.mutex) {
        LISA_LOGE(LOG_TAG, "Failed to create mutex");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 创建传输完成信号量 */
    i2c1_priv.xfer_sem = xSemaphoreCreateBinary();
    if (!i2c1_priv.xfer_sem) {
        LISA_LOGE(LOG_TAG, "Failed to create I2C1 transfer semaphore");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 初始化 HAL I2C */
    if (I2C_Initialize(i2c1_priv.hal_handler, i2c1_event_callback, &i2c1_priv) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to initialize I2C1");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 使能电源 */
    if (I2C_PowerControl(i2c1_priv.hal_handler, CSK_POWER_FULL) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to power on I2C1");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 设置传输模式为中断模式 */
    if (I2C_Control(i2c1_priv.hal_handler, CSK_I2C_TRANSMIT_MODE, 0) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to set I2C1 transmit mode");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 设置默认总线速度为标准模式 */
    if (I2C_Control(i2c1_priv.hal_handler, CSK_I2C_BUS_SPEED, CSK_I2C_BUS_SPEED_STANDARD) != 0) {
        LISA_LOGE(LOG_TAG, "Failed to set I2C1 bus speed");
        return LISA_DEVICE_ERR_INIT_FAIL;
    }

    /* 配置 I2C1 引脚复用 */
    lisa_i2c1_pinmux();

    /* See I2C0: clear only after the physical pads are routed. */
    I2C_Control(i2c1_priv.hal_handler, CSK_I2C_BUS_CLEAR, 0);

    /* 设置默认配置 */
    i2c1_priv.config.speed = LISA_I2C_SPEED_STANDARD;
    i2c1_priv.config.master_mode = true;
    i2c1_priv.config.slave_addr = 0;

    LISA_LOGI(LOG_TAG, "I2C1 initialized successfully");

    return LISA_DEVICE_OK;
}
#endif

/* ===== 设备注册 ===== */

#if CONFIG_LISA_I2C0
LISA_DEVICE_REGISTER(i2c0,                        /* 设备名称 */
                     &arcs_i2c_api,               /* API指针 */
                     &i2c0_priv,                  /* 私有数据指针 */
                     NULL,                        /* 用户数据 */
                     arcs_i2c0_init,              /* 初始化函数 */
                     LISA_DEVICE_LEVEL_NORMAL,    /* 级别 */
                     LISA_DEVICE_PRIORITY_NORMAL); /* 优先级 */
#endif

#if CONFIG_LISA_I2C1
LISA_DEVICE_REGISTER(i2c1,                        /* 设备名称 */
                     &arcs_i2c_api,               /* API指针 */
                     &i2c1_priv,                  /* 私有数据指针 */
                     NULL,                        /* 用户数据 */
                     arcs_i2c1_init,              /* 初始化函数 */
                     LISA_DEVICE_LEVEL_NORMAL,    /* 级别 */
                     LISA_DEVICE_PRIORITY_NORMAL); /* 优先级 */
#endif
