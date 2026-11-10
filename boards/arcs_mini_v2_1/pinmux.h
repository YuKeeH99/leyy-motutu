/*
 * Copyright (c) 2026, LISTENAI
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pinmux.h
 * @brief arcs_mini_v2_1 pin aliases and pinmux declarations
 *
 * The selected functions and function indices were verified against the
 * official LISA Pinmux Tool LS26/QFN76 database.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* PAD_A aliases. */
#define CAMERA_RST_PIN   0
#define PA_EN_PIN        1
#define AP_LOG_RX_PIN    2
#define AP_LOG_TX_PIN    3
#define SERVO_PWM4_PIN   4
#define SERVO_PWM5_PIN   5
#define SERVO_PWM6_PIN   6
#define SERVO_PWM7_PIN   7
#define WS2812_DATA_PIN  8
#define TOUCH_SENSOR_PIN    9
#define TOUCH_SENSOR_2_PIN  13
#define MPU_INT_PIN      12
#define LCD_PWM_PIN      21
#define LCD_CS_PIN       22
#define LCD_CD_PIN       23
#define LCD_SPI_DATA_PIN 24
#define LCD_SPI_CLK_PIN  25
#define LCD_TE_PIN       27

/* PAD_B aliases. */
#define USB_DET_PIN    0
#define LED_PIN        1
#define CP_LOG_TX_PIN  2
#define POWER_EN_PIN   3
#define POWER_KEY_PIN  4
#define BAT_ADC_PIN    5
#define I2C0_SDA_PIN   6
#define I2C0_SCL_PIN   7
#define CHARGE_DET_PIN 8
#define LCD_RST_PIN    9

/* PWM channel assignments from the LS26/QFN76 pinmux database. */
#define WS2812_PWM_CHANNEL 0U
#define LCD_PWM_CHANNEL    1U
#define SERVO_PWM4_CHANNEL 4U
#define SERVO_PWM5_CHANNEL 5U
#define SERVO_PWM6_CHANNEL 6U
#define SERVO_PWM7_CHANNEL 7U

/* GPIO device aliases. */
#define CAMERA_RST_DEVICE_NAME   "gpioa"
#define PA_EN_DEVICE_NAME        "gpioa"
#define WS2812_DATA_DEVICE_NAME  "gpioa"
#define TOUCH_SENSOR_DEVICE_NAME   "gpioa"
#define TOUCH_SENSOR_2_DEVICE_NAME "gpioa"
#define MPU_INT_DEVICE_NAME      "gpioa"
#define LCD_CD_DEVICE_NAME       "gpioa"
#define LCD_TE_DEVICE_NAME       "gpioa"
#define USB_DET_DEVICE_NAME      "gpiob"
#define LED_DEVICE_NAME          "gpiob"
#define POWER_EN_DEVICE_NAME     "gpiob"
#define POWER_KEY_DEVICE_NAME    "gpiob"
#define CHARGE_DET_DEVICE_NAME   "gpiob"
/* TP4056 CHRG is open drain: low while charging, R109 pulls idle high. */
#define CHARGE_DET_ACTIVE_LEVEL  0
#define LCD_RST_DEVICE_NAME      "gpiob"

void lisa_adc_pinmux(void);
void lisa_capture_pinmux(void);
void lisa_dvp_pinmux(void);
void lisa_gpioa_pinmux(void);
void lisa_gpiob_pinmux(void);
void lisa_i2c0_pinmux(void);
void lisa_i2c1_pinmux(void);
void lisa_i2s0_pinmux(void);
void lisa_i2s1_pinmux(void);
void lisa_jtag_pinmux(void);
void lisa_pwm_pinmux(void);
void lisa_qspi_lcd_pinmux(void);
void lisa_rgb_pinmux(void);
void lisa_sdio_pinmux(void);
void lisa_spi0_pinmux(void);
void lisa_spi1_pinmux(void);
void lisa_spi2_pinmux(void);
void lisa_uart0_pinmux(void);
void lisa_uart1_pinmux(void);
void lisa_uart2_pinmux(void);

/* PA08 defaults to GPIO. The WS2812 service may switch it explicitly. */
void board_ws2812_pinmux_gpio(void);
void board_ws2812_pinmux_pwm(void);

#ifdef __cplusplus
}
#endif
