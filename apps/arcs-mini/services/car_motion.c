#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#define TAG "car"
#include "lisa_log.h"

#include "shell.h"

#include "servo_bus.h"
#include "car_motion.h"

/* 轮位对应舵机 ID (PA04-07) */
#define WHEEL_FL   1U   /* 左前 Front-Left  = PA04 */
#define WHEEL_FR   2U   /* 右前 Front-Right = PA05 */
#define WHEEL_RL   3U   /* 左后 Rear-Left   = PA06 */
#define WHEEL_RR   4U   /* 右后 Rear-Right  = PA07 */

/*
 * 麦克纳姆混控基向量 (servo 层实测标定, 2026-07-14):
 * 直接下发给 servo 1..4 (FL,FR,RL,RR) 的符号, 三组互相正交, 构成完备模型。
 *   前进  vy>0 : servo[+,-,+,-]   (已实测: 车前进)
 *   左移  vx>0 : servo[+,+,+,+]   (已实测: 车整体向左平移)
 *   左自转 w>0 : servo[+,+,-,-]   (已实测: 原地逆时针/车头左扫)
 * 由此每个 servo 命令是三分量的线性叠加:
 *   FL =  vy + vx + w
 *   FR = -vy + vx + w
 *   RL =  vy + vx - w
 *   RR = -vy + vx - w
 * 所有动作(前进/后退/左右平移/左右自转/左右转弯)都由 car_drive() 从这三分量合成,
 * 不再有"四轮组合贴错"的可能。麦轮无法靠锁轮差速转弯, 转弯=前进+自转。
 */
static bool s_car_ready = false;
static TimerHandle_t s_motion_stop_timer = NULL;
/* 斜向侧移增益(‰): 新麦轮侧向打滑使侧移分量缩水, 45°命令实走偏前(约35°)。
 * 放大侧移指令补偿, 使实际兑现的侧移追上前进 -> 合成回正45°。现场 `car cfg diag_side_gain` 调。
 * 放大后峰值归一化会自动限速保方向(见 car_diag_move)。1000=不放大。 */
static uint16_t s_diag_side_gain_permille = 1170U;  /* 新麦轮高速实测: 1170‰ 使左前45°命令实走≈45°(补侧滑缩水) */

/* 速度标定默认值(2026-07-14 实测: 前进3s走16cm=53mm/s低速, 高速3s走28cm=93mm/s;
 * 原地自转3s转150度=50dps/s低速; 转弯3s转45度=15dps/s低速。
 * 高速档按 DPS 400->1000 的 2.5x 比例推算, 平移取保守估计, 现场用 `car cfg` 微调。 */
#define CAR_WHEEL_DIAMETER_DEFAULT_MM        45U
#define CAR_LINEAR_MMPS_LOW_DEFAULT          49U  /* 2026-07 新麦轮实测: 前进30cm→30(旧53偏高欠走); 前进靠轮子主转不打滑, 定距准 */
#define CAR_LINEAR_MMPS_HIGH_DEFAULT         93U  /* 新麦轮高速实测: 前进30cm→30.5, 准(默认档) */
#define CAR_STRAFE_MMPS_LOW_DEFAULT          45U  /* 新麦轮: 取让平均≈30cm的值; 平移靠侧滚子, 打滑致单次距离随机波动±几cm, 开环无法消除 */
#define CAR_STRAFE_MMPS_HIGH_DEFAULT         83U  /* 新麦轮高速实测: 平移30cm→29.6, 准 */
#define CAR_TURN_INNER_PERMILLE_DEFAULT     300U
#define CAR_TURN_DEGPS_LOW_DEFAULT           15U
#define CAR_TURN_DEGPS_HIGH_DEFAULT          37U
#define CAR_SPIN_DEGPS_LOW_DEFAULT           50U
#define CAR_SPIN_DEGPS_HIGH_DEFAULT         125U
#define CAR_DIAG_MMPS_LOW_DEFAULT            47U  /* 新麦轮: 斜向定距受滚子打滑主导随机波动(同命令实走20~40cm), 开环标不准, 取折中让平均≈30 */
#define CAR_DIAG_MMPS_HIGH_DEFAULT           62U  /* 新麦轮高速实测: 45°定距30cm→29.5(打滑波动, 取平均准值) */
#define CAR_STRAFE_YAW_PERMILLE_DEFAULT      15  /* 2026-07 新麦轮实测: 无补偿左平移车头右偏, 补 15‰ 左自转即走直(旧值100是旧轮标定) */
#define CAR_DIAG_YAW_PERMILLE_DEFAULT         0  /* 新麦轮: 斜向偏航因右前轮偏弱而四方向不对称(左前偏左/右前偏右), 单一值补不平, 置中性待修硬件 */
#define CAR_DIAG_COAST_MM_DEFAULT            0U  /* 实测斜向无固定滑行, 纯速度误差; 定距误差成比例, 靠 diag_low/high 校准 */

