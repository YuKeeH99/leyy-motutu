/* CONFIG_* 宏由 autoconf.h 经 -imacros 全局注入。 */
#if defined(CONFIG_CAR_SERVO_BUS)

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#define TAG "servo-bus-uart"
#include "lisa_log.h"

#include "IOMuxManager.h"
#include "lisa_device.h"
#include "lisa_uart.h"

#include "servo_backend.h"

/*
 * 串口总线舵机后端: 高德诚协议, UART1 半双工"只发不收"。
 * UART1 TX = PA04(func3 = uart1_txd), 115200/8N1。4 舵机并联共用总线, ID 区分。
 */

#define SERVO_UART_NAME    "uart1"
#define SERVO_UART_PAD     CSK_IOMUX_PAD_A
#define SERVO_TX_PIN       4U
#define SERVO_UART_FUNC    3U
#define SERVO_ID_BROADCAST 254U

#define FRAME_HEADER_HOST  0xABU
#define CMD_SYS_CONFIG     0x12U
#define CMD_TORQUE_SWITCH  0x40U
#define CMD_MOTOR_SPEED    0x49U
#define SYS_CFG_MOTOR_MODE_BIT (1U << 4)
#define TORQUE_ON          3U
#define SERVO_TX_TIMEOUT_MS 20U

static lisa_device_t *s_uart = NULL;
static bool s_inited = false;

/* 强符号: UART1 TX 映射到 PA04(func3), 替代 board 默认 CP 日志脚。 */
void lisa_uart1_pinmux(void)
{
    IOMuxManager_PinConfigure(SERVO_UART_PAD, SERVO_TX_PIN, SERVO_UART_FUNC);
}

/* 双重累加和校验(cks1=0xAB, cks2=0xAC 初值; 遍历含帧头的 len 字节)。已用 PDF 例子核对。 */
static uint8_t servo_checksum(const uint8_t *frame, uint32_t len)
{
    uint8_t cks1 = 0xAB, cks2 = 0xAC;
    for (uint32_t i = 0; i < len; ++i) {
        cks1 += frame[i];
        cks2 += cks1;
    }
    return (uint8_t)(cks1 + cks2);
}

static int servo_send(uint8_t id, uint8_t cmd, const uint8_t *data, uint32_t dlen)
{
    if (!s_inited || !s_uart) {
        return -1;
    }
    uint8_t frame[16];
    if (dlen > sizeof(frame) - 5U) {
        return -1;
    }
    IOMuxManager_PinConfigure(SERVO_UART_PAD, SERVO_TX_PIN, SERVO_UART_FUNC);

    uint32_t total = 5U + dlen;
    frame[0] = FRAME_HEADER_HOST;
    frame[1] = id;
    frame[2] = (uint8_t)total;
    frame[3] = cmd;
    for (uint32_t i = 0; i < dlen; ++i) {
        frame[4 + i] = data[i];
    }
    frame[total - 1] = servo_checksum(frame, total - 1);

    return lisa_uart_write_sync(s_uart, frame, total, SERVO_TX_TIMEOUT_MS);
}

int servo_backend_set_speed(uint8_t id, int16_t dps)
{
    uint8_t d[2] = { (uint8_t)(dps & 0xFF), (uint8_t)((dps >> 8) & 0xFF) };
    return servo_send(id, CMD_MOTOR_SPEED, d, 2);
}

int servo_backend_set_pulse_us(uint8_t id, uint32_t pulse_us)
{
    (void)id; (void)pulse_us;
    return -1;   /* 串口舵机无 PWM 脉宽概念 */
}

int servo_backend_stop(uint8_t id)
{
    return servo_backend_set_speed(id, 0);
}

int servo_backend_stop_all(void)
{
    uint8_t d[2] = { 0, 0 };
    return servo_send(SERVO_ID_BROADCAST, CMD_MOTOR_SPEED, d, 2);
}

static void servo_enable_torque(uint8_t id)
{
    uint8_t d = TORQUE_ON;
    servo_send(id, CMD_TORQUE_SWITCH, &d, 1);
}

static void servo_enable_motor_mode(uint8_t id)
{
    uint32_t cfg = (2U << 30) | SYS_CFG_MOTOR_MODE_BIT;
    uint8_t d[4] = {
        (uint8_t)(cfg & 0xFF), (uint8_t)((cfg >> 8) & 0xFF),
        (uint8_t)((cfg >> 16) & 0xFF), (uint8_t)((cfg >> 24) & 0xFF)
    };
    servo_send(id, CMD_SYS_CONFIG, d, 4);
}

int servo_backend_init(void)
{
    s_uart = lisa_device_get(SERVO_UART_NAME);
    if (!s_uart || !lisa_device_ready(s_uart)) {
        LOGE("servo-bus-uart: %s not ready", SERVO_UART_NAME);
        return -1;
    }

    lisa_uart_config_t cfg = LISA_UART_CONFIG_DEFAULT();   /* 115200/8N1 */
    if (lisa_uart_configure(s_uart, &cfg) != LISA_DEVICE_OK) {
        LOGE("servo-bus-uart: configure failed");
        return -1;
    }

    lisa_uart1_pinmux();
    s_inited = true;

    for (uint8_t id = 1; id <= 4; ++id) {
        servo_enable_motor_mode(id);
        vTaskDelay(pdMS_TO_TICKS(5));
        servo_enable_torque(id);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    LOGW("servo-bus-uart: ready (UART1 TX=PA04 func3, 115200, half-duplex)");
    return 0;
}

const char *servo_backend_name(void)
{
    return "bus(UART1)";
}

#endif /* CONFIG_CAR_SERVO_BUS */
