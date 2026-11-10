#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "servo-bus"
#include "lisa_log.h"

#include "shell.h"

#include "servo_bus.h"
#include "servo_backend.h"

/*
 * L1 舵机层(公共分发)。公开接口保持稳定, 具体后端(PWM/串口)由 Kconfig 编译时二选一,
 * 见 servo_backend.h。L2 car_motion / L3 mcp 只依赖本层, 换后端无感知。
 */

static bool s_inited = false;

#define SERVO_ANGLE_MIN_DEG   (-90)
#define SERVO_ANGLE_MAX_DEG   90
#define SERVO_PULSE_MIN_US    500U
#define SERVO_PULSE_CENTER_US 1500U
#define SERVO_PULSE_MAX_US    2500U
#define SERVO_PULSE_SPAN_US   1000

static int servo_bus_ensure_inited(void)
{
    if (s_inited) {
        return 0;
    }

    return servo_bus_init();
}

int servo_bus_init(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* V2.1 PWM has exactly one owner: service_v2_actuators + gait.
     * Reject legacy lazy initialization from dog/car/MCP/web entry points. */
    LOGE("legacy servo backend disabled on V2.1; use v2hw gait/frame");
    return -1;
#else
    int ret = servo_backend_init();
    s_inited = (ret == 0);
    if (s_inited) {
        LOGW("servo-bus: ready (backend=%s)", servo_backend_name());
    } else {
        LOGE("servo-bus: backend init failed ret=%d", ret);
    }
    return ret;
#endif
}

int servo_set_speed(uint8_t id, int16_t dps)
{
    if (servo_bus_ensure_inited() != 0) {
        return -1;
    }
    return servo_backend_set_speed(id, dps);
}

int servo_set_pulse_us(uint8_t id, uint32_t pulse_us)
{
    if (servo_bus_ensure_inited() != 0) {
        return -1;
    }
    return servo_backend_set_pulse_us(id, pulse_us);
}

int servo_set_angle_deg(uint8_t id, int16_t angle_deg)
{
    int32_t pulse;

    if (angle_deg < SERVO_ANGLE_MIN_DEG) {
        angle_deg = SERVO_ANGLE_MIN_DEG;
    } else if (angle_deg > SERVO_ANGLE_MAX_DEG) {
        angle_deg = SERVO_ANGLE_MAX_DEG;
    }

    pulse = SERVO_PULSE_CENTER_US + ((int32_t)angle_deg * SERVO_PULSE_SPAN_US) / SERVO_ANGLE_MAX_DEG;
    if (pulse < (int32_t)SERVO_PULSE_MIN_US) {
        pulse = SERVO_PULSE_MIN_US;
    } else if (pulse > (int32_t)SERVO_PULSE_MAX_US) {
        pulse = SERVO_PULSE_MAX_US;
    }

    return servo_set_pulse_us(id, (uint32_t)pulse);
}

int servo_set_abs_angle_deg(uint8_t id, uint16_t angle_deg)
{
    if (angle_deg > 180U) {
        angle_deg = 180U;
    }

    return servo_set_pulse_us(id, SERVO_PULSE_MIN_US + ((uint32_t)angle_deg * 2000U) / 180U);
}

int servo_broadcast_speed(int16_t dps)
{
    /* PWM 后端无广播概念; 统一按逐个 id 下发(1-4)。串口后端可用真广播覆写。 */
    int r = 0;
    for (uint8_t id = 1; id <= 4; ++id) {
        int rr = servo_backend_set_speed(id, dps);
        if (rr != 0) {
            r = rr;
        }
    }
    return r;
}

int servo_stop(uint8_t id)
{
    if (servo_bus_ensure_inited() != 0) {
        return -1;
    }
    return servo_backend_stop(id);
}

int servo_stop_all(void)
{
    if (servo_bus_ensure_inited() != 0) {
        return -1;
    }
    return servo_backend_stop_all();
}

int servo_enable_torque(uint8_t id)
{
    (void)id;   /* PWM 舵机无扭矩开关; 串口后端在 init 内部处理。保留接口兼容。 */
    return 0;
}

int servo_enable_motor_mode(uint8_t id)
{
    (void)id;   /* 同上, PWM 舵机无此概念。 */
    return 0;
}

/*
 * 诊断 shell 命令: servo
 *   servo <id> <dps>   设置舵机 id(1-4) 速度(度/秒,正负=方向)
 *   servo stop         停止所有
 *   servo              打印状态/后端
 */
static int servo_cmd_handler(int argc, char *argv[])
{
    Shell *sh = shellGetCurrent();

    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        int r = servo_stop_all();
        shellPrint(sh, "servo: stop all ret=%d\n", r);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "us") == 0) {
        /* servo us <id> <脉宽us>: 调试, 让指定舵机恒定输出该脉宽 PWM */
        int id = atoi(argv[2]);
        int us = atoi(argv[3]);
        int r = servo_set_pulse_us((uint8_t)id, (uint32_t)us);
        shellPrint(sh, "servo: id=%d pulse=%dus ret=%d\n", id, us, r);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "deg") == 0) {
        int id = atoi(argv[2]);
        int deg = atoi(argv[3]);
        int r = servo_set_angle_deg((uint8_t)id, (int16_t)deg);
        shellPrint(sh, "servo: id=%d angle=%ddeg ret=%d\n", id, deg, r);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "abs") == 0) {
        int id = atoi(argv[2]);
        int deg = atoi(argv[3]);
        int r = servo_set_abs_angle_deg((uint8_t)id, (uint16_t)deg);
        shellPrint(sh, "servo: id=%d abs=%ddeg ret=%d\n", id, deg, r);
        return 0;
    }
    if (argc >= 3) {
        int id = atoi(argv[1]);
        int dps = atoi(argv[2]);
        int r = servo_set_speed((uint8_t)id, (int16_t)dps);
        shellPrint(sh, "servo: id=%d speed=%d dps ret=%d\n", id, dps, r);
        return 0;
    }

    shellPrint(sh, "servo: inited=%d backend=%s\n", (int)s_inited, servo_backend_name());
    shellPrint(sh, "usage: servo <id 1-4> <dps> | servo us <id> <us> | servo deg <id> <deg> | servo abs <id> <0-180> | servo stop\n");
    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 servo, servo_cmd_handler, servo bus debug);