static car_motion_calib_t s_car_calib = {
    .wheel_diameter_mm = CAR_WHEEL_DIAMETER_DEFAULT_MM,
    .linear_mm_per_s_low = CAR_LINEAR_MMPS_LOW_DEFAULT,
    .linear_mm_per_s_high = CAR_LINEAR_MMPS_HIGH_DEFAULT,
    .strafe_mm_per_s_low = CAR_STRAFE_MMPS_LOW_DEFAULT,
    .strafe_mm_per_s_high = CAR_STRAFE_MMPS_HIGH_DEFAULT,
    .turn_inner_permille = CAR_TURN_INNER_PERMILLE_DEFAULT,
    .turn_deg_per_s_low = CAR_TURN_DEGPS_LOW_DEFAULT,
    .turn_deg_per_s_high = CAR_TURN_DEGPS_HIGH_DEFAULT,
    .spin_deg_per_s_low = CAR_SPIN_DEGPS_LOW_DEFAULT,
    .spin_deg_per_s_high = CAR_SPIN_DEGPS_HIGH_DEFAULT,
    .diag_mm_per_s_low = CAR_DIAG_MMPS_LOW_DEFAULT,
    .diag_mm_per_s_high = CAR_DIAG_MMPS_HIGH_DEFAULT,
    .strafe_yaw_permille = CAR_STRAFE_YAW_PERMILLE_DEFAULT,
    .diag_yaw_permille = CAR_DIAG_YAW_PERMILLE_DEFAULT,
    .diag_coast_mm = CAR_DIAG_COAST_MM_DEFAULT,
    .strafe_layout = CAR_STRAFE_LAYOUT_DIAG,  /* 标准麦轮横移: 对角轮同向, AXLE 会让车身扭转上下晃 */
    .spin_layout = CAR_SPIN_LAYOUT_DIAG,
};

int car_stop(void);

/*
 * 两档速度 -> 舵机转速(dps)。servo 层 dps 满量程 1000 对应脉宽偏离中位 1000us(=500/2500us),
 * 线性映射, 所以 dps 值 ~= 脉宽偏离量(us)。
 *   低速: 偏离 ~120us   高速: 偏离满量程(1000us)
 * (连续旋转舵机死区约 ±30~50us, 低速取 120 确保越过死区又不太快)
 */
/* 低速偏离中位 400us(=1100/1900us), 越过死区且比满速慢; 高速满偏 1000us(=500/2500us)。
 * 实测若低速仍太快/太慢, 只需调 DPS_LOW。 */
#define DPS_LOW    400
#define DPS_HIGH   1000
static int16_t speed_to_dps(car_speed_t s)
{
    return (s == CAR_SPEED_HIGH) ? DPS_HIGH : DPS_LOW;
}

static uint32_t car_linear_mm_per_s(car_speed_t speed)
{
    return speed == CAR_SPEED_HIGH ? s_car_calib.linear_mm_per_s_high : s_car_calib.linear_mm_per_s_low;
}

static uint32_t car_strafe_mm_per_s(car_speed_t speed)
{
    return speed == CAR_SPEED_HIGH ? s_car_calib.strafe_mm_per_s_high : s_car_calib.strafe_mm_per_s_low;
}

static uint32_t car_diag_mm_per_s(car_speed_t speed)
{
    return speed == CAR_SPEED_HIGH ? s_car_calib.diag_mm_per_s_high : s_car_calib.diag_mm_per_s_low;
}

static uint16_t car_turn_deg_per_s(car_speed_t speed)
{
    return speed == CAR_SPEED_HIGH ? s_car_calib.turn_deg_per_s_high : s_car_calib.turn_deg_per_s_low;
}

static uint16_t car_spin_deg_per_s(car_speed_t speed)
{
    return speed == CAR_SPEED_HIGH ? s_car_calib.spin_deg_per_s_high : s_car_calib.spin_deg_per_s_low;
}

static void car_motion_stop_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    if (!s_car_ready) {
        return;
    }

    if (servo_stop_all() != 0) {
        LOGE("car timed stop failed");
        return;
    }

    LOGI("car timed stop");
}

static void car_cancel_stop_timer(void)
{
    if (s_motion_stop_timer != NULL) {
        xTimerStop(s_motion_stop_timer, 0);
    }
}

static int car_arm_stop_timer(uint32_t duration_ms)
{
    TickType_t ticks = 0;

    if (duration_ms == 0U) {
        return car_stop();
    }
    if (s_motion_stop_timer == NULL) {
        LOGE("motion stop timer not ready");
        return -1;
    }

    ticks = pdMS_TO_TICKS(duration_ms);
    if (ticks == 0U) {
        ticks = 1U;
    }

    xTimerStop(s_motion_stop_timer, 0);
    if (xTimerChangePeriod(s_motion_stop_timer, ticks, 0) != pdPASS) {
        LOGE("arm motion timer failed, duration_ms=%lu", (unsigned long)duration_ms);
        return -1;
    }

    LOGI("car timed motion armed: %lu ms", (unsigned long)duration_ms);
    return 0;
}

/*
 * 设置四轮"前进分量"(整车坐标: 正=前进)。left/right 为左右两侧分量(dps),
 * 各自乘方向符号下发到对应轮。差速/原地转靠左右分量不同实现。
 */
static int car_require_ready(void)
{
    if (!s_car_ready) {
        LOGE("car motion is not ready");
        return -1;
    }

    return 0;
}

static int car_prepare_for_motion(void)
{
    int ret = car_require_ready();

    if (ret != 0) {
        return ret;
    }

    car_cancel_stop_timer();
    return 0;
}

static int16_t car_clamp_dps(int32_t v)
{
    if (v > DPS_HIGH) {
        return (int16_t)DPS_HIGH;
    }
    if (v < -DPS_HIGH) {
        return (int16_t)(-DPS_HIGH);
    }
    return (int16_t)v;
}

