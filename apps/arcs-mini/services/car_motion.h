#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * L2 小车原子运动层: 把 4 个驱动轮 PWM 舵机组合成小车运动。
 *
 * 轮位与舵机 ID: ID1=左前(PA04), ID2=右前(PA05), ID3=左后(PA06), ID4=右后(PA07)。
 * 左右轮物理镜像安装, 同一"前进"朝向左右轮转向相反(内部方向符号表处理)。
 *
 * 连续旋转舵机实测非线性、只有约 2-3 级有效速度, 故不用 0-100 百分比,
 * 改用两档速度: 低速(偏离中位小)/高速(偏离中位大)。
 */

/* 速度档 */
typedef enum {
    CAR_SPEED_LOW = 0,   /* 低速: 脉宽偏离中位小(~100us) */
    CAR_SPEED_HIGH = 1,  /* 高速: 脉宽偏离中位大(~满偏 1000us) */
} car_speed_t;

/* 横移轮组布局/混控模式。 */
typedef enum {
    CAR_STRAFE_LAYOUT_AXLE = 0, /* 前轮同向、后轮同向，适合当前这台麦轮车优先尝试 */
    CAR_STRAFE_LAYOUT_DIAG = 1, /* 对角轮同向，保留旧实现便于对照 */
} car_strafe_layout_t;

typedef enum {
    CAR_SPIN_LAYOUT_SIDE = 0, /* 左右两侧反向，传统差速原地转 */
    CAR_SPIN_LAYOUT_DIAG = 1, /* 对角轮同向，适合当前轮组装法异常时校准 */
} car_spin_layout_t;

typedef struct {
    uint16_t wheel_diameter_mm;       /* 轮胎直径，默认 45mm，仅保留为参考参数 */
    uint16_t linear_mm_per_s_low;     /* 实测直行低速，默认 16mm/s */
    uint16_t linear_mm_per_s_high;    /* 实测直行高速 */
    uint16_t strafe_mm_per_s_low;     /* 实测横移低速 */
    uint16_t strafe_mm_per_s_high;    /* 实测横移高速 */
    uint16_t turn_inner_permille;     /* 左/右转内侧轮速比例，避免锁死，1000=同速 */
    uint16_t turn_deg_per_s_low;      /* 差速转弯低速角速度 */
    uint16_t turn_deg_per_s_high;     /* 差速转弯高速角速度 */
    uint16_t spin_deg_per_s_low;      /* 原地旋转低速角速度 */
    uint16_t spin_deg_per_s_high;     /* 原地旋转高速角速度 */
    uint16_t diag_mm_per_s_low;       /* 斜向(45度)移动低速, 需单独标定(斜向只有一条对角轮转) */
    uint16_t diag_mm_per_s_high;      /* 斜向移动高速 */
    int16_t  strafe_yaw_permille;     /* 平移偏航前馈补偿(千分比 of 平移速度): 抵消开环平移跑偏。
                                       * 正=左平移时车头本会右偏,补一点左自转纠正; 现场 `car cfg strafe_yaw` 调 */
    int16_t  diag_yaw_permille;       /* 斜向偏航前馈补偿(千分比): 斜向含前进分量会自扶正, 需比平移小。
                                       * 现场 `car cfg diag_yaw` 调 */
    uint16_t diag_coast_mm;           /* 斜向定距启停滑行补偿(mm): 定时停车后惯性多滑的固定量,
                                       * 从命令距离里先扣掉再算时长; 现场 `car cfg diag_coast` 调 */
    car_strafe_layout_t strafe_layout;
    car_spin_layout_t spin_layout;
} car_motion_calib_t;

/* 初始化(内部调 servo_bus_init + 四轮使能)。返回 0 成功。 */
int car_motion_init(void);
int car_motion_is_ready(void);

