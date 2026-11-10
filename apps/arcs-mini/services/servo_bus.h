#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * L1 舵机层: 高德诚标准总线舵机(半双工单总线串口)驱动。
 *
 * 4 个舵机并联在同一根信号线上, 靠 ID(1-4) 区分寻址。连续旋转/电机模式,
 * 用 0x49 恒速度命令控制转速与方向。
 *
 * 硬件: UART1 TX = PA04(func3), 115200/8N1, 半双工"只发不收"(单线发送)。
 * 舵机信号线接 PA04, GND 共地, 电源用外部供电。
 *
 * 协议帧: 帧头(0xAB) | ID | 长度 | 命令字 | 数据(小端) | 校验(双重累加和)
 * 详见 资料/高德诚标准舵机协议 和 xiaoche_notes.md。
 */

#define SERVO_ID_BROADCAST   254U   /* 广播 ID: 一帧发给所有舵机 */

/* 初始化 UART1 + 使能所有舵机的扭矩和电机模式。返回 0 成功。 */
int servo_bus_init(void);

/* 设置指定舵机连续旋转速度。dps: 目标转速(度/秒), 正负表示方向, 0=停止。
 * 使用 0x49 恒速度命令(持续转, 不计时)。返回 0 成功。 */
int servo_set_speed(uint8_t id, int16_t dps);

/* 直接设置指定舵机 PWM 脉宽(us)。普通位置舵机控制推荐走该接口。 */
int servo_set_pulse_us(uint8_t id, uint32_t pulse_us);

/* 把逻辑角度映射到常见 500~2500us PWM 脉宽:
 *   -90deg -> 500us
 *     0deg -> 1500us
 *   +90deg -> 2500us
 * 返回 0 成功。若实际安装角超出范围，应由上层先做夹紧。 */
int servo_set_angle_deg(uint8_t id, int16_t angle_deg);

/* 绝对角度接口，按常见位置舵机 0~180deg 语义映射:
 *     0deg -> 500us
 *    90deg -> 1500us
 *   180deg -> 2500us
 * 适合上电回中和单舵机调试。 */
int servo_set_abs_angle_deg(uint8_t id, uint16_t angle_deg);

/* 广播设置所有舵机速度(ID 254)。注意: 广播不区分左右轮镜像, 仅用于同步急停等。 */
int servo_broadcast_speed(int16_t dps);

/* 停止指定舵机(速度 0)。 */
int servo_stop(uint8_t id);

/* 停止所有舵机(广播速度 0)。 */
int servo_stop_all(void);

/* 打开指定舵机扭矩(0x40=3)。 */
int servo_enable_torque(uint8_t id);

/* 使能指定舵机电机模式(0x12 bit4=1), 连续旋转必需。 */
int servo_enable_motor_mode(uint8_t id);

#ifdef __cplusplus
}
#endif
