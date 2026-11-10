/**
 * @file board.h
 * @brief arcs_mini_v2_1 board interface
 */

#pragma once

#include "pinmux.h"

/* Common device names used by the application-side peripheral services. */
#define SERVO_PWM_DEVICE_NAME   "pwm0"
#define SENSOR_I2C_DEVICE_NAME  "i2c0"
#define MPU6050_I2C_DEVICE_NAME SENSOR_I2C_DEVICE_NAME
#define PN532_I2C_DEVICE_NAME   SENSOR_I2C_DEVICE_NAME

#define BOARD_ARCS_MINI_V2_1_HAS_CAMERA 0
#define BOARD_ARCS_MINI_V2_1_HAS_DVP    0

/*
 * Camera sources are still compiled by apps/arcs-mini, even when their
 * devices are disabled by overlay.conf. Keep the legacy pin constants so
 * those translation units compile, but lisa_dvp_pinmux() is intentionally
 * empty and none of these pads is switched to DVP. In particular, PA13 is
 * physically the second TTP223 output on this board revision.
 */
#ifndef CAM_HSYNC_PIN
#define CAM_HSYNC_PIN 10
#endif
#ifndef CAM_VSYNC_PIN
#define CAM_VSYNC_PIN 11
#endif
#ifndef CAM_PCLK_PIN
#define CAM_PCLK_PIN 12
#endif
#ifndef CAM_D0_PIN
#define CAM_D0_PIN 13
#endif
#ifndef CAM_D1_PIN
#define CAM_D1_PIN 14
#endif
#ifndef CAM_D2_PIN
#define CAM_D2_PIN 15
#endif
#ifndef CAM_D3_PIN
#define CAM_D3_PIN 16
#endif
#ifndef CAM_D4_PIN
#define CAM_D4_PIN 17
#endif
#ifndef CAM_D5_PIN
#define CAM_D5_PIN 18
#endif
#ifndef CAM_D6_PIN
#define CAM_D6_PIN 19
#endif
#ifndef CAM_D7_PIN
#define CAM_D7_PIN 20
#endif
#ifndef CAM_MCLK_PIN
#define CAM_MCLK_PIN 26
#endif

#ifdef __cplusplus
extern "C" {
#endif

const char *board_get_name(void);

#ifdef __cplusplus
}
#endif