/*
 * 统一麦轮混控: 三个整车速度分量 -> 四个 servo 命令。
 *   vy: 前后(正=前进)   vx: 左右平移(正=向左)   w: 自转(正=左自转/逆时针)
 * 基向量由 servo 层实测标定(见文件头), 叠加后限幅到 [-1000,1000]。
 *
 * 2026-07 换新麦轮(X布局)后, 用 `servo <id> +200` 逐轮单驱实测胎面顶部方向:
 *   FL(1)=前 FR(2)=后 RL(3)=前 RR(4)=后 => 前进列 servo[+,-,+,-]
 * 再手动摆位实测另两列:
 *   平移-左 servo[FL-,FR-,RL+,RR+] => vx 列 [-,-,+,+]
 *   左自转  servo[全 -]            => w  列 [-,-,-,-]
 * 三列均硬件实测(非推导), 已逐项验证前进/左右转弯/左右自转/左右平移方向正确。
 */
static int car_drive(int16_t vy, int16_t vx, int16_t w)
{
    int ret = 0;
    int16_t fl = car_clamp_dps((int32_t)vy - vx - w);
    int16_t fr = car_clamp_dps((int32_t)-vy - vx - w);
    int16_t rl = car_clamp_dps((int32_t)vy + vx - w);
    int16_t rr = car_clamp_dps((int32_t)-vy + vx - w);

    if (servo_set_speed(WHEEL_FL, fl) != 0) {
        ret = -1;
    }
    if (servo_set_speed(WHEEL_FR, fr) != 0) {
        ret = -1;
    }
    if (servo_set_speed(WHEEL_RL, rl) != 0) {
        ret = -1;
    }
    if (servo_set_speed(WHEEL_RR, rr) != 0) {
        ret = -1;
    }

    return ret;
}

static int car_start_timed_motion(int (*motion_fn)(car_speed_t), car_speed_t speed, uint32_t duration_ms)
{
    int ret = motion_fn(speed);

    if (ret != 0) {
        return ret;
    }

    return car_arm_stop_timer(duration_ms);
}

/*
 * distance_mm: 命令距离; speed_mm_per_s: 标定速度; coast_mm: 该运动类型的启停滑行补偿。
 * 开环定时运动停车后会因惯性多滑一段与命令距离无关的固定量, 先从目标里扣掉再算时间,
 * 否则短距离(如斜向 10cm)会被固定滑行放大成大误差。平移/前进滑行不明显, coast 传 0 即回退原公式。
 */
static uint32_t car_distance_to_duration_ms(uint32_t distance_mm, uint32_t speed_mm_per_s, uint32_t coast_mm)
{
    if (distance_mm == 0U) {
        return 0U;
    }
    if (speed_mm_per_s == 0U) {
        return 0U;
    }

    /* 目标小于滑行量时给个最小驱动时间(半个滑行距离对应的时长), 避免算出 0 完全不动。 */
    if (distance_mm <= coast_mm) {
        return (uint32_t)(((uint64_t)distance_mm * 1000ULL) / ((uint64_t)speed_mm_per_s * 2ULL));
    }
    return (uint32_t)(((uint64_t)(distance_mm - coast_mm) * 1000ULL) / speed_mm_per_s);
}

static uint32_t car_angle_to_duration_ms(uint16_t angle_deg, uint16_t deg_per_s)
{
    if (angle_deg == 0U) {
        return 0U;
    }
    if (deg_per_s == 0U) {
        return 0U;
    }

    return (uint32_t)(((uint64_t)angle_deg * 1000ULL) / deg_per_s);
}

int car_motion_is_ready(void)
{
    return s_car_ready ? 1 : 0;
}

int car_forward(car_speed_t speed)
{
    int16_t v = speed_to_dps(speed);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive(v, 0, 0);
    if (ret != 0) {
        LOGE("car forward apply failed");
        return ret;
    }

    LOGI("car forward %s", speed == CAR_SPEED_HIGH ? "high" : "low");
    return 0;
}

int car_backward(car_speed_t speed)
{
    int16_t v = speed_to_dps(speed);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive((int16_t)-v, 0, 0);
    if (ret != 0) {
        LOGE("car backward apply failed");
        return ret;
    }

    LOGI("car backward %s", speed == CAR_SPEED_HIGH ? "high" : "low");
    return 0;
}

int car_turn_left(car_speed_t speed)
{
    /* 麦轮转弯 = 前进 + 左自转(边走边左拐的弧线)。麦轮无法靠锁轮差速转弯,
     * 必须叠加自转分量。turn_inner_permille 控制自转占前进的比例。 */
    int16_t v = speed_to_dps(speed);
    int16_t w = (int16_t)(((int32_t)v * (int32_t)s_car_calib.turn_inner_permille) / 1000);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive(v, 0, w);
    if (ret != 0) {
        LOGE("car turn_left apply failed");
        return ret;
    }

    LOGI("car turn_left %s v=%d w=%d", speed == CAR_SPEED_HIGH ? "high" : "low", v, w);
    return 0;
}

int car_turn_right(car_speed_t speed)
{
    /* 麦轮转弯 = 前进 + 右自转(w 取负)。 */
    int16_t v = speed_to_dps(speed);
    int16_t w = (int16_t)(((int32_t)v * (int32_t)s_car_calib.turn_inner_permille) / 1000);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive(v, 0, (int16_t)-w);
    if (ret != 0) {
        LOGE("car turn_right apply failed");
        return ret;
    }

    LOGI("car turn_right %s v=%d w=%d", speed == CAR_SPEED_HIGH ? "high" : "low", v, w);
    return 0;
}

