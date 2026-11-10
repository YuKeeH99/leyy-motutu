#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 舵机后端抽象。由 servo_bus.c 调用, 具体实现由 Kconfig 二选一:
 *   CONFIG_CAR_SERVO_PWM -> servo_backend_pwm.c  (PWM 舵机, PA04-07 四路PWM)
 *   CONFIG_CAR_SERVO_BUS -> servo_backend_bus.c  (串口总线舵机, UART1 半双工)
 *
 * 上层(servo_bus.h / car_motion / mcp)只用抽象接口, 不感知后端差异。
 */

/* 初始化后端硬件(PWM 通道 或 UART)。返回 0 成功。 */
int servo_backend_init(void);

/* 设置舵机 id(1-4) 连续旋转速度。dps: 度/秒, 正负=方向, 0=停。返回 0 成功。 */
int servo_backend_set_speed(uint8_t id, int16_t dps);

/* 调试用: 让舵机 id 直接输出指定脉宽(us)的 PWM。仅 PWM 后端有效, 串口后端返回 -1。 */
int servo_backend_set_pulse_us(uint8_t id, uint32_t pulse_us);

/* 停止舵机 id。 */
int servo_backend_stop(uint8_t id);

/* 停止所有舵机。 */
int servo_backend_stop_all(void);

/* 后端名称(用于诊断打印)。 */
const char *servo_backend_name(void);

#ifdef __cplusplus
}
#endif
