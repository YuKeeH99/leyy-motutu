/* CONFIG_* 宏由 autoconf.h 经 -imacros 全局注入, 无需 include。 */
#if defined(CONFIG_CAR_SERVO_PWM)

#include <stdbool.h>
#include <stdint.h>

#define TAG "servo-pwm"
#include "lisa_log.h"

#include "ClockManager.h"
#include "Driver_GPT_PWM.h"
#include "IOMuxManager.h"
#include "lisa_device.h"
#include "pinmux.h"

#include "servo_backend.h"

/*
 * PWM 舵机后端: 4 路独立 PWM 驱动连续旋转舵机。
 * 引脚(func12): PA04=gpt_pwm_4(CH4), PA05=gpt_pwm_5(CH5), PA06=gpt_pwm_6(CH6), PA07=gpt_pwm_7(CH7)。
 * ID 1-4 -> CH4-7 -> PA04-07。
 *
 * 连续旋转舵机 50Hz PWM 脉宽调速: 1.5ms=停, 2.0ms=正转满速, 1.0ms=反转满速。
 * dps(度/秒,±) 线性映射到脉宽。
 *
 * CH4-7 属于 GPT "第二组", counter 靠直接置 CH_TIMER_ENABLE 启动(CH_CNT_EN 写不进)。
 * 详见 servo_driver_notes.md(servo-debug 分支验证过的方法)。
 */

#define PWM_FUNC               12U
#define IOMUX_PAD              CSK_IOMUX_PAD_A

/* 直接寄存器访问 */
#define REG32(addr)            (*(volatile uint32_t *)(uintptr_t)(addr))
#define IOMUX_BASE_ADDR        0x47500000UL
#define GPT_BASE_ADDR          0x47300000UL
#define GPT_RELOAD_OFFSET(ch)  (0x34U + (ch) * 4U)
#define GPT_CH_TIMER_ENABLE    0xB0U

#define SERVO_FREQ_HZ          50U
#define PULSE_STOP_US          1500U   /* 中位=停 */
#define PULSE_MIN_US           500U    /* 一个方向满速(1500-1000) */
#define PULSE_MAX_US           2500U   /* 另一方向满速(1500+1000) */
#define PULSE_SPAN_US          1000    /* 满速时相对中位的脉宽偏移 */
#define SERVO_MAX_DPS          1000    /* dps 满量程 */

/* ID(1-4) -> GPT 通道(4-7) -> 引脚(PA04-07) */
#define ID_TO_CHANNEL(id)      ((uint32_t)((id) + 3))   /* 1->4, 2->5, 3->6, 4->7 */
#define ID_TO_PIN(id)          ((uint8_t)((id) + 3))    /* 1->PA04 ... 4->PA07 */

typedef struct { uint32_t reg; uint8_t shift; } clk_div_t;
static const clk_div_t s_clk_divs[] = {
    {CSK_GPT_PWM_CLKDIV_1, 0},   {CSK_GPT_PWM_CLKDIV_2, 1},
    {CSK_GPT_PWM_CLKDIV_4, 2},   {CSK_GPT_PWM_CLKDIV_8, 3},
    {CSK_GPT_PWM_CLKDIV_16, 4},  {CSK_GPT_PWM_CLKDIV_32, 5},
    {CSK_GPT_PWM_CLKDIV_64, 6},  {CSK_GPT_PWM_CLKDIV_128, 7},
};

static void *s_pwm_hal = NULL;
static uint32_t s_effclk = 0;
static bool s_inited = false;

/* 强符号覆盖 board 弱符号: 配 PA04-07(CH4-7) + PA21(LCD背光CH1, 保留原功能) = func12。 */
void lisa_pwm_pinmux(void)
{
    IOMuxManager_PinConfigure(IOMUX_PAD, LCD_PWM_PIN, PWM_FUNC);   /* PA21 背光 */
    IOMuxManager_PinConfigure(IOMUX_PAD, 4, PWM_FUNC);
    IOMuxManager_PinConfigure(IOMUX_PAD, 5, PWM_FUNC);
    IOMuxManager_PinConfigure(IOMUX_PAD, 6, PWM_FUNC);
    IOMuxManager_PinConfigure(IOMUX_PAD, 7, PWM_FUNC);
}

static bool select_clk(uint32_t *control_div, uint32_t *effclk)
{
    uint32_t pclk = CRM_GetCmn_peri_pclkFreq();
    if (pclk == 0) {
        pclk = CRM_GetSrcFreq(CRM_IpSrcPeriClk);
    }
    if (pclk == 0) {
        return false;
    }
    for (uint32_t i = 0; i < sizeof(s_clk_divs) / sizeof(s_clk_divs[0]); ++i) {
        uint32_t clk = pclk >> s_clk_divs[i].shift;
        uint32_t period = clk / SERVO_FREQ_HZ;
        if (period > 0 && period <= UINT16_MAX) {
            *control_div = s_clk_divs[i].reg;
            *effclk = clk;
            return true;
        }
    }
    return false;
}

static uint16_t us_to_counts(uint32_t pulse_us)
{
    uint64_t counts = ((uint64_t)s_effclk * pulse_us + 500000ULL) / 1000000ULL;
    if (counts < 1U) {
        counts = 1U;
    } else if (counts > UINT16_MAX) {
        counts = UINT16_MAX;
    }
    return (uint16_t)counts;
}