int car_spin_left(car_speed_t speed)
{
    /* 原地左自转(逆时针): 纯 w 分量, 实测 servo[+,+,-,-]。 */
    int16_t v = speed_to_dps(speed);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive(0, 0, v);
    if (ret != 0) {
        LOGE("car spin_left apply failed");
        return ret;
    }

    LOGI("car spin_left %s", speed == CAR_SPEED_HIGH ? "high" : "low");
    return 0;
}

int car_spin_right(car_speed_t speed)
{
    /* 原地右自转(顺时针): 纯 -w 分量。 */
    int16_t v = speed_to_dps(speed);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive(0, 0, (int16_t)-v);
    if (ret != 0) {
        LOGE("car spin_right apply failed");
        return ret;
    }

    LOGI("car spin_right %s", speed == CAR_SPEED_HIGH ? "high" : "low");
    return 0;
}

int car_strafe_left(car_speed_t speed)
{
    /* 向左平移: 纯 vx 分量, 实测 servo[+,+,+,+](四轮同向)。
     * 叠加 strafe_yaw 前馈补偿抵消开环平移跑偏(左平移车头易右偏, 补正 w=左自转纠正)。 */
    int16_t v = speed_to_dps(speed);
    int16_t w = (int16_t)(((int32_t)v * (int32_t)s_car_calib.strafe_yaw_permille) / 1000);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive(0, v, w);
    if (ret != 0) {
        LOGE("car strafe_left apply failed");
        return ret;
    }

    LOGI("car strafe_left %s w=%d", speed == CAR_SPEED_HIGH ? "high" : "low", w);
    return 0;
}

int car_strafe_right(car_speed_t speed)
{
    /* 向右平移: 纯 -vx 分量, servo[-,-,-,-]。补偿方向相反(右平移车头左偏, 补负 w=右自转纠正)。 */
    int16_t v = speed_to_dps(speed);
    int16_t w = (int16_t)(((int32_t)v * (int32_t)s_car_calib.strafe_yaw_permille) / 1000);
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    ret = car_drive(0, (int16_t)-v, (int16_t)-w);
    if (ret != 0) {
        LOGE("car strafe_right apply failed");
        return ret;
    }

    LOGI("car strafe_right %s w=%d", speed == CAR_SPEED_HIGH ? "high" : "low", w);
    return 0;
}

/*
 * 斜向角度分解表: 前分量 = v * fwd_permille/1000, 侧分量 = v * side_permille/1000。
 * 取值 = √2 * cos/sin, 使 45° 时两分量都=1000(等幅, 与旧实现一致, diag 速度标定不作废);
 * 合速度幅值 = √2*v 恒定, 与角度无关, 故 30/45/60 三档共用同一 diag 速度标定。
 * angle_deg: 相对车头正前方的偏转角, 越大越偏向侧移(60°接近平移), 越小越偏向正前(30°)。
 */
static void car_diag_angle_permille(uint16_t angle_deg, uint16_t *fwd_permille, uint16_t *side_permille)
{
    switch (angle_deg) {
    case 30U:  /* √2*cos30=1225, √2*sin30=707 : 前多侧少 */
        *fwd_permille = 1225U;
        *side_permille = 707U;
        break;
    case 60U:  /* √2*cos60=707, √2*sin60=1225 : 侧多前少 */
        *fwd_permille = 707U;
        *side_permille = 1225U;
        break;
    case 45U:
    default:   /* √2*cos45 = √2*sin45 = 1000 : 等幅, 同旧行为 */
        *fwd_permille = 1000U;
        *side_permille = 1000U;
        break;
    }
}

/*
 * 斜向移动: 前进/后退分量 + 左/右平移分量 按角度叠加(w 为偏航补偿)。
 * sy: +1=前进侧, -1=后退侧;  sx: +1=左, -1=右;  angle_deg: 30/45/60(其它按 45 处理)。
 * 平移侧吃 diag_yaw 偏航补偿(补偿量随实际侧分量大小走)。
 */
static int car_diag_move(car_speed_t speed, int sy, int sx, uint16_t angle_deg, const char *tag)
{
    int16_t v = speed_to_dps(speed);
    uint16_t fwd_permille = 1000U;
    uint16_t side_permille = 1000U;
    int16_t vy;
    int16_t vx;
    int16_t w;
    int ret = car_prepare_for_motion();

    if (ret != 0) {
        return ret;
    }

    car_diag_angle_permille(angle_deg, &fwd_permille, &side_permille);
    /* 侧移增益: 补偿新轮侧滑致侧移缩水, 把偏前的实际角度拉回目标角。 */
    side_permille = (uint16_t)(((uint32_t)side_permille * (uint32_t)s_diag_side_gain_permille) / 1000U);
    vy = (int16_t)(((int32_t)v * (int32_t)fwd_permille) / 1000);
    vx = (int16_t)(((int32_t)v * (int32_t)side_permille) / 1000);
    /* 偏航补偿正比于实际侧移分量, 侧分量越大(角度越大)补得越多。 */
    w = (int16_t)(((int32_t)vx * (int32_t)s_car_calib.diag_yaw_permille) / 1000);

    /* 峰值归一化: 45°斜向的主驱轮命令幅值 = |vy|+|vx|+|w|, 高速档易超限幅 DPS_HIGH。
     * 撞限幅会把四轮出力比例砍歪 -> 打滑跑偏。这里若峰值超限, 三分量等比例缩小到刚好=DPS_HIGH,
     * 方向(角度)不变、不再饱和。代价: 斜向最高速受限(合速度约半速), 这也是斜向天生比直行慢的原因。 */
    {
        int32_t peak = (int32_t)(vy >= 0 ? vy : -vy) + (int32_t)(vx >= 0 ? vx : -vx)
                     + (int32_t)(w >= 0 ? w : -w);
        if (peak > DPS_HIGH && peak > 0) {
            vy = (int16_t)(((int32_t)vy * DPS_HIGH) / peak);
            vx = (int16_t)(((int32_t)vx * DPS_HIGH) / peak);
            w  = (int16_t)(((int32_t)w  * DPS_HIGH) / peak);
        }
    }

    ret = car_drive((int16_t)(sy * vy), (int16_t)(sx * vx), (int16_t)(sx * w));
    if (ret != 0) {
        LOGE("car %s apply failed", tag);
        return ret;
    }

    LOGI("car %s %s deg=%u", tag, speed == CAR_SPEED_HIGH ? "high" : "low", angle_deg);
    return 0;
}