int car_forward(car_speed_t speed);            /* 前进 */
int car_backward(car_speed_t speed);           /* 后退 */
int car_turn_left(car_speed_t speed);          /* 左转(差速: 左轮停/右轮前进) */
int car_turn_right(car_speed_t speed);         /* 右转 */
int car_spin_left(car_speed_t speed);          /* 原地左转(左轮后退/右轮前进) */
int car_spin_right(car_speed_t speed);         /* 原地右转 */
int car_strafe_left(car_speed_t speed);        /* 麦克纳姆横移: 向左平移 */
int car_strafe_right(car_speed_t speed);       /* 麦克纳姆横移: 向右平移 */
int car_diag_front_left(car_speed_t speed);    /* 斜向: 左前(前进+左移) */
int car_diag_front_right(car_speed_t speed);   /* 斜向: 右前(前进+右移) */
int car_diag_rear_left(car_speed_t speed);     /* 斜向: 左后(后退+左移) */
int car_diag_rear_right(car_speed_t speed);    /* 斜向: 右后(后退+右移) */
/* 带斜向角度(30/45/60, 相对车头正前方偏转; 其它值按 45 处理)的即时斜向。 */
int car_diag_front_left_angle(car_speed_t speed, uint16_t angle_deg);
int car_diag_front_right_angle(car_speed_t speed, uint16_t angle_deg);
int car_diag_rear_left_angle(car_speed_t speed, uint16_t angle_deg);
int car_diag_rear_right_angle(car_speed_t speed, uint16_t angle_deg);
int car_stop(void);                            /* 停止 */

/* 开环定时/定距/定角接口。执行后自动到时停车。 */
int car_forward_distance_mm(uint32_t distance_mm, car_speed_t speed);
int car_backward_distance_mm(uint32_t distance_mm, car_speed_t speed);
int car_strafe_left_distance_mm(uint32_t distance_mm, car_speed_t speed);
int car_strafe_right_distance_mm(uint32_t distance_mm, car_speed_t speed);
int car_diag_front_left_distance_mm(uint32_t distance_mm, car_speed_t speed);
int car_diag_front_right_distance_mm(uint32_t distance_mm, car_speed_t speed);
int car_diag_rear_left_distance_mm(uint32_t distance_mm, car_speed_t speed);
int car_diag_rear_right_distance_mm(uint32_t distance_mm, car_speed_t speed);
/* 带斜向角度(30/45/60)的定距斜向。 */
int car_diag_front_left_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg);
int car_diag_front_right_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg);
int car_diag_rear_left_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg);
int car_diag_rear_right_distance_mm_angle(uint32_t distance_mm, car_speed_t speed, uint16_t angle_deg);
int car_turn_left_angle_deg(uint16_t angle_deg, car_speed_t speed);
int car_turn_right_angle_deg(uint16_t angle_deg, car_speed_t speed);
int car_spin_left_angle_deg(uint16_t angle_deg, car_speed_t speed);
int car_spin_right_angle_deg(uint16_t angle_deg, car_speed_t speed);

/* 校准与调试接口。 */
int car_motion_get_calib(car_motion_calib_t *cfg);
int car_motion_set_wheel_diameter_mm(uint16_t diameter_mm);
int car_motion_set_linear_mm_per_s(car_speed_t speed, uint16_t mm_per_s);
int car_motion_set_strafe_mm_per_s(car_speed_t speed, uint16_t mm_per_s);
int car_motion_set_turn_inner_permille(uint16_t permille);
int car_motion_set_turn_deg_per_s(car_speed_t speed, uint16_t deg_per_s);
int car_motion_set_spin_deg_per_s(car_speed_t speed, uint16_t deg_per_s);
int car_motion_set_diag_mm_per_s(car_speed_t speed, uint16_t mm_per_s);
int car_motion_set_strafe_yaw_permille(int16_t permille);
int car_motion_set_diag_yaw_permille(int16_t permille);
int car_motion_set_diag_coast_mm(uint16_t coast_mm);
int car_motion_set_strafe_layout(car_strafe_layout_t layout);
int car_motion_set_spin_layout(car_spin_layout_t layout);

#ifdef __cplusplus
}
#endif
