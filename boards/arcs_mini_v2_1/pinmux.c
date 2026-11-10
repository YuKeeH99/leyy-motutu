/*
 * Copyright (c) 2026, LISTENAI
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pinmux.c
 * @brief arcs_mini_v2_1 pinmux configuration
 *
 * Function indices follow the official LISA Pinmux Tool LS26/QFN76 data.
 */

#include "pinmux.h"
#include "IOMuxManager.h"

__attribute__((weak)) void lisa_adc_pinmux(void)
{
    AON_IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, BAT_ADC_PIN, 3);
}

__attribute__((weak)) void lisa_capture_pinmux(void)
{
}

/* PA12 is the MPU interrupt GPIO; DVP is deliberately unavailable. */
__attribute__((weak)) void lisa_dvp_pinmux(void)
{
}

__attribute__((weak)) void lisa_gpioa_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, CAMERA_RST_PIN, 1);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, PA_EN_PIN, 1);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, SERVO_PWM4_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, SERVO_PWM5_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, SERVO_PWM6_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, SERVO_PWM7_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, WS2812_DATA_PIN, 1);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, TOUCH_SENSOR_PIN, 1);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, MPU_INT_PIN, 0);
    /* PA13 is U7/U9 TTP223 OUT; function 0 selects GPIOA_13. */
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, TOUCH_SENSOR_2_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_CD_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_TE_PIN, 0);
}

__attribute__((weak)) void lisa_gpiob_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, USB_DET_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, LED_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, POWER_EN_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, POWER_KEY_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, CHARGE_DET_PIN, 0);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, LCD_RST_PIN, 0);
}

__attribute__((weak)) void lisa_i2c0_pinmux(void)
{
    /* MPU6050 and PN532 share SDA=PB06 and SCL=PB07. */
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, I2C0_SDA_PIN, 8);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, I2C0_SCL_PIN, 8);
}

__attribute__((weak)) void lisa_i2c1_pinmux(void)
{
    /* I2C1 is not routed to peripherals on this board revision. */
}

__attribute__((weak)) void lisa_i2s0_pinmux(void)
{
}

__attribute__((weak)) void lisa_i2s1_pinmux(void)
{
}

__attribute__((weak)) void lisa_jtag_pinmux(void)
{
}

__attribute__((weak)) void lisa_pwm_pinmux(void)
{
    /* Servo pins stay GPIO until an individual channel is explicitly enabled.
     * This avoids exposing the connectors to GPT during early boot. */
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_PWM_PIN, 12);
}

__attribute__((weak)) void lisa_qspi_lcd_pinmux(void)
{
}

__attribute__((weak)) void lisa_rgb_pinmux(void)
{
}

/* PA04-PA09 are PWM/GPIO on v2.1; SDIO is deliberately unavailable. */
__attribute__((weak)) void lisa_sdio_pinmux(void)
{
}

__attribute__((weak)) void lisa_spi0_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_CS_PIN, 5);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_SPI_DATA_PIN, 5);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, LCD_SPI_CLK_PIN, 5);
}

__attribute__((weak)) void lisa_spi1_pinmux(void)
{
}

__attribute__((weak)) void lisa_spi2_pinmux(void)
{
}

__attribute__((weak)) void lisa_uart0_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, AP_LOG_RX_PIN, 2);
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, AP_LOG_TX_PIN, 2);
}

/* UART1/UART2 are disabled by overlay.conf to protect the expansion pins. */
__attribute__((weak)) void lisa_uart1_pinmux(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_B, CP_LOG_TX_PIN, 3);
}

__attribute__((weak)) void lisa_uart2_pinmux(void)
{
}

void board_ws2812_pinmux_gpio(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, WS2812_DATA_PIN, 1);
}

void board_ws2812_pinmux_pwm(void)
{
    IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, WS2812_DATA_PIN, 12);
}