int car_diag_front_left(car_speed_t speed)
{
    return car_diag_move(speed, +1, +1, 45U, "diag_front_left");
}

int car_diag_front_right(car_speed_t speed)
{
    return car_diag_move(speed, +1, -1, 45U, "diag_front_right");
}

int car_diag_rear_left(car_speed_t speed)
{
    return car_diag_move(speed, -1, +1, 45U, "diag_rear_left");
}

int car_diag_rear_right(car_speed_t speed)
{
    return car_diag_move(speed, -1, -1, 45U, "diag_rear_right");
}

int car_diag_front_left_angle(car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move(speed, +1, +1, angle_deg, "diag_front_left");
}

int car_diag_front_right_angle(car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move(speed, +1, -1, angle_deg, "diag_front_right");
}

int car_diag_rear_left_angle(car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move(speed, -1, +1, angle_deg, "diag_rear_left");
}

int car_diag_rear_right_angle(car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move(speed, -1, -1, angle_deg, "diag_rear_right");
}

int car_stop(void)
{
    int ret = car_require_ready();

    if (ret != 0) {
        return ret;
    }

    car_cancel_stop_timer();
    ret = servo_stop_all();
    if (ret != 0) {
        LOGE("car stop apply failed");
        return ret;
    }

    LOGI("car stop");
    return 0;
}

int car_forward_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_forward,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_linear_mm_per_s(speed), 0U));
}

int car_backward_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_backward,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_linear_mm_per_s(speed), 0U));
}

int car_strafe_left_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_strafe_left,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_strafe_mm_per_s(speed), 0U));
}

int car_strafe_right_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_strafe_right,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_strafe_mm_per_s(speed), 0U));
}

int car_diag_front_left_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_diag_front_left,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_diag_mm_per_s(speed),
                                                              s_car_calib.diag_coast_mm));
}

int car_diag_front_right_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_diag_front_right,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_diag_mm_per_s(speed),
                                                              s_car_calib.diag_coast_mm));
}

int car_diag_rear_left_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_diag_rear_left,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_diag_mm_per_s(speed),
                                                              s_car_calib.diag_coast_mm));
}

int car_diag_rear_right_distance_mm(uint32_t distance_mm, car_speed_t speed)
{
    return car_start_timed_motion(car_diag_rear_right,
                                  speed,
                                  car_distance_to_duration_ms(distance_mm, car_diag_mm_per_s(speed),
                                                              s_car_calib.diag_coast_mm));
}

/*
 * 带斜向角度(30/45/60)的定距移动。合速度幅值与角度无关(见 car_diag_angle_permille),
 * 所以定时换算沿用同一 diag 速度与滑行补偿。sy/sx 决定四个方向。
 */
static int car_diag_move_distance_mm(uint32_t distance_mm, car_speed_t speed, int sy, int sx,
                                     uint16_t angle_deg, const char *tag)
{
    int ret = car_diag_move(speed, sy, sx, angle_deg, tag);
    if (ret != 0) {
        return ret;
    }
    return car_arm_stop_timer(car_distance_to_duration_ms(distance_mm, car_diag_mm_per_s(speed),
                                                          s_car_calib.diag_coast_mm));
}

int car_diag_front_left_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move_distance_mm(distance_mm, speed, +1, +1, angle_deg, "diag_front_left");
}

int car_diag_front_right_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move_distance_mm(distance_mm, speed, +1, -1, angle_deg, "diag_front_right");
}

int car_diag_rear_left_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move_distance_mm(distance_mm, speed, -1, +1, angle_deg, "diag_rear_left");
}

int car_diag_rear_right_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg)
{
    return car_diag_move_distance_mm(distance_mm, speed, -1, -1, angle_deg, "diag_rear_right");
}

int car_turn_left_angle_deg(uint16_t angle_deg, car_speed_t speed)
{
    return car_start_timed_motion(car_turn_left, speed, car_angle_to_duration_ms(angle_deg, car_turn_deg_per_s(speed)));
}

int car_turn_right_angle_deg(uint16_t angle_deg, car_speed_t speed)
{
    return car_start_timed_motion(car_turn_right, speed, car_angle_to_duration_ms(angle_deg, car_turn_deg_per_s(speed)));
}

int car_spin_left_angle_deg(uint16_t angle_deg, car_speed_t speed)
{
    return car_start_timed_motion(car_spin_left, speed, car_angle_to_duration_ms(angle_deg, car_spin_deg_per_s(speed)));
}