/* 配置单个 PWM 通道(CH4-7 之一): 时钟源/分频/边沿对齐/PWM 模式。 */
static int pwm_channel_config(uint32_t channel, uint32_t control_div)
{
    uint32_t control = CSK_GPT_PWM_MODE |
                       CSK_GPT_PWM_CLKSRC_PCLK |
                       CSK_GPT_PWM_OUTMODE_EDGE_ALIGNED |
                       CSK_GPT_PWM_OUTPOLARITY_HIGH |
                       control_div |
                       CSK_GPT_PWM_OPERATION_MODE_PWM;
    return HAL_GPT_PWMControl(s_pwm_hal, control, (GPT_CHANNEL_TYPE)channel);
}

/* 给通道输出指定脉宽(us)。写 RELOAD + 置 CH_TIMER_ENABLE 对应位启动 counter。 */
static void pwm_channel_output(uint32_t channel, uint32_t pulse_us)
{
    uint32_t period_counts = s_effclk / SERVO_FREQ_HZ;
    uint16_t high = us_to_counts(pulse_us);
    uint16_t low;
    if (period_counts <= high) {
        low = 1U;
    } else if ((period_counts - high) > UINT16_MAX) {
        low = UINT16_MAX;
    } else {
        low = (uint16_t)(period_counts - high);
    }

    HAL_GPT_SetPWMFreqDutyByCounter_NoShadow(s_pwm_hal, (GPT_CHANNEL_TYPE)channel, high, low);
    HAL_GPT_ShadowLoad(s_pwm_hal, (GPT_CHANNEL_SYNC_TYPE)(1U << channel));

    /* 第二组通道(CH4-7)直接写 RELOAD + 置 TIMER_ENABLE bit(24+ch) 才真正出波形 */
    REG32(GPT_BASE_ADDR + GPT_RELOAD_OFFSET(channel)) = ((uint32_t)high << 16) | low;
    REG32(GPT_BASE_ADDR + GPT_CH_TIMER_ENABLE) |= (1U << (24 + channel));
}

int servo_backend_set_speed(uint8_t id, int16_t dps)
{
    if (!s_inited || id < 1 || id > 4) {
        return -1;
    }

    if (dps > SERVO_MAX_DPS) {
        dps = SERVO_MAX_DPS;
    } else if (dps < -SERVO_MAX_DPS) {
        dps = -SERVO_MAX_DPS;
    }

    /* dps -> 脉宽 (舵机规则: 1500=停; 1500->500=顺时针; 1500->2500=逆时针):
     *   dps=0   -> 1500us 停
     *   dps>0   -> >1500us (1500~2500) 逆时针转, +1000dps=2500us 最快
     *   dps<0   -> <1500us (1500~500)  顺时针转, -1000dps=500us  最快 */
    int32_t pulse = (int32_t)PULSE_STOP_US + (int32_t)dps * PULSE_SPAN_US / SERVO_MAX_DPS;
    if (pulse < (int32_t)PULSE_MIN_US) {
        pulse = PULSE_MIN_US;
    } else if (pulse > (int32_t)PULSE_MAX_US) {
        pulse = PULSE_MAX_US;
    }

    uint8_t pin = ID_TO_PIN(id);
    /* 每次夺回 pinmux, 对抗 ap 核覆盖 */
    IOMuxManager_PinConfigure(IOMUX_PAD, pin, PWM_FUNC);

    pwm_channel_output(ID_TO_CHANNEL(id), (uint32_t)pulse);
    return 0;
}

int servo_backend_set_pulse_us(uint8_t id, uint32_t pulse_us)
{
    if (!s_inited || id < 1 || id > 4) {
        return -1;
    }
    IOMuxManager_PinConfigure(IOMUX_PAD, ID_TO_PIN(id), PWM_FUNC);
    pwm_channel_output(ID_TO_CHANNEL(id), pulse_us);
    return 0;
}

int servo_backend_stop(uint8_t id)
{
    return servo_backend_set_speed(id, 0);   /* 0 dps = 1500us 中位 = 停 */
}

int servo_backend_stop_all(void)
{
    int r = 0;
    for (uint8_t id = 1; id <= 4; ++id) {
        if (servo_backend_stop(id) != 0) {
            r = -1;
        }
    }
    return r;
}

int servo_backend_init(void)
{
    uint32_t control_div;

    lisa_device_t *pwm_dev = lisa_device_get("pwm0");
    if (!pwm_dev || !lisa_device_ready(pwm_dev)) {
        LOGE("servo-pwm: pwm0 not ready");
        return -1;
    }

    s_pwm_hal = GPT0_PWM();
    if (!s_pwm_hal) {
        LOGE("servo-pwm: GPT0_PWM handle null");
        return -1;
    }

    if (!select_clk(&control_div, &s_effclk)) {
        LOGE("servo-pwm: no valid clk divider");
        return -1;
    }

    lisa_pwm_pinmux();

    s_inited = true;

    /* 配置并启动 4 个通道(CH4-7)。顺序对齐 servo-debug 已验证路径:
     * PWMControl -> 输出脉宽(写RELOAD+TIMER_ENABLE) -> EnablePWM。初始都停(1500us)。 */
    for (uint8_t id = 1; id <= 4; ++id) {
        uint32_t ch = ID_TO_CHANNEL(id);
        if (pwm_channel_config(ch, control_div) != 0) {
            LOGE("servo-pwm: ch%lu config failed", (unsigned long)ch);
            s_inited = false;
            return -1;
        }
        servo_backend_stop(id);   /* 输出 1500us 中位, 内部写 RELOAD + TIMER_ENABLE */
        HAL_GPT_EnablePWM(s_pwm_hal, (GPT_CHANNEL_TYPE)ch);
    }

    LOGW("servo-pwm: ready (CH4-7 = PA04-07, 50Hz, effclk=%u)", s_effclk);
    return 0;
}

const char *servo_backend_name(void)
{
    return "pwm(PA04-07)";
}

#endif /* CONFIG_CAR_SERVO_PWM */