int car_spin_right_angle_deg(uint16_t angle_deg, car_speed_t speed)
{
    return car_start_timed_motion(car_spin_right, speed, car_angle_to_duration_ms(angle_deg, car_spin_deg_per_s(speed)));
}

int car_motion_get_calib(car_motion_calib_t *cfg)
{
    if (cfg == NULL) {
        return -1;
    }

    *cfg = s_car_calib;
    return 0;
}

int car_motion_set_wheel_diameter_mm(uint16_t diameter_mm)
{
    if (diameter_mm == 0U) {
        return -1;
    }

    s_car_calib.wheel_diameter_mm = diameter_mm;
    return 0;
}

int car_motion_set_linear_mm_per_s(car_speed_t speed, uint16_t mm_per_s)
{
    if (mm_per_s == 0U) {
        return -1;
    }

    if (speed == CAR_SPEED_HIGH) {
        s_car_calib.linear_mm_per_s_high = mm_per_s;
    } else {
        s_car_calib.linear_mm_per_s_low = mm_per_s;
    }
    return 0;
}

int car_motion_set_strafe_mm_per_s(car_speed_t speed, uint16_t mm_per_s)
{
    if (mm_per_s == 0U) {
        return -1;
    }

    if (speed == CAR_SPEED_HIGH) {
        s_car_calib.strafe_mm_per_s_high = mm_per_s;
    } else {
        s_car_calib.strafe_mm_per_s_low = mm_per_s;
    }

    return 0;
}

int car_motion_set_turn_inner_permille(uint16_t permille)
{
    if (permille > 1000U) {
        return -1;
    }

    s_car_calib.turn_inner_permille = permille;
    return 0;
}

int car_motion_set_turn_deg_per_s(car_speed_t speed, uint16_t deg_per_s)
{
    if (deg_per_s == 0U) {
        return -1;
    }

    if (speed == CAR_SPEED_HIGH) {
        s_car_calib.turn_deg_per_s_high = deg_per_s;
    } else {
        s_car_calib.turn_deg_per_s_low = deg_per_s;
    }

    return 0;
}

int car_motion_set_spin_deg_per_s(car_speed_t speed, uint16_t deg_per_s)
{
    if (deg_per_s == 0U) {
        return -1;
    }

    if (speed == CAR_SPEED_HIGH) {
        s_car_calib.spin_deg_per_s_high = deg_per_s;
    } else {
        s_car_calib.spin_deg_per_s_low = deg_per_s;
    }

    return 0;
}

int car_motion_set_diag_mm_per_s(car_speed_t speed, uint16_t mm_per_s)
{
    if (mm_per_s == 0U) {
        return -1;
    }

    if (speed == CAR_SPEED_HIGH) {
        s_car_calib.diag_mm_per_s_high = mm_per_s;
    } else {
        s_car_calib.diag_mm_per_s_low = mm_per_s;
    }

    return 0;
}

int car_motion_set_strafe_layout(car_strafe_layout_t layout)
{
    if (layout != CAR_STRAFE_LAYOUT_AXLE && layout != CAR_STRAFE_LAYOUT_DIAG) {
        return -1;
    }

    s_car_calib.strafe_layout = layout;
    return 0;
}

int car_motion_set_spin_layout(car_spin_layout_t layout)
{
    if (layout != CAR_SPIN_LAYOUT_SIDE && layout != CAR_SPIN_LAYOUT_DIAG) {
        return -1;
    }

    s_car_calib.spin_layout = layout;
    return 0;
}

int car_motion_set_strafe_yaw_permille(int16_t permille)
{
    if (permille > 1000 || permille < -1000) {
        return -1;
    }

    s_car_calib.strafe_yaw_permille = permille;
    return 0;
}

int car_motion_set_diag_yaw_permille(int16_t permille)
{
    if (permille > 1000 || permille < -1000) {
        return -1;
    }

    s_car_calib.diag_yaw_permille = permille;
    return 0;
}

int car_motion_set_diag_coast_mm(uint16_t coast_mm)
{
    s_car_calib.diag_coast_mm = coast_mm;
    return 0;
}

int car_motion_init(void)
{
    int stop_ret = 0;
    int ret = servo_bus_init();
    s_car_ready = false;
    if (ret != 0) {
        LOGE("car: servo_bus_init failed ret=%d", ret);
        return ret;
    }

    if (s_motion_stop_timer == NULL) {
        s_motion_stop_timer = xTimerCreate("car.stop",
                                           pdMS_TO_TICKS(1000U),
                                           pdFALSE,
                                           NULL,
                                           car_motion_stop_timer_cb);
        if (s_motion_stop_timer == NULL) {
            LOGE("car: create motion stop timer failed");
            return -1;
        }
    }

    s_car_ready = true;
    stop_ret = car_stop();
    if (stop_ret != 0) {
        s_car_ready = false;
        LOGE("car: initial stop failed ret=%d", stop_ret);
        return stop_ret;
    }

    LOGW("car_motion: ready (FL=1 FR=2 RL=3 RR=4, strafe=%s, spin=%s, wheel=%umm)",
         s_car_calib.strafe_layout == CAR_STRAFE_LAYOUT_AXLE ? "axle" : "diag",
         s_car_calib.spin_layout == CAR_SPIN_LAYOUT_DIAG ? "diag" : "side",
         s_car_calib.wheel_diameter_mm);
    return 0;
}

static car_speed_t car_parse_speed_arg(const char *arg)
{
    /* 默认高速: 仅显式 "low" 才切低速, 其它(含距离/角度数字参数)一律高速。 */
    return (arg != NULL && strcmp(arg, "low") == 0) ? CAR_SPEED_LOW : CAR_SPEED_HIGH;
}

static void car_print_cfg(Shell *sh)
{
    car_motion_calib_t cfg;

    if (car_motion_get_calib(&cfg) != 0) {
        shellPrint(sh, "car cfg: read failed\n");
        return;
    }

    shellPrint(sh,
               "car cfg: wheel=%umm linear_low=%u linear_high=%u strafe_low=%u strafe_high=%u diag_low=%u diag_high=%u turn_inner=%u "
               "turn_low=%u turn_high=%u spin_low=%u spin_high=%u strafe_yaw=%d diag_yaw=%d diag_coast=%u strafe=%s spin=%s\n",
               cfg.wheel_diameter_mm,
               cfg.linear_mm_per_s_low,
               cfg.linear_mm_per_s_high,
               cfg.strafe_mm_per_s_low,
               cfg.strafe_mm_per_s_high,
               cfg.diag_mm_per_s_low,
               cfg.diag_mm_per_s_high,
               cfg.turn_inner_permille,
               cfg.turn_deg_per_s_low,
               cfg.turn_deg_per_s_high,
               cfg.spin_deg_per_s_low,
               cfg.spin_deg_per_s_high,
               cfg.strafe_yaw_permille,
               cfg.diag_yaw_permille,
               cfg.diag_coast_mm,
               cfg.strafe_layout == CAR_STRAFE_LAYOUT_AXLE ? "axle" : "diag",
               cfg.spin_layout == CAR_SPIN_LAYOUT_DIAG ? "diag" : "side");
}

/*
 * 诊断 shell 命令: car
 *   car fwd|back|left|right|spinl|spinr|strafel|strafer [low|high]
 *   car fwdmm|backmm|strafelmm|strafermm <mm> [low|high]
 *   car turnldeg|turnrdeg|spinldeg|spinrdeg <deg> [low|high]
 *   car cfg show
 *   car cfg layout axle|diag
 *   car cfg spin_layout side|diag
 *   car cfg wheel <mm>
 *   car cfg linear_low|linear_high|strafe_low|strafe_high <mmps>
 *   car cfg turn_inner <permille>
 *   car cfg turn_low|turn_high|spin_low|spin_high <value>
 *   car stop
 */
static int car_cmd_handler(int argc, char *argv[])
{
    Shell *sh = shellGetCurrent();
    const char *a = NULL;
    car_speed_t sp = CAR_SPEED_HIGH;  /* 默认高速档(路况干扰小, 运动更稳定)。显式传 low 才切低速 */
    int ret = 0;

    if (argc < 2) {
        car_print_cfg(sh);
        shellPrint(sh, "usage: car fwd|back|left|right|spinl|spinr|strafel|strafer [low|high]\n");
        shellPrint(sh, "       car fl|fr|bl|br [deg(30/45/60)] [low|high]   (斜向, deg 默认45)\n");
        shellPrint(sh, "       car fwdmm|backmm|strafelmm|strafermm <mm> [low|high]\n");
        shellPrint(sh, "       car flmm|frmm|blmm|brmm <mm> [deg(30/45/60)] [low|high]\n");
        shellPrint(sh, "       car turnldeg|turnrdeg|spinldeg|spinrdeg <deg> [low|high]\n");
        shellPrint(sh, "       car cfg show|layout axle|diag|spin_layout side|diag|wheel <mm>\n");
        shellPrint(sh, "       car cfg linear_low|linear_high|strafe_low|strafe_high|diag_low|diag_high <mmps>|turn_inner <permille>\n");
        return 0;
    }

    if (strcmp(argv[1], "cfg") == 0) {
        if (argc < 3 || strcmp(argv[2], "show") == 0) {
            car_print_cfg(sh);
            return 0;
        }

        if (strcmp(argv[2], "layout") == 0 && argc >= 4) {
            ret = car_motion_set_strafe_layout(strcmp(argv[3], "diag") == 0 ? CAR_STRAFE_LAYOUT_DIAG
                                                                             : CAR_STRAFE_LAYOUT_AXLE);
        } else if (strcmp(argv[2], "spin_layout") == 0 && argc >= 4) {
            ret = car_motion_set_spin_layout(strcmp(argv[3], "side") == 0 ? CAR_SPIN_LAYOUT_SIDE
                                                                           : CAR_SPIN_LAYOUT_DIAG);
        } else if (strcmp(argv[2], "wheel") == 0 && argc >= 4) {
            ret = car_motion_set_wheel_diameter_mm((uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "linear_low") == 0 && argc >= 4) {
            ret = car_motion_set_linear_mm_per_s(CAR_SPEED_LOW, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "linear_high") == 0 && argc >= 4) {
            ret = car_motion_set_linear_mm_per_s(CAR_SPEED_HIGH, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "strafe_low") == 0 && argc >= 4) {
            ret = car_motion_set_strafe_mm_per_s(CAR_SPEED_LOW, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "strafe_high") == 0 && argc >= 4) {
            ret = car_motion_set_strafe_mm_per_s(CAR_SPEED_HIGH, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "turn_inner") == 0 && argc >= 4) {
            ret = car_motion_set_turn_inner_permille((uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "turn_low") == 0 && argc >= 4) {
            ret = car_motion_set_turn_deg_per_s(CAR_SPEED_LOW, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "turn_high") == 0 && argc >= 4) {
            ret = car_motion_set_turn_deg_per_s(CAR_SPEED_HIGH, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "spin_low") == 0 && argc >= 4) {
            ret = car_motion_set_spin_deg_per_s(CAR_SPEED_LOW, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "spin_high") == 0 && argc >= 4) {
            ret = car_motion_set_spin_deg_per_s(CAR_SPEED_HIGH, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "strafe_yaw") == 0 && argc >= 4) {
            ret = car_motion_set_strafe_yaw_permille((int16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "diag_low") == 0 && argc >= 4) {
            ret = car_motion_set_diag_mm_per_s(CAR_SPEED_LOW, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "diag_high") == 0 && argc >= 4) {
            ret = car_motion_set_diag_mm_per_s(CAR_SPEED_HIGH, (uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "diag_yaw") == 0 && argc >= 4) {
            ret = car_motion_set_diag_yaw_permille((int16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "diag_coast") == 0 && argc >= 4) {
            ret = car_motion_set_diag_coast_mm((uint16_t)atoi(argv[3]));
        } else if (strcmp(argv[2], "diag_side_gain") == 0 && argc >= 4) {
            s_diag_side_gain_permille = (uint16_t)atoi(argv[3]);
            ret = 0;
        } else {
            shellPrint(sh, "unknown cfg action\n");
            return 0;
        }

        shellPrint(sh, "car cfg ret=%d\n", ret);
        car_print_cfg(sh);
        return 0;
    }

    a = argv[1];
    sp = (argc >= 3) ? car_parse_speed_arg(argv[argc - 1]) : CAR_SPEED_HIGH;

    /* 斜向可选角度: 即时 `car fl [deg] [spd]` 取 argv[2]; 定距 `car flmm <mm> [deg] [spd]` 取 argv[3]。
     * 非数字或缺省(atoi=0)时按 45° 处理。 */
    uint16_t diag_deg_inst = (argc >= 3) ? (uint16_t)atoi(argv[2]) : 45U;
    uint16_t diag_deg_mm = (argc >= 4) ? (uint16_t)atoi(argv[3]) : 45U;
    if (diag_deg_inst == 0U) {
        diag_deg_inst = 45U;
    }
    if (diag_deg_mm == 0U) {
        diag_deg_mm = 45U;
    }

    if (strcmp(a, "fwd") == 0) {
        ret = car_forward(sp);
    } else if (strcmp(a, "back") == 0) {
        ret = car_backward(sp);
    } else if (strcmp(a, "left") == 0) {
        ret = car_turn_left(sp);
    } else if (strcmp(a, "right") == 0) {
        ret = car_turn_right(sp);
    } else if (strcmp(a, "spinl") == 0) {
        ret = car_spin_left(sp);
    } else if (strcmp(a, "spinr") == 0) {
        ret = car_spin_right(sp);
    } else if (strcmp(a, "strafel") == 0) {
        ret = car_strafe_left(sp);
    } else if (strcmp(a, "strafer") == 0) {
        ret = car_strafe_right(sp);
    } else if (strcmp(a, "fwdmm") == 0 && argc >= 3) {
        ret = car_forward_distance_mm((uint32_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "backmm") == 0 && argc >= 3) {
        ret = car_backward_distance_mm((uint32_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "strafelmm") == 0 && argc >= 3) {
        ret = car_strafe_left_distance_mm((uint32_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "strafermm") == 0 && argc >= 3) {
        ret = car_strafe_right_distance_mm((uint32_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "fl") == 0) {
        ret = car_diag_front_left_angle(sp, diag_deg_inst);
    } else if (strcmp(a, "fr") == 0) {
        ret = car_diag_front_right_angle(sp, diag_deg_inst);
    } else if (strcmp(a, "bl") == 0) {
        ret = car_diag_rear_left_angle(sp, diag_deg_inst);
    } else if (strcmp(a, "br") == 0) {
        ret = car_diag_rear_right_angle(sp, diag_deg_inst);
    } else if (strcmp(a, "flmm") == 0 && argc >= 3) {
        ret = car_diag_front_left_distance_mm_angle((uint32_t)atoi(argv[2]), sp, diag_deg_mm);
    } else if (strcmp(a, "frmm") == 0 && argc >= 3) {
        ret = car_diag_front_right_distance_mm_angle((uint32_t)atoi(argv[2]), sp, diag_deg_mm);
    } else if (strcmp(a, "blmm") == 0 && argc >= 3) {
        ret = car_diag_rear_left_distance_mm_angle((uint32_t)atoi(argv[2]), sp, diag_deg_mm);
    } else if (strcmp(a, "brmm") == 0 && argc >= 3) {
        ret = car_diag_rear_right_distance_mm_angle((uint32_t)atoi(argv[2]), sp, diag_deg_mm);
    } else if (strcmp(a, "turnldeg") == 0 && argc >= 3) {
        ret = car_turn_left_angle_deg((uint16_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "turnrdeg") == 0 && argc >= 3) {
        ret = car_turn_right_angle_deg((uint16_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "spinldeg") == 0 && argc >= 3) {
        ret = car_spin_left_angle_deg((uint16_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "spinrdeg") == 0 && argc >= 3) {
        ret = car_spin_right_angle_deg((uint16_t)atoi(argv[2]), sp);
    } else if (strcmp(a, "stop") == 0) {
        ret = car_stop();
    } else {
        shellPrint(sh, "unknown action: %s\n", a);
        return 0;
    }
    shellPrint(sh, "car: %s %s ret=%d\n", a, sp == CAR_SPEED_HIGH ? "high" : "low", ret);
    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 car, car_cmd_handler, car motion debug);
