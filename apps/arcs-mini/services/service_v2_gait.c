#include "service_v2_gait.h"
#include "service_v2_idle_action.h"
#include "service_offline_action.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "lisa_log.h"
#include "lisa_mutex.h"
#include "lisa_thread.h"
#include "lisa_time.h"
#include "lisa_typedef.h"

#include "battery/battery.h"
#include "power/power_manager.h"

#include "service_v2_actuators.h"
#include "service_v2_rgb_effect.h"
#include "service_v2_gait_preflight_policy.h"
#include "service_v2_sensors.h"
#include "service_v2_imu_interaction.h"
#include "voice_msg.h"
#include "voice_player.h"

#define TAG "v2.gait"

#define GAIT_TASK_STACK  3072U
#define GAIT_QUEUE_DEPTH 4U

/* 播放线程等待/踢狗的粒度，和固件插值器的 20 ms 步长对齐。 */
#define GAIT_TICK_MS 20U

#define GAIT_WALK_FRAMES        8U
#define GAIT_WALK_FRAME_MS      80U
#define GAIT_RUN_FRAME_MS       40U
#define GAIT_TURN_FRAMES        4U
#define GAIT_TURN_FRAME_MS      150U
#define GAIT_APPROACH_CENTER_MS 300U
#define GAIT_APPROACH_PHASE_MS  160U
#define GAIT_POSE_MS            400U
#define GAIT_ASSEMBLY_MOVE_MS   700U
#define GAIT_HOME_RETURN_MS     400U
#define GAIT_STANDBY_STAND_MS       500U
#define GAIT_STANDBY_REAR_FOLD_MS  1200U
#define GAIT_STANDBY_FRONT_FOLD_MS 1200U

/*
 * Motion Studio 上实机通过的三拍起翻时间轴。宏保留绝对起点，下面的
 * 编译期断言会防止后续改单拍时长时悄悄把总时间改掉。
 */
#define GAIT_FLIP_HOME_MOVE_MS      250U
#define GAIT_FLIP_FRONT_START_MS    350U
#define GAIT_FLIP_FRONT_MOVE_MS     125U
#define GAIT_FLIP_REAR_START_MS     675U
#define GAIT_FLIP_REAR_MOVE_MS      425U
#define GAIT_FLIP_HOOK_START_MS     1330U
#define GAIT_FLIP_HOOK_MOVE_MS      1100U
#define GAIT_FLIP_AFTER_HOME_HOLD_MS (GAIT_FLIP_FRONT_START_MS - GAIT_FLIP_HOME_MOVE_MS)
#define GAIT_FLIP_AFTER_FRONT_HOLD_MS \
    (GAIT_FLIP_REAR_START_MS - (GAIT_FLIP_FRONT_START_MS + GAIT_FLIP_FRONT_MOVE_MS))
#define GAIT_FLIP_AFTER_REAR_HOLD_MS \
    (GAIT_FLIP_HOOK_START_MS - (GAIT_FLIP_REAR_START_MS + GAIT_FLIP_REAR_MOVE_MS))

/*
 * 完整翻滚测试的后续时间轴。正面半圈来自 2026-09-01 实机成功口令；反面
 * 半圈先按“前后职责互换、通道不换”解算，随后也由用户完成实机验证。
 * 每次越过平衡点后仍必须先通过 IMU 分面门，失败立即卸 PWM。
 */
#define GAIT_ROLL_REVERSE_FRONT_RETRACT_MS 350U
#define GAIT_ROLL_REVERSE_REAR_STAGE_MS    300U
#define GAIT_ROLL_REVERSE_HOOK_MS          1100U
#define GAIT_ROLL_REAR_RETURN_MS           350U
#define GAIT_ROLL_CROSS_MS                 225U
#define GAIT_ROLL_DANCE_ENTRY_MS           600U
#define GAIT_ROLL_DANCE_HALF_BEAT_MS       160U
#define GAIT_ROLL_DANCE_CYCLES             6U
#define GAIT_ROLL_RECOVER_PAIR_MS          250U
#define GAIT_ROLL_LANDING_TIMEOUT_MS       2500U
#define GAIT_ROLL_LANDING_SAMPLE_MS        20U
#define GAIT_ROLL_CYCLE_HOLD_MS            750U
#define GAIT_ROLL_START_HOLD_MS            800U
#define GAIT_ROLL_MIN_BATTERY_MV            3300U
#define GAIT_BACKFLIP_DRIVE_CYCLES             3U
#define GAIT_BACKFLIP_RELOAD_MS              320U
#define GAIT_BACKFLIP_REAR_DRIVE_MS          100U
#define GAIT_BACKFLIP_KICK_HOLD_MS           240U

/*
 * 2026-09-01 实机逐拍确认的 PA13 右前腿动作。板端插值器以 20 ms 更新，
 * 所以上位机调试时的 110 ms 快拍在固件中取 120 ms；下方还会回读插值器
 * 完成态，确保每次真的到达 1800/2000 us 端点后才反向。
 */
#define GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_STAGE1_MS 700U
#define GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_MS        400U
#define GAIT_RIGHT_FRONT_KICK_APPROACH_MS            500U
#define GAIT_RIGHT_FRONT_KICK_EXTEND_MS              260U
#define GAIT_RIGHT_FRONT_KICK_HALF_BEAT_MS           120U
#define GAIT_TENNIS_STRIKE_MS                          60U
#define GAIT_TENNIS_FRONT_HOME_MS                      20U
#define GAIT_TENNIS_REAR_KICK_MS                       60U
#define GAIT_TENNIS_HOME_SETTLE_MS                    120U
#define GAIT_TENNIS_SHARP_TURN_CYCLES                   3U
#define GAIT_TENNIS_SHARP_TURN_FRAME_MS                60U
#define GAIT_PAW_WAVE_HALF_BEAT_MS                    180U
#define GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS               350U
#define GAIT_TRICK_MOVE_MS                            400U
#define GAIT_DANCE_HALF_BEAT_MS                       250U
#define GAIT_DANCE_CYCLES                              45U
#define GAIT_DANCE_RECOVER_SUPPORT_MS                 250U
#define GAIT_DANCE_RECOVER_HOME_MS                    300U
#define GAIT_JUMP_SIT_MS                              500U
#define GAIT_JUMP_KICK_MS                              80U
#define GAIT_JUMP_RELEASE_MS                           80U
#define GAIT_BUTT_WIGGLE_HALF_BEAT_MS                 180U
#define GAIT_RIGHT_FRONT_ACTION_FRONT_HOME_MS 500U
#define GAIT_RIGHT_FRONT_ACTION_REAR_HOME_MS  500U
#define GAIT_RIGHT_FRONT_KICK_COLD_HOLD_MS    200U
#define GAIT_SERVO_PHYSICAL_SETTLE_MS          100U
#define GAIT_RIGHT_FRONT_ACTION_FINAL_HOLD_MS  100U
#define GAIT_FRAME_SETTLE_TIMEOUT_MS             60U
#define GAIT_HOME_TOLERANCE_US                  40U

/* 起翻超出平时走行标定区间，但不得超出实机已验证的 700..2300 us 动作包络。 */
#define GAIT_FLIP_MIN_US 700U
#define GAIT_FLIP_MAX_US 2300U

/*
 * 起翻只允许从正面朝上、接近水平的姿态起动。当前样机 HOME 的 Z 约为
 * -17922 LSB。连续 5 帧均值的 Z 必须仍显著为负，加速度模长必须像静止重力；
 * pitch 用 atan2(-Ax, hypot(Ay, Az)) 定义。下方用 tan(20°)≈0.364 的整数平方
 * 不等式实现，不给固件额外引入 libm。
 */
#define GAIT_FLIP_IMU_SAMPLE_COUNT       5U
#define GAIT_FLIP_IMU_SAMPLE_INTERVAL_MS 10U
#define GAIT_FLIP_FACE_UP_Z_MAX          (-12000)
#define GAIT_FLIP_FACE_REVERSE_Z_MIN     12000
#define GAIT_FLIP_ACCEL_NORM_MIN         12000U
#define GAIT_FLIP_ACCEL_NORM_MAX         22000U
#define GAIT_FLIP_ACCEL_AXIS_SPREAD_MAX  1000
/*
 * MPU6050 是 +/-250 dps，灵敏度 131 LSB/(deg/s)。实机静止偏置最高约 226 LSB；
 * 1500 LSB（约 11.5 dps）留出偏置/触摸振动余量，仍会拦住明显转动中的起翻。
 */
#define GAIT_FLIP_GYRO_ABS_MAX 1500
#define GAIT_FLIP_TAN_20_X1000           364U

/*
 * 当前样机的四路安装方向为 CH0/CH2 反向、CH1/CH3 正向。固定通道顺序是
 * CH0..CH3 = 原左前、原右前、原左后、原右后。旧 Home 的
 * 2000/1000 us 会让左前脚悬空。实机逐路确认后的 Home 为：左前 2100、右前
 * 900、原左后 2050、原右后 870 us。走行波形仍受 900..2100 us 限制；静态 Home 和
 * 起翻帧另外受实机验证的 700..2300 us 动作包络限制。
 */
#define GAIT_HOME_FL_US 2100U
#define GAIT_HOME_FR_US 900U
#define GAIT_HOME_RL_US 2050U
#define GAIT_HOME_RR_US 870U

/*
 * 步态中心和静态 Home 分开：左前 Home 已到 2100 us 软限位，不能围绕 Home
 * 做对称摆动。前后走行围绕 2000/1000 us，把 V5 基准波形缩放到实机最终确认的
 * 1925..2075 / 925..1075 us 安全行程；动作结束再回到上面的逐腿精确 Home。
 */
#define GAIT_CENTER_HIGH_US     2000U
#define GAIT_CENTER_LOW_US      1000U
#define GAIT_REFERENCE_HIGH_US  2000U
#define GAIT_REFERENCE_LOW_US   1000U
#define GAIT_TABLE_MAX_DELTA_US 60U
#define GAIT_CALIBRATED_MIN_US  900U
#define GAIT_CALIBRATED_MAX_US  2100U

/*
 * 摆幅缩放，百分比。
 *
 * 帧表存的是整机上调出来的 V5 基准摆幅（相对参考姿态 ±60 µs），下发前统一按
 * 这个比例缩放。实机对比后最终选 125%，四舍五入后为 ±75 µs，即
 * 150 µs / 约 22.5° 峰峰行程。慢走每帧 80 ms，快跑每帧 40 ms；两者只改频率，
 * 不改已验证的腿序和步幅。四路仍被下面的编译期断言限制在 900..2100 us 内。
 */
#define GAIT_AMPLITUDE_PCT 125U

#define GAIT_SCALED_MAX_DELTA_US ((GAIT_TABLE_MAX_DELTA_US * GAIT_AMPLITUDE_PCT + 50U) / 100U)

_Static_assert(GAIT_WALK_FRAMES *GAIT_WALK_FRAME_MS == SERVICE_V2_GAIT_WALK_CYCLE_MS,
               "walk cycle duration out of sync with the keyframe table");
_Static_assert(GAIT_WALK_FRAMES *GAIT_RUN_FRAME_MS == SERVICE_V2_GAIT_RUN_CYCLE_MS,
               "run cycle duration out of sync with the keyframe table");
_Static_assert(GAIT_TURN_FRAMES *GAIT_TURN_FRAME_MS == SERVICE_V2_GAIT_TURN_CYCLE_MS,
               "turn cycle duration out of sync with the keyframe table");
_Static_assert(GAIT_CENTER_HIGH_US + GAIT_SCALED_MAX_DELTA_US <= GAIT_CALIBRATED_MAX_US,
               "gait exceeds the calibrated high pulse limit");
_Static_assert(GAIT_CENTER_LOW_US >= GAIT_CALIBRATED_MIN_US + GAIT_SCALED_MAX_DELTA_US,
               "gait exceeds the calibrated low pulse limit");
_Static_assert(GAIT_HOME_FL_US >= GAIT_FLIP_MIN_US && GAIT_HOME_FL_US <= GAIT_FLIP_MAX_US &&
                   GAIT_HOME_FR_US >= GAIT_FLIP_MIN_US && GAIT_HOME_FR_US <= GAIT_FLIP_MAX_US &&
                   GAIT_HOME_RL_US >= GAIT_FLIP_MIN_US && GAIT_HOME_RL_US <= GAIT_FLIP_MAX_US &&
                   GAIT_HOME_RR_US >= GAIT_FLIP_MIN_US && GAIT_HOME_RR_US <= GAIT_FLIP_MAX_US,
               "Home pose exceeds the verified motion envelope");
_Static_assert(500U >= SERVICE_V2_SERVO_MIN_PULSE_US &&
                   2500U <= SERVICE_V2_SERVO_MAX_PULSE_US,
               "standby fold exceeds the servo hard pulse limits");
_Static_assert(GAIT_STANDBY_STAND_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U &&
                   GAIT_STANDBY_REAR_FOLD_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U &&
                   GAIT_STANDBY_FRONT_FOLD_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U,
               "standby fold frames must align with the servo interpolation step");
_Static_assert(GAIT_FLIP_HOOK_START_MS + GAIT_FLIP_HOOK_MOVE_MS == SERVICE_V2_GAIT_FLIP_RISE_DURATION_MS,
               "flip-rise duration out of sync with the fixed timeline");
_Static_assert(GAIT_FLIP_HOME_MOVE_MS <= GAIT_FLIP_FRONT_START_MS &&
                   GAIT_FLIP_FRONT_START_MS + GAIT_FLIP_FRONT_MOVE_MS <= GAIT_FLIP_REAR_START_MS &&
                   GAIT_FLIP_REAR_START_MS + GAIT_FLIP_REAR_MOVE_MS <= GAIT_FLIP_HOOK_START_MS,
               "flip-rise keyframes overlap");
_Static_assert(2300U >= GAIT_FLIP_MIN_US && 2300U <= GAIT_FLIP_MAX_US && 700U >= GAIT_FLIP_MIN_US &&
                   700U <= GAIT_FLIP_MAX_US && 2050U >= GAIT_FLIP_MIN_US && 2050U <= GAIT_FLIP_MAX_US &&
                   870U >= GAIT_FLIP_MIN_US && 870U <= GAIT_FLIP_MAX_US,
               "flip-rise front-retract frame exceeds the verified motion envelope");
_Static_assert(2300U >= GAIT_FLIP_MIN_US && 2300U <= GAIT_FLIP_MAX_US && 700U >= GAIT_FLIP_MIN_US &&
                   700U <= GAIT_FLIP_MAX_US && 2250U >= GAIT_FLIP_MIN_US && 2250U <= GAIT_FLIP_MAX_US,
               "flip-rise rear-load frame exceeds the verified motion envelope");
_Static_assert(2000U >= GAIT_FLIP_MIN_US && 2000U <= GAIT_FLIP_MAX_US && 1000U >= GAIT_FLIP_MIN_US &&
                   1000U <= GAIT_FLIP_MAX_US && 2250U >= GAIT_FLIP_MIN_US && 2250U <= GAIT_FLIP_MAX_US &&
                   700U >= GAIT_FLIP_MIN_US && 700U <= GAIT_FLIP_MAX_US,
               "flip-rise front-hook frame exceeds the verified motion envelope");

/*
 * 站立与趴下姿态。
 *
 * 四路的机械零位是反的（CH0/CH2 反向安装），所以同一个关节角在两侧的脉宽是
 * 关于 1500 µs 反向变化。静态 Home 再叠加本机逐腿 trim；趴下仍使用已经实测
 * 通过的 1750/1250 us。角度越大腿越外撇、支撑越少，趴下角必然大于站立角。
 */
static const uint16_t s_pose_stand[SERVICE_V2_SERVO_COUNT] = {GAIT_HOME_FL_US, GAIT_HOME_FR_US, GAIT_HOME_RL_US,
                                                              GAIT_HOME_RR_US};
static const uint16_t s_pose_prone[SERVICE_V2_SERVO_COUNT] = {1750U, 1250U, 1750U, 1250U};
/* 前腿向前、后腿向后水平伸直，供装配舵盘；到位后保持 PWM 扭矩。 */
static const uint16_t s_pose_assembly[SERVICE_V2_SERVO_COUNT] = {1500U, 1500U, 1615U, 1315U};
/*
 * 休眠携带姿态按实机给定端点执行。必须从 HOME 起步，先只收后腿，
 * 再让两条前腿向前转到最终位置；不能四路同时跨越整个行程。
 */
static const uint16_t s_standby_rear_folded[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2500U, 500U};
static const uint16_t s_standby_all_folded[SERVICE_V2_SERVO_COUNT] = {500U, 2500U, 2500U, 500U};
static const uint16_t s_gait_center[SERVICE_V2_SERVO_COUNT] = {GAIT_CENTER_HIGH_US, GAIT_CENTER_LOW_US,
                                                               GAIT_CENTER_HIGH_US, GAIT_CENTER_LOW_US};
static const uint16_t s_flip_front_retract[SERVICE_V2_SERVO_COUNT] = {2300U, 700U, 2050U, 870U};
static const uint16_t s_flip_rear_load[SERVICE_V2_SERVO_COUNT] = {2300U, 700U, 2250U, 700U};
static const uint16_t s_flip_front_hook[SERVICE_V2_SERVO_COUNT] = {2000U, 1000U, 2250U, 700U};

/* Backward first half, physical FL/FR/RR/RL. Bench-confirmed starting poses:
 * rear 2200/750 raises the head with front HOME; front 2200/800 increases
 * that tilt. Rear 1950/970 BEFORE front retraction instead pitched nose-down
 * in the failed video. Establish rear support first and let it settle.
 * The rear pair supplies the impulse: slow preload, then rapid backward
 * extension to the same rear coordinates used by the tennis kick. Front
 * support stays fixed throughout that stroke. Restore the user-confirmed
 * hop-then-backflip sequence; do not add a slow initial extension or a
 * front-release stage.
 * PWM endpoint confirmation is not physical shaft-angle feedback. */
static const uint16_t s_backflip_rear_support[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2200U, 750U};
static const uint16_t s_backflip_front_press[SERVICE_V2_SERVO_COUNT] = {2200U, 800U, 2200U, 750U};
static const uint16_t s_backflip_rear_preload[SERVICE_V2_SERVO_COUNT] = {2200U, 800U, 2250U, 670U};
static const uint16_t s_backflip_rear_drive[SERVICE_V2_SERVO_COUNT] = {2200U, 800U, 1615U, 1315U};
static const uint16_t s_backflip_recover_front[SERVICE_V2_SERVO_COUNT] = {1070U, 1800U, 2250U, 670U};

/*
 * 固定通道仍是 CH0..CH3 = 原左前、原右前、原左后、原右后。
 * 两条后腿物理同向时脉宽符号相反。2026-09-02 实机重新标定为
 * CH2 +150 us、CH3 -120 us，即 [2200,750]；该点后壳刚好悬空，不会像旧值
 * [2250,700] 那样过度收腿并坐入机械死点。右前腿只沿 HOME 的正方向前送。
 */
static const uint16_t s_right_front_kick_rear_preload_stage1[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2150U, 800U};
static const uint16_t s_right_front_kick_rear_preload[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2200U, 750U};
static const uint16_t s_right_front_kick_approach[SERVICE_V2_SERVO_COUNT] = {2100U, 1400U, 2200U, 750U};
static const uint16_t s_right_front_kick_base[SERVICE_V2_SERVO_COUNT] = {2100U, 1800U, 2200U, 750U};
static const uint16_t s_right_front_kick_forward[SERVICE_V2_SERVO_COUNT] = {2100U, 2000U, 2200U, 750U};
/* 发球专用后腿蓄力；普通右前腿动作仍使用上面的实机标定帧。 */
static const uint16_t s_tennis_rear_preload_stage1[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2180U, 770U};
static const uint16_t s_tennis_rear_preload[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2230U, 720U};
static const uint16_t s_tennis_approach[SERVICE_V2_SERVO_COUNT] = {2100U, 1400U, 2230U, 720U};
static const uint16_t s_tennis_base[SERVICE_V2_SERVO_COUNT] = {2100U, 1800U, 2230U, 720U};
static const uint16_t s_tennis_forward[SERVICE_V2_SERVO_COUNT] = {2100U, 2000U, 2230U, 720U};
/* 收拍后后腿立即向后蹬，复用装配位已确认的后腿水平坐标。 */
static const uint16_t s_tennis_rear_kick[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 1615U, 1315U};
/* 网球落地击球：先跑位/侧移，再以坐姿蓄力完成前后腿同时爆发。 */
static const uint16_t s_tennis_forehand_ready[SERVICE_V2_SERVO_COUNT] = {2100U, 1500U, 2230U, 720U};
static const uint16_t s_tennis_forehand_strike[SERVICE_V2_SERVO_COUNT] = {2100U, 2000U, 1615U, 1315U};
static const uint16_t s_tennis_backhand_ready[SERVICE_V2_SERVO_COUNT] = {1900U, 900U, 2230U, 720U};
static const uint16_t s_tennis_backhand_strike[SERVICE_V2_SERVO_COUNT] = {1400U, 900U, 1615U, 1315U};
static const uint16_t s_tennis_double_backhand_ready[SERVICE_V2_SERVO_COUNT] = {1900U, 1100U, 2230U, 720U};
static const uint16_t s_tennis_double_backhand_strike[SERVICE_V2_SERVO_COUNT] = {1450U, 1550U, 1615U, 1315U};

/* 新产品动作均限制在已经验证过的 700..2300 us 大动作包络内。 */
/* 握手只围绕装配水平位（1500 us）小幅摆动，不再继续抬到头顶。 */
static const uint16_t s_left_front_handshake_base[SERVICE_V2_SERVO_COUNT] = {1600U, 900U, 2200U, 750U};
static const uint16_t s_left_front_handshake_wave[SERVICE_V2_SERVO_COUNT] = {1400U, 900U, 2200U, 750U};
static const uint16_t s_right_front_handshake_base[SERVICE_V2_SERVO_COUNT] = {2100U, 1400U, 2200U, 750U};
static const uint16_t s_right_front_handshake_wave[SERVICE_V2_SERVO_COUNT] = {2100U, 1600U, 2200U, 750U};
static const uint16_t s_greeting_base[SERVICE_V2_SERVO_COUNT] = {2100U, 1500U, 2200U, 750U};
static const uint16_t s_greeting_wave[SERVICE_V2_SERVO_COUNT] = {2100U, 1800U, 2200U, 750U};
static const uint16_t s_scratch_base[SERVICE_V2_SERVO_COUNT] = {2100U, 2000U, 2200U, 750U};
static const uint16_t s_scratch_wave[SERVICE_V2_SERVO_COUNT] = {2100U, 2300U, 2200U, 750U};
static const uint16_t s_pose_sit[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2300U, 700U};
/* Standing stretch, physical channels FL, FR, RR, RL. Rear legs bend
 * forward only 111 us from calibrated HOME (nominally 10 degrees with
 * 2000 us / 180 degrees; actual angle depends on servo installation).
 * Keep the existing low front stretch and two slow 150 us tremors. */
static const uint16_t s_idle_stretch_support[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2161U, 759U};
static const uint16_t s_idle_stretch_extend[SERVICE_V2_SERVO_COUNT] = {1750U, 1250U, 2161U, 759U};
static const uint16_t s_idle_stretch_a[SERVICE_V2_SERVO_COUNT] = {1900U, 1100U, 2161U, 759U};
static const uint16_t s_idle_stretch_b[SERVICE_V2_SERVO_COUNT] = {1800U, 1200U, 2161U, 759U};

/* Initial floor-test poses: rear extension uses the calibrated assembly
 * position; front paws move slightly behind HOME before lowering the chest. */
static const uint16_t s_pushup_rear[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 1615U, 1315U};
static const uint16_t s_pushup_up[SERVICE_V2_SERVO_COUNT] = {2150U, 850U, 1615U, 1315U};
static const uint16_t s_pushup_down[SERVICE_V2_SERVO_COUNT] = {2300U, 700U, 1615U, 1315U};
static const uint16_t s_kiss_left[SERVICE_V2_SERVO_COUNT] = {1800U, 900U, 2150U, 800U};
static const uint16_t s_kiss_right[SERVICE_V2_SERVO_COUNT] = {2100U, 1200U, 2150U, 800U};
/*
 * 120 BPM 机械舞：后腿动作严格收在 HOME 附近，避免连续深蹲让屁股落地；
 * 抬前爪后必须穿插四脚支撑帧，禁止双前爪同时悬空。
 */
static const uint16_t s_dance_squat[SERVICE_V2_SERVO_COUNT] = {2070U, 930U, 2110U, 810U};
static const uint16_t s_dance_pop[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 1990U, 930U};
static const uint16_t s_dance_diagonal_a[SERVICE_V2_SERVO_COUNT] = {2040U, 840U, 2110U, 930U};
static const uint16_t s_dance_diagonal_b[SERVICE_V2_SERVO_COUNT] = {2160U, 960U, 1990U, 810U};
static const uint16_t s_dance_left_paw[SERVICE_V2_SERVO_COUNT] = {1500U, 900U, 2110U, 930U};
static const uint16_t s_dance_right_paw[SERVICE_V2_SERVO_COUNT] = {2100U, 1500U, 1990U, 810U};
static const uint16_t s_dance_hip_left[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2100U, 920U};
static const uint16_t s_dance_hip_right[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2000U, 820U};
/* 舞蹈专用回位先让后腿重新承重，再回到四脚 HOME；不能复用深蹲预载回位。 */
static const uint16_t s_dance_recover_support[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 1990U, 930U};
/* 跳跃蹬出：与装配方向一致，前腿向前、后腿向后水平伸展。 */
static const uint16_t s_jump_kick[SERVICE_V2_SERVO_COUNT] = {1500U, 1500U, 1615U, 1315U};
/* 扭屁股时前腿保持 HOME 支撑，后腿以同脉宽方向交替形成左右摆动。 */
static const uint16_t s_butt_wiggle_left[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 2180U, 1000U};
static const uint16_t s_butt_wiggle_right[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 1920U, 740U};
/*
 * 手动翻倒后的反馈必须保持“普通站立关节角度”：身体朝天时，这组角度会让
 * 四只脚自然朝上。不能用翻滚链的 reverse HOME，否则脚会重新转向地面。
 */
static const uint16_t s_upside_feedback_wiggle_a[SERVICE_V2_SERVO_COUNT] = {2040U, 840U, 2110U, 930U};
static const uint16_t s_upside_feedback_wiggle_b[SERVICE_V2_SERVO_COUNT] = {2160U, 960U, 1990U, 810U};

/*
 * 正面半圈到第一次 180° 的后续实测帧。数组永远是固定 CH0..CH3，不能按
 * 翻面后的“新前腿/新后腿”重新排序。
 */
static const uint16_t s_roll_normal_dance_base[SERVICE_V2_SERVO_COUNT] = {2000U, 1000U, 1450U, 1630U};
static const uint16_t s_roll_normal_dance_kick[SERVICE_V2_SERVO_COUNT] = {2000U, 1000U, 1300U, 1480U};
static const uint16_t s_roll_normal_rear_home[SERVICE_V2_SERVO_COUNT] = {2000U, 1000U, 2050U, 870U};
static const uint16_t s_roll_normal_cross[SERVICE_V2_SERVO_COUNT] = {1700U, 1300U, 2050U, 870U};
static const uint16_t s_roll_recover_reverse_front[SERVICE_V2_SERVO_COUNT] = {1700U, 1300U, 1150U, 1900U};

/*
 * 第一次翻面后：当前世界前腿是 CH2/CH3，当前世界后腿是 CH0/CH1。
 * H_rev/P_rev 以及 stage/hook/cross 整条链均已通过实机验证。
 */
static const uint16_t s_pose_reverse_home[SERVICE_V2_SERVO_COUNT] = {1100U, 1900U, 1150U, 1900U};
static const uint16_t s_roll_reverse_front_retract[SERVICE_V2_SERVO_COUNT] = {1100U, 1900U, 850U, 2200U};
static const uint16_t s_roll_reverse_rear_stage1[SERVICE_V2_SERVO_COUNT] = {1200U, 1800U, 850U, 2200U};
static const uint16_t s_roll_reverse_rear_load[SERVICE_V2_SERVO_COUNT] = {1300U, 1730U, 850U, 2200U};
static const uint16_t s_roll_reverse_front_hook[SERVICE_V2_SERVO_COUNT] = {1300U, 1730U, 1150U, 1900U};
static const uint16_t s_roll_reverse_rear_home[SERVICE_V2_SERVO_COUNT] = {1100U, 1900U, 1150U, 1900U};
static const uint16_t s_roll_reverse_cross[SERVICE_V2_SERVO_COUNT] = {1100U, 1900U, 1450U, 1600U};
static const uint16_t s_roll_recover_normal_front[SERVICE_V2_SERVO_COUNT] = {2100U, 900U, 1450U, 1600U};

_Static_assert(700U >= GAIT_FLIP_MIN_US && 2300U <= GAIT_FLIP_MAX_US,
               "full-roll keyframes exceed the verified motion envelope");
_Static_assert(750U >= GAIT_FLIP_MIN_US && 2200U <= GAIT_FLIP_MAX_US &&
                   900U >= GAIT_FLIP_MIN_US && 2000U <= GAIT_FLIP_MAX_US,
               "right-front-kick keyframes exceed the verified motion envelope");
_Static_assert(GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_STAGE1_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U &&
                   GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U &&
                   GAIT_RIGHT_FRONT_KICK_HALF_BEAT_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U,
               "right-front-kick frames must align with the servo interpolation step");
_Static_assert(GAIT_RIGHT_FRONT_ACTION_FRONT_HOME_MS > 0U &&
                   GAIT_RIGHT_FRONT_ACTION_FRONT_HOME_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U &&
                   GAIT_RIGHT_FRONT_ACTION_REAR_HOME_MS > 0U &&
                   GAIT_RIGHT_FRONT_ACTION_REAR_HOME_MS % SERVICE_V2_SERVO_FRAME_STEP_MS == 0U,
               "right-front action HOME recovery must align with the servo interpolation step");
_Static_assert(GAIT_SERVO_PHYSICAL_SETTLE_MS > 0U &&
                   GAIT_SERVO_PHYSICAL_SETTLE_MS % GAIT_TICK_MS == 0U &&
                   GAIT_RIGHT_FRONT_ACTION_FINAL_HOLD_MS > 0U &&
                   GAIT_RIGHT_FRONT_ACTION_FINAL_HOLD_MS % GAIT_TICK_MS == 0U,
               "right-front action physical settling must align with the gait tick");

/*
 * V5 帧表是在旧 2000/1000 Home 上实测得到的。计算摆幅时必须继续以这个参考姿态
 * 取 delta，再把 delta 叠加到独立步态中心；否则只改 Home 会把每条腿的摆幅压歪。
 */
static const uint16_t s_gait_reference_stand[SERVICE_V2_SERVO_COUNT] = {GAIT_REFERENCE_HIGH_US, GAIT_REFERENCE_LOW_US,
                                                                        GAIT_REFERENCE_HIGH_US, GAIT_REFERENCE_LOW_US};

/*
 * 前进步态：沿用实机验证过的四拍相位表，8 个关键帧线性插值走一周期。
 * 表里保留 V5 原始实测值，实际下发前乘 GAIT_AMPLITUDE_PCT。慢走和快跑
 * 共用这张表，只切换 80/40 ms 帧时长；后退按相位倒序播放。
 *
 * 这组数是在整机上逐帧调出来的，不要凭直觉改单个数值——四路之间有相位关系。
 */
static const uint16_t s_gait_forward[GAIT_WALK_FRAMES][SERVICE_V2_SERVO_COUNT] = {
    {1940U, 975U, 2060U, 1031U}, {2011U, 1003U, 2048U, 1051U}, {2060U, 1031U, 2025U, 1060U},
    {2048U, 1051U, 1997U, 989U}, {2025U, 1060U, 1969U, 940U},  {1997U, 989U, 1949U, 952U},
    {1969U, 940U, 1940U, 975U},  {1949U, 952U, 2011U, 1003U},
};

/*
 * 左右转是实机最终确认的“内侧两腿固定 HOME，外侧两腿交替踏步”。
 * 通道顺序 CH0..CH3 = FL,FR,RL,RR；每拍 150 ms。全部提速到 100 ms 或改成 80/150 ms
 * 混合节奏都会降低实际转角，因此不再继续压缩帧时长。
 */
static const uint16_t s_turn_left[GAIT_TURN_FRAMES][SERVICE_V2_SERVO_COUNT] = {
    {2100U, 1100U, 2050U, 950U},
    {2100U, 1100U, 1850U, 950U},
    {2100U, 900U, 1850U, 950U},
    {2100U, 900U, 2050U, 950U},
};

static const uint16_t s_turn_right[GAIT_TURN_FRAMES][SERVICE_V2_SERVO_COUNT] = {
    {1900U, 900U, 2050U, 950U},
    {1900U, 900U, 2050U, 1150U},
    {2100U, 900U, 2050U, 1150U},
    {2100U, 900U, 2050U, 950U},
};

static void gait_frame_at(uint32_t index, uint16_t out[SERVICE_V2_SERVO_COUNT])
{
    uint32_t i = index % GAIT_WALK_FRAMES;

    for (uint32_t ch = 0U; ch < SERVICE_V2_SERVO_COUNT; ++ch) {
        int32_t base = (int32_t)s_gait_center[ch];
        int32_t delta = (int32_t)s_gait_forward[i][ch] - (int32_t)s_gait_reference_stand[ch];
        /* 带符号四舍五入，别让整数除法把小摆幅一路截掉。 */
        int32_t scaled = (delta * (int32_t)GAIT_AMPLITUDE_PCT + (delta >= 0 ? 50 : -50)) / 100;

        out[ch] = (uint16_t)(base + scaled);
    }
}

typedef struct {
    service_v2_gait_action_t action;
    uint32_t cycles;
    uint8_t speed_percent;
    bool background;
    uint32_t generation;
} gait_cmd_t;

static uint32_t gait_duration_scaled(uint32_t duration_ms, uint8_t speed_percent)
{
    uint32_t speed = speed_percent;
    if (speed < 20U) speed = 20U;
    if (speed > 100U) speed = 100U;
    uint32_t scaled = (duration_ms * 50U + speed - 1U) / speed;
    scaled = ((scaled + GAIT_TICK_MS - 1U) / GAIT_TICK_MS) * GAIT_TICK_MS;
    return scaled < GAIT_TICK_MS ? GAIT_TICK_MS : scaled;
}

static bool s_ready;
static QueueHandle_t s_queue;
static lisa_mutex_t *s_lock;
static service_v2_gait_status_t s_status;
/*
 * s_pending_commands 在 xQueueSend 成功的同一个锁区间内增加；worker 从
 * FreeRTOS 队列取走命令后，只有在同一把锁内将 s_worker_active 置位后
 * 才会减少它。因此从队列取走到 busy 更新的窗口始终仍被视为非空闲。
 */
static uint32_t s_pending_commands;
static bool s_worker_active;
/* Owned exclusively by the gait worker, never by the audio callbacks. */
static bool s_background_command;
static uint32_t s_background_generation;
/* Worker-owned recovery, discarded as soon as another pose owner takes over. */
static bool s_background_home_pending;
static service_v2_servo_status_t s_background_home_snapshot;
/* Worker-owned: a paused recovery must retain the original motion's path. */
static service_v2_gait_action_t s_background_home_action;
static uint32_t s_idle_scratch_random_state;
static bool s_wake_resetting;
static bool s_wake_standing;
/* Boot-only application policy, not a persistent PWM/sleep interlock. */
static bool s_boot_pose_authorized;
static bool s_roll_reserved;
/* 右前蹬腿和网球发球共用正常面 HOME、安全门及独占语义。 */
static bool s_normal_face_motion_reserved;
/* 动作被 STOP/ALL_OFF 抢占后，独占权必须移交到所有恢复/卸力命令完成。 */
static uint32_t s_normal_face_motion_terminal_pending;
/* 只有动作入口的正常面 IMU 门通过后，被 STOP 抢占才允许套用正常 HOME。 */
static bool s_normal_face_motion_pose_validated;

static bool gait_safety_gates_enabled(void)
{
#if CONFIG_APP_V2_GAIT_SAFETY_GATES_ENABLE
    return true;
#else
    /* Backflip retains battery/USB protection even with legacy gates off.
     * Its start and landing no longer depend on IMU acceptance. */
    return s_status.current == SERVICE_V2_GAIT_ACTION_BACKFLIP;
#endif
}

static bool action_is_normal_face_motion(service_v2_gait_action_t action)
{
    return action == SERVICE_V2_GAIT_ACTION_IDLE_STRETCH ||
           action == SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH ||
           action == SERVICE_V2_GAIT_ACTION_MUSIC_BEAT ||
           action == SERVICE_V2_GAIT_ACTION_PUSHUP ||
           action == SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME ||
           action == SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK ||
           action == SERVICE_V2_GAIT_ACTION_TENNIS_SERVE ||
           action == SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND ||
           action == SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND ||
           action == SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND ||
           action == SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE ||
           action == SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE ||
           action == SERVICE_V2_GAIT_ACTION_GREETING ||
           action == SERVICE_V2_GAIT_ACTION_SCRATCH ||
           action == SERVICE_V2_GAIT_ACTION_SIT ||
           action == SERVICE_V2_GAIT_ACTION_KISS ||
           action == SERVICE_V2_GAIT_ACTION_DANCE ||
           action == SERVICE_V2_GAIT_ACTION_JUMP ||
           action == SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT ||
           action == SERVICE_V2_GAIT_ACTION_CODEX_SQUAT;
}

static void status_set(bool busy, service_v2_gait_action_t action, uint32_t cycles_done, int last_error)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    s_status.busy = busy;
    s_status.current = action;
    s_status.cycles_done = cycles_done;
    if (last_error != 0) {
        s_status.last_error = last_error;
    }
    lisa_mutex_unlock(s_lock);
}

static bool cmd_pending(void)
{
    if (s_background_command &&
        (s_background_generation != service_offline_action_generation() ||
         !service_v2_background_motion_allowed())) return true;
    if (__atomic_load_n(&s_wake_resetting, __ATOMIC_ACQUIRE)) return true;
    if (s_queue != NULL && uxQueueMessagesWaiting(s_queue) > 0U) {
        return true;
    }

    /* Only full rolls are unsafe with a Type-C cable attached.  Normal-face
     * tricks and the small upside wiggle are explicitly supported while a
     * serial/ADB cable is used for diagnostics. */
    return gait_safety_gates_enabled() && s_status.busy &&
           (s_status.current == SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP ||
            s_status.current == SERVICE_V2_GAIT_ACTION_BACKFLIP) &&
           power_is_usb_plugged();
}

static void command_begin(const gait_cmd_t *cmd)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    if (s_pending_commands > 0U) {
        s_pending_commands--;
    }
    s_worker_active = true;
    s_background_command = cmd->background;
    s_background_generation = cmd->generation;
    if (!cmd->background) s_background_home_pending = false;
    /* 让普通状态查询也不暴露 dequeue -> run_* 之间的假空闲。 */
    s_status.busy = true;
    s_status.current = cmd->action;
    s_status.cycles_done = 0U;
    s_status.last_error = 0;
    lisa_mutex_unlock(s_lock);
}

static int command_result(void)
{
    int result;

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    result = s_status.last_error;
    lisa_mutex_unlock(s_lock);
    return result;
}

static void command_end(void)
{
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    if (s_status.current == SERVICE_V2_GAIT_ACTION_STAND ||
        s_status.current == SERVICE_V2_GAIT_ACTION_ASSEMBLY) s_wake_standing = false;
    if (s_status.current == SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP ||
        s_status.current == SERVICE_V2_GAIT_ACTION_BACKFLIP ||
        s_status.current == SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE) {
        s_roll_reserved = false;
    }
    if (action_is_normal_face_motion(s_status.current)) {
        /* 已排队的 STOP/ALL_OFF 接管独占权，不在动作 worker 结束窗口提前释放。 */
        if (s_normal_face_motion_terminal_pending == 0U) {
            s_normal_face_motion_reserved = false;
            s_normal_face_motion_pose_validated = false;
        }
    } else if (s_normal_face_motion_reserved &&
               (s_status.current == SERVICE_V2_GAIT_ACTION_STOP ||
                s_status.current == SERVICE_V2_GAIT_ACTION_ALL_OFF)) {
        if (s_normal_face_motion_terminal_pending > 0U) {
            s_normal_face_motion_terminal_pending--;
        }
        if (s_normal_face_motion_terminal_pending == 0U) {
            s_normal_face_motion_reserved = false;
            s_normal_face_motion_pose_validated = false;
        }
    }
    s_worker_active = false;
    s_background_command = false;
    lisa_mutex_unlock(s_lock);
}

/**
 * 下发一帧并等它跑完。
 *
 * 等待期间做两件事：按 20 ms 踢一次舵机租约，以及检查有没有新命令。
 *
 * 踢租约是必须的。看门狗是全局的：Motion Studio 解锁时会武装它，一旦超时固件
 * 直接切断四路 PWM。语音触发的步态跟主机心跳没有关系，自己不踢就会在半路被
 * 咬掉。watchdog_ms 为 0（也就是没人武装）时 kick 是空操作，无条件踢是安全的。
 *
 * @return 0 正常跑完，-ECANCELED 被新命令抢占，其它为帧下发失败。
 */
static int play_frame(const uint16_t pulse_us[SERVICE_V2_SERVO_COUNT], uint32_t duration_ms,
                      service_v2_servo_easing_t easing)
{
    if (s_background_command && cmd_pending()) return -ECANCELED;
    int ret = service_v2_actuators_servo_frame(pulse_us, duration_ms, easing);

    if (ret != 0) {
        return ret;
    }

    for (uint32_t waited = 0U; waited < duration_ms; waited += GAIT_TICK_MS) {
        uint32_t remaining = duration_ms - waited;
        lisa_thread_mdelay(remaining < GAIT_TICK_MS ? remaining : GAIT_TICK_MS);
        (void)service_v2_actuators_servo_watchdog_kick();

        /*
         * 抢占不需要额外收尾：下一帧起帧时固件以当前脉宽为起点重新插值，
         * 从半路切过去是连续的，不会有跳变。
         */
        if (cmd_pending()) {
            return -ECANCELED;
        }
    }

    return 0;
}

/*
 * play_frame() 的等待时长与插值器相同，但两个 LOW 优先级线程在最后一个 tick
 * 同时唤醒时，gait 线程可能先返回。新触摸动作在每一拍后显式回读完成态，
 * 防止尚差最后一步就被下一张反向帧接管。
 */
static int wait_frame_target(const uint16_t target_us[SERVICE_V2_SERVO_COUNT])
{
    service_v2_servo_status_t servo = {0};

    for (uint32_t waited = 0U; waited <= GAIT_FRAME_SETTLE_TIMEOUT_MS; waited += GAIT_TICK_MS) {
        if (cmd_pending()) {
            return -ECANCELED;
        }

        int ret = service_v2_actuators_servo_get_status(&servo);
        if (ret != 0) {
            return ret;
        }
        if (servo.watchdog_expired || servo.enabled_mask != SERVICE_V2_SERVO_ALL_MASK) {
            return -EIO;
        }

        bool at_target = !servo.frame_active;
        for (uint32_t ch = 0U; at_target && ch < SERVICE_V2_SERVO_COUNT; ++ch) {
            at_target = servo.current_us[ch] == target_us[ch] && servo.target_us[ch] == target_us[ch];
        }
        if (at_target) {
            return 0;
        }
        if (waited == GAIT_FRAME_SETTLE_TIMEOUT_MS) {
            break;
        }

        lisa_thread_mdelay(GAIT_TICK_MS);
        (void)service_v2_actuators_servo_watchdog_kick();
    }

    LISA_LOGE(TAG, "servo frame did not settle: current=%u,%u,%u,%u target=%u,%u,%u,%u active=%u",
              (unsigned)servo.current_us[0], (unsigned)servo.current_us[1], (unsigned)servo.current_us[2],
              (unsigned)servo.current_us[3], (unsigned)servo.target_us[0], (unsigned)servo.target_us[1],
              (unsigned)servo.target_us[2], (unsigned)servo.target_us[3], servo.frame_active ? 1U : 0U);
    return -ETIMEDOUT;
}

static int hold_pose(uint32_t duration_ms);

/*
 * 插值器的“到点”只代表命令脉宽已到目标，不代表承重中的实体舵机已经
 * 越过静摩擦和机械死点。原始上位机实机成功命令在每帧后都额外保压约
 * 100 ms；板端必须保留这一物理稳定时间，不能用命令态回读代替。
 */
static int play_frame_settled(const uint16_t pulse_us[SERVICE_V2_SERVO_COUNT], uint32_t duration_ms,
                              service_v2_servo_easing_t easing)
{
    int ret = play_frame(pulse_us, duration_ms, easing);

    if (ret == 0) {
        ret = wait_frame_target(pulse_us);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_SERVO_PHYSICAL_SETTLE_MS);
    }

    return ret;
}

/*
 * 右前蹬腿和网球发球结束时，不能把四路同时从
 * [2100,1800,2200,750] 拉回 HOME。标准 PWM 没有位置反馈，承重时后腿会
 * 跟不上命令并让实物停在约 90 度坐姿。2026-09-02 上位机实机成功的恢复顺序是：
 * 先保持后腿支撑并用 500 ms 收回前腿，到位后保压 100 ms；再用 500 ms
 * 单独将后腿快速回到 HOME，到位后总计保压 200 ms。2026-09-02 插线上位机
 * 已按“后腿预收 -> 500 ms 回 HOME”验证指令链；最终站立效果需在拔掉 Type-C
 * 后由触摸动作复验，以排除线材支撑力和 USB 供电对姿态的影响。
 */
static int recover_normal_face_home_two_stage(void)
{
    int ret = play_frame_settled(s_right_front_kick_rear_preload,
                                 GAIT_RIGHT_FRONT_ACTION_FRONT_HOME_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);

    if (ret == 0) {
        ret = play_frame_settled(s_pose_stand, GAIT_RIGHT_FRONT_ACTION_REAR_HOME_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        /* play_frame_settled() 已保压 100 ms；末帧再补 100 ms 对齐实机成功节拍。 */
        ret = hold_pose(GAIT_RIGHT_FRONT_ACTION_FINAL_HOLD_MS);
    }

    return ret;
}

/* 在两张关键帧之间原位保压，不再向插值器提交一张假的“同点帧”。 */
static int hold_pose(uint32_t duration_ms)
{
    for (uint32_t waited = 0U; waited < duration_ms; waited += GAIT_TICK_MS) {
        uint32_t remaining = duration_ms - waited;
        lisa_thread_mdelay(remaining < GAIT_TICK_MS ? remaining : GAIT_TICK_MS);
        (void)service_v2_actuators_servo_watchdog_kick();

        if (cmd_pending()) {
            return -ECANCELED;
        }
    }

    return 0;
}

/* 发球收拍需要右前腿有爆发感，但承重后腿仍按实机验证过的节奏回位。 */
static int recover_tennis_serve_home(void)
{
    int ret = play_frame_settled(s_tennis_rear_preload,
                                 GAIT_TENNIS_FRONT_HOME_MS,
                                 SERVICE_V2_SERVO_EASING_LINEAR);
    if (ret == 0) {
        ret = play_frame_settled(s_tennis_rear_kick, GAIT_TENNIS_REAR_KICK_MS,
                                 SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_pose_stand, GAIT_TENNIS_HOME_SETTLE_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_RIGHT_FRONT_ACTION_FINAL_HOLD_MS);
    }
    return ret;
}

/*
 * 舞蹈结束后的独立强制回位。第一拍前腿已经在 HOME，仅抬起后躯恢复承重；
 * 第二拍四脚回 HOME，并由 play_frame_settled() 继续保压，避免最后一帧命令
 * 已到但实体舵机仍停在屁股着地的位置。
 */
static int recover_dance_home_two_stage(void)
{
    int ret = play_frame_settled(s_dance_recover_support,
                                 GAIT_DANCE_RECOVER_SUPPORT_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);

    if (ret == 0) {
        ret = play_frame_settled(s_pose_stand, GAIT_DANCE_RECOVER_HOME_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }

    return ret;
}

static bool pulse_near(uint16_t actual_us, uint16_t expected_us)
{
    uint16_t delta_us = actual_us > expected_us ? actual_us - expected_us : expected_us - actual_us;

    return delta_us <= GAIT_HOME_TOLERANCE_US;
}

static bool servo_status_is_pristine(const service_v2_servo_status_t *servo)
{
    if (servo->enabled_mask != 0U || servo->sequence != 0U || servo->frame_active ||
        servo->sweep_active || servo->watchdog_ms != 0U || servo->watchdog_expired) {
        return false;
    }

    for (uint32_t ch = 0U; ch < SERVICE_V2_SERVO_COUNT; ++ch) {
        if (servo->current_us[ch] != SERVICE_V2_SERVO_CENTER_PULSE_US ||
            servo->target_us[ch] != SERVICE_V2_SERVO_CENTER_PULSE_US) {
            return false;
        }
    }
    return true;
}

/*
 * PWM 全关时不能直接用有时长的帧回 HOME：驱动内部的冷启动命令值是
 * 1500 us，那样会先在错误角度使能再扫到标定值。先在关闭态逐路预装，
 * 再用 0 ms 四路帧原子使能，供触摸动作和 STOP 共用。
 */
static int enable_normal_home_from_disabled(uint32_t hold_ms)
{
    int ret = service_v2_actuators_servo_disable_all();

    for (uint32_t ch = 0U; ret == 0 && ch < SERVICE_V2_SERVO_COUNT; ++ch) {
        ret = service_v2_actuators_servo_set_us((uint8_t)ch, s_pose_stand[ch]);
    }
    if (ret == 0 && cmd_pending()) {
        /* 预装期间到达的 STOP/ALL_OFF/Type-C 必须在使能 PWM 前截住。 */
        ret = -ECANCELED;
    }
    if (ret == 0) {
        ret = service_v2_actuators_servo_frame(s_pose_stand, 0U, SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0 && hold_ms > 0U) {
        ret = hold_pose(hold_ms);
    }
    return ret;
}

/*
 * 右前蹬腿和网球发球只允许从正常面 HOME 起步。冷启动 PWM 全关时，先在
 * 关闭状态预装 HOME 再原子使能，避免第一帧错误地从内部默认 1500 us 扫过去。
 */
static int prepare_normal_face_motion_home(void)
{
    service_v2_servo_status_t servo = {0};
    int ret = cmd_pending() ? -ECANCELED : service_v2_actuators_servo_get_status(&servo);

    if (ret != 0) {
        return ret;
    }
    if (servo.frame_active || servo.sweep_active || servo.watchdog_ms != 0U || servo.watchdog_expired) {
        return -EBUSY;
    }
    bool require_held_home = s_background_command &&
                             s_status.current == SERVICE_V2_GAIT_ACTION_CODEX_SQUAT;
    if (servo.enabled_mask == 0U) {
        bool disabled_at_home = true;
        for (uint32_t ch = 0U; ch < SERVICE_V2_SERVO_COUNT; ++ch) {
            if (!pulse_near(servo.current_us[ch], s_pose_stand[ch]) ||
                !pulse_near(servo.target_us[ch], s_pose_stand[ch])) {
                disabled_at_home = false;
                break;
            }
        }
        if (require_held_home && (!servo.usb_idle_released || !disabled_at_home)) return -EACCES;
        if (!servo_status_is_pristine(&servo) && !disabled_at_home) {
            /* 中途 fail-off 会保留非 HOME 脉宽；下一次触摸不得 0 ms 瞬拉 HOME。 */
            return -EAGAIN;
        }
        return enable_normal_home_from_disabled(GAIT_RIGHT_FRONT_KICK_COLD_HOLD_MS);
    }
    if (servo.enabled_mask != SERVICE_V2_SERVO_ALL_MASK) {
        return -EACCES;
    }

    bool at_home = true;
    for (uint32_t ch = 0U; ch < SERVICE_V2_SERVO_COUNT; ++ch) {
        if (!pulse_near(servo.current_us[ch], s_pose_stand[ch]) ||
            !pulse_near(servo.target_us[ch], s_pose_stand[ch])) {
            at_home = false;
            break;
        }
    }
    if (require_held_home && !at_home) return -EAGAIN;
    /* A completed pose such as SIT may deliberately hold away from HOME.
     * The IMU normal-face gate has already passed, so return it smoothly to
     * HOME before starting another large action. Disabled/fail-off poses are
     * still rejected by the branch above. */
    return at_home ? 0 : play_frame_settled(s_pose_stand, GAIT_HOME_RETURN_MS,
                                            SERVICE_V2_SERVO_EASING_EASE_OUT);
}

static uint64_t square_i32(int32_t value)
{
    int64_t wide = value;
    return (uint64_t)(wide * wide);
}

static int32_t abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

static bool flip_pose_vector_is_safe(int32_t accel_x, int32_t accel_y, int32_t accel_z, bool reverse_face)
{
    uint64_t x_squared = square_i32(accel_x);
    uint64_t yz_squared = square_i32(accel_y) + square_i32(accel_z);
    uint64_t xz_squared = x_squared + square_i32(accel_z);
    uint64_t norm_squared = x_squared + yz_squared;
    uint64_t norm_min_squared = (uint64_t)GAIT_FLIP_ACCEL_NORM_MIN * GAIT_FLIP_ACCEL_NORM_MIN;
    uint64_t norm_max_squared = (uint64_t)GAIT_FLIP_ACCEL_NORM_MAX * GAIT_FLIP_ACCEL_NORM_MAX;
    uint64_t pitch_lhs = x_squared * 1000000ULL;
    uint64_t pitch_rhs = yz_squared * GAIT_FLIP_TAN_20_X1000 * GAIT_FLIP_TAN_20_X1000;
    uint64_t roll_lhs = square_i32(accel_y) * 1000000ULL;
    uint64_t roll_rhs = xz_squared * GAIT_FLIP_TAN_20_X1000 * GAIT_FLIP_TAN_20_X1000;

    bool z_is_expected = reverse_face ? accel_z >= GAIT_FLIP_FACE_REVERSE_Z_MIN
                                      : accel_z <= GAIT_FLIP_FACE_UP_Z_MAX;

    return z_is_expected && norm_squared >= norm_min_squared &&
           norm_squared <= norm_max_squared && pitch_lhs <= pitch_rhs && roll_lhs <= roll_rhs;
}

/*
 * 起翻是唯一会大幅超出走行区间的动作，安全门必须在第一张 HOME PWM 帧
 * 之前完成。取 5 个原始样本再用均值判断，避免单点 I2C 噪声或轻微振动误触发。
 */
static int flip_pose_check(bool reverse_face, const char *phase)
{
    if (!gait_safety_gates_enabled()) {
        return 0;
    }

    int ret = service_v2_mpu6050_init(false);
    int64_t accel_x_sum = 0;
    int64_t accel_y_sum = 0;
    int64_t accel_z_sum = 0;
    int32_t accel_x_min = 0;
    int32_t accel_x_max = 0;
    int32_t accel_y_min = 0;
    int32_t accel_y_max = 0;
    int32_t accel_z_min = 0;
    int32_t accel_z_max = 0;

    if (ret != 0) {
        LISA_LOGE(TAG, "%s MPU6050 init failed: %d", phase, ret);
        return ret;
    }

    for (uint32_t i = 0U; i < GAIT_FLIP_IMU_SAMPLE_COUNT; ++i) {
        service_v2_mpu6050_raw_t sample = {0};

        if (cmd_pending()) {
            return -ECANCELED;
        }

        ret = service_v2_mpu6050_read_raw(&sample);
        if (ret != 0) {
            LISA_LOGE(TAG, "%s MPU6050 sample %u failed: %d", phase, (unsigned)i, ret);
            return ret;
        }

        if (!flip_pose_vector_is_safe(sample.accel_x, sample.accel_y, sample.accel_z, reverse_face)) {
            LISA_LOGW(TAG, "%s rejected by sample %u: accel=%d,%d,%d", phase, (unsigned)i,
                      (int)sample.accel_x, (int)sample.accel_y, (int)sample.accel_z);
            return -EPERM;
        }

        if (abs_i32(sample.gyro_x) > GAIT_FLIP_GYRO_ABS_MAX || abs_i32(sample.gyro_y) > GAIT_FLIP_GYRO_ABS_MAX ||
            abs_i32(sample.gyro_z) > GAIT_FLIP_GYRO_ABS_MAX) {
            LISA_LOGW(TAG, "%s rejected by gyro sample %u: gyro=%d,%d,%d", phase, (unsigned)i,
                      (int)sample.gyro_x, (int)sample.gyro_y, (int)sample.gyro_z);
            return -EPERM;
        }

        if (i == 0U) {
            accel_x_min = accel_x_max = sample.accel_x;
            accel_y_min = accel_y_max = sample.accel_y;
            accel_z_min = accel_z_max = sample.accel_z;
        } else {
            if (sample.accel_x < accel_x_min) {
                accel_x_min = sample.accel_x;
            }
            if (sample.accel_x > accel_x_max) {
                accel_x_max = sample.accel_x;
            }
            if (sample.accel_y < accel_y_min) {
                accel_y_min = sample.accel_y;
            }
            if (sample.accel_y > accel_y_max) {
                accel_y_max = sample.accel_y;
            }
            if (sample.accel_z < accel_z_min) {
                accel_z_min = sample.accel_z;
            }
            if (sample.accel_z > accel_z_max) {
                accel_z_max = sample.accel_z;
            }
        }

        accel_x_sum += sample.accel_x;
        accel_y_sum += sample.accel_y;
        accel_z_sum += sample.accel_z;

        if (i + 1U < GAIT_FLIP_IMU_SAMPLE_COUNT) {
            lisa_thread_mdelay(GAIT_FLIP_IMU_SAMPLE_INTERVAL_MS);
        }
    }

    int32_t accel_x = (int32_t)(accel_x_sum / (int64_t)GAIT_FLIP_IMU_SAMPLE_COUNT);
    int32_t accel_y = (int32_t)(accel_y_sum / (int64_t)GAIT_FLIP_IMU_SAMPLE_COUNT);
    int32_t accel_z = (int32_t)(accel_z_sum / (int64_t)GAIT_FLIP_IMU_SAMPLE_COUNT);

    if (accel_x_max - accel_x_min > GAIT_FLIP_ACCEL_AXIS_SPREAD_MAX ||
        accel_y_max - accel_y_min > GAIT_FLIP_ACCEL_AXIS_SPREAD_MAX ||
        accel_z_max - accel_z_min > GAIT_FLIP_ACCEL_AXIS_SPREAD_MAX) {
        LISA_LOGW(TAG, "%s rejected by accel stability: spread=%d,%d,%d", phase,
                  (int)(accel_x_max - accel_x_min), (int)(accel_y_max - accel_y_min),
                  (int)(accel_z_max - accel_z_min));
        return -EPERM;
    }

    if (!flip_pose_vector_is_safe(accel_x, accel_y, accel_z, reverse_face)) {
        LISA_LOGW(TAG, "%s rejected by pose gate: accel=%d,%d,%d", phase, (int)accel_x, (int)accel_y,
                  (int)accel_z);
        return -EPERM;
    }

    LISA_LOGI(TAG, "%s pose gate passed: accel=%d,%d,%d", phase, (int)accel_x, (int)accel_y, (int)accel_z);
    return 0;
}

typedef enum {
    ROLL_FACE_NORMAL = 0,
    ROLL_FACE_REVERSE,
} roll_face_t;

static const char *roll_face_name(roll_face_t face)
{
    return face == ROLL_FACE_REVERSE ? "reverse" : "normal";
}

/* Conservative gates may be disabled for this small toy, but upside-down
 * recovery still needs a broad face decision. Average three samples and use
 * gravity-Z only, so hand motion or gyro settling cannot reject the command. */
static int detect_roll_start_face_relaxed(roll_face_t *face)
{
    int ret;
    int32_t accel_z_sum = 0;

    if (face == NULL) {
        return -EINVAL;
    }

    ret = service_v2_mpu6050_init(false);
    if (ret != 0) {
        return ret;
    }

    for (uint32_t i = 0U; i < 3U; ++i) {
        service_v2_mpu6050_raw_t sample = {0};

        if (cmd_pending()) {
            return -ECANCELED;
        }
        ret = service_v2_mpu6050_read_raw(&sample);
        if (ret != 0) {
            return ret;
        }
        accel_z_sum += sample.accel_z;
        if (i + 1U < 3U) {
            lisa_thread_mdelay(GAIT_FLIP_IMU_SAMPLE_INTERVAL_MS);
        }
    }

    int32_t accel_z = accel_z_sum / 3;
    *face = service_v2_gait_accel_z_is_upside_down(accel_z) ? ROLL_FACE_REVERSE : ROLL_FACE_NORMAL;
    LISA_LOGI(TAG, "relaxed face detection=%s accel_z=%d",
              roll_face_name(*face), (int)accel_z);
    return 0;
}

/*
 * 自动翻滚只用一次 5 帧窗口判面。每一帧都必须稳定属于同一面，避免先按
 * normal 采一次、再按 reverse 采一次时，用户恰好在两个窗口之间搬动机身。
 */
static int detect_roll_start_face(roll_face_t *face)
{
    if (face == NULL) {
        return -EINVAL;
    }

    if (!gait_safety_gates_enabled()) {
        return detect_roll_start_face_relaxed(face);
    }

    int ret = service_v2_mpu6050_init(false);
    int detected_face = -1;
    int64_t accel_x_sum = 0;
    int64_t accel_y_sum = 0;
    int64_t accel_z_sum = 0;
    int32_t accel_x_min = 0;
    int32_t accel_x_max = 0;
    int32_t accel_y_min = 0;
    int32_t accel_y_max = 0;
    int32_t accel_z_min = 0;
    int32_t accel_z_max = 0;

    if (ret != 0) {
        LISA_LOGE(TAG, "roll start MPU6050 init failed: %d", ret);
        return ret;
    }

    for (uint32_t i = 0U; i < GAIT_FLIP_IMU_SAMPLE_COUNT; ++i) {
        service_v2_mpu6050_raw_t sample = {0};

        if (cmd_pending()) {
            return -ECANCELED;
        }

        ret = service_v2_mpu6050_read_raw(&sample);
        if (ret != 0) {
            LISA_LOGE(TAG, "roll start MPU6050 sample %u failed: %d", (unsigned)i, ret);
            return ret;
        }

        bool normal = flip_pose_vector_is_safe(sample.accel_x, sample.accel_y, sample.accel_z, false);
        bool reverse = flip_pose_vector_is_safe(sample.accel_x, sample.accel_y, sample.accel_z, true);

        if (normal == reverse) {
            LISA_LOGW(TAG, "roll start face ambiguous at sample %u: accel=%d,%d,%d", (unsigned)i,
                      (int)sample.accel_x, (int)sample.accel_y, (int)sample.accel_z);
            return -EPERM;
        }

        int sample_face = reverse ? (int)ROLL_FACE_REVERSE : (int)ROLL_FACE_NORMAL;
        if (detected_face < 0) {
            detected_face = sample_face;
        } else if (sample_face != detected_face) {
            LISA_LOGW(TAG, "roll start face changed inside sample window");
            return -EPERM;
        }

        if (abs_i32(sample.gyro_x) > GAIT_FLIP_GYRO_ABS_MAX ||
            abs_i32(sample.gyro_y) > GAIT_FLIP_GYRO_ABS_MAX ||
            abs_i32(sample.gyro_z) > GAIT_FLIP_GYRO_ABS_MAX) {
            LISA_LOGW(TAG, "roll start rejected by gyro sample %u: gyro=%d,%d,%d", (unsigned)i,
                      (int)sample.gyro_x, (int)sample.gyro_y, (int)sample.gyro_z);
            return -EPERM;
        }

        if (i == 0U) {
            accel_x_min = accel_x_max = sample.accel_x;
            accel_y_min = accel_y_max = sample.accel_y;
            accel_z_min = accel_z_max = sample.accel_z;
        } else {
            if (sample.accel_x < accel_x_min) {
                accel_x_min = sample.accel_x;
            }
            if (sample.accel_x > accel_x_max) {
                accel_x_max = sample.accel_x;
            }
            if (sample.accel_y < accel_y_min) {
                accel_y_min = sample.accel_y;
            }
            if (sample.accel_y > accel_y_max) {
                accel_y_max = sample.accel_y;
            }
            if (sample.accel_z < accel_z_min) {
                accel_z_min = sample.accel_z;
            }
            if (sample.accel_z > accel_z_max) {
                accel_z_max = sample.accel_z;
            }
        }

        accel_x_sum += sample.accel_x;
        accel_y_sum += sample.accel_y;
        accel_z_sum += sample.accel_z;

        if (i + 1U < GAIT_FLIP_IMU_SAMPLE_COUNT) {
            lisa_thread_mdelay(GAIT_FLIP_IMU_SAMPLE_INTERVAL_MS);
        }
    }

    int32_t accel_x = (int32_t)(accel_x_sum / (int64_t)GAIT_FLIP_IMU_SAMPLE_COUNT);
    int32_t accel_y = (int32_t)(accel_y_sum / (int64_t)GAIT_FLIP_IMU_SAMPLE_COUNT);
    int32_t accel_z = (int32_t)(accel_z_sum / (int64_t)GAIT_FLIP_IMU_SAMPLE_COUNT);

    if (accel_x_max - accel_x_min > GAIT_FLIP_ACCEL_AXIS_SPREAD_MAX ||
        accel_y_max - accel_y_min > GAIT_FLIP_ACCEL_AXIS_SPREAD_MAX ||
        accel_z_max - accel_z_min > GAIT_FLIP_ACCEL_AXIS_SPREAD_MAX) {
        LISA_LOGW(TAG, "roll start rejected by accel stability: spread=%d,%d,%d",
                  (int)(accel_x_max - accel_x_min), (int)(accel_y_max - accel_y_min),
                  (int)(accel_z_max - accel_z_min));
        return -EPERM;
    }

    bool reverse = detected_face == (int)ROLL_FACE_REVERSE;
    if (!flip_pose_vector_is_safe(accel_x, accel_y, accel_z, reverse)) {
        LISA_LOGW(TAG, "roll start rejected by averaged pose: accel=%d,%d,%d", (int)accel_x,
                  (int)accel_y, (int)accel_z);
        return -EPERM;
    }

    *face = reverse ? ROLL_FACE_REVERSE : ROLL_FACE_NORMAL;
    LISA_LOGI(TAG, "roll start detected %s face: accel=%d,%d,%d", roll_face_name(*face),
              (int)accel_x, (int)accel_y, (int)accel_z);
    return 0;
}

static int motion_battery_check(const char *phase)
{
    if (!gait_safety_gates_enabled()) {
        return 0;
    }

    uint16_t voltage_mv = battery_get_voltage_mv();
    if (voltage_mv < GAIT_ROLL_MIN_BATTERY_MV) {
        LISA_LOGW(TAG, "%s blocked by battery=%umV minimum=%umV", phase, (unsigned)voltage_mv,
                  (unsigned)GAIT_ROLL_MIN_BATTERY_MV);
        return -EAGAIN;
    }

    return 0;
}

static int roll_power_check(const char *phase)
{
    if (!gait_safety_gates_enabled()) {
        return 0;
    }

    if (power_is_usb_plugged()) {
        LISA_LOGW(TAG, "%s blocked while Type-C is connected", phase);
        return -EBUSY;
    }

    return motion_battery_check(phase);
}

/*
 * 正常面动作允许在 Type-C/ADB 调试时运行；只有翻滚链路继续由
 * roll_power_check() 禁止插线执行。电池、静止姿态、正常面 HOME 仍缺一不可。
 * battery gate 通过后即武装 fail-off；因此后续任何失败都由调用者关闭四路 PWM。
 */
static int prepare_normal_face_motion(const char *phase, bool *motion_started)
{
    /* Background movement keeps the touch pose/battery guards even when the
     * diagnostic switch disables the heavier foreground gait checks. */
    if (s_background_command) {
        service_v2_imu_interaction_status_t imu = {0};
        if (service_v2_imu_interaction_get_status(&imu) != 0 ||
            !imu.ready || imu.pose != SERVICE_V2_IMU_POSE_NORMAL) return -EPERM;
        if (battery_get_voltage_mv() < GAIT_ROLL_MIN_BATTERY_MV) return -EAGAIN;
    }
    int ret = motion_battery_check(phase);

    if (ret == 0) {
        *motion_started = true;
        ret = flip_pose_check(false, phase);
    }
    if (ret == 0) {
        /* 与 submit_command() 用同一把锁发布“已判正面”；锁内同时复查
         * pending，使 STOP 要么保守映射 ALL_OFF，要么在授权 HOME 后入队。 */
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        bool interrupted = s_pending_commands != 0U || uxQueueMessagesWaiting(s_queue) != 0U;
        if (!interrupted) {
            s_normal_face_motion_pose_validated = true;
        }
        lisa_mutex_unlock(s_lock);
        if (interrupted) {
            ret = -ECANCELED;
        }
    }
    if (ret == 0 && s_status.current != SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME) {
        ret = prepare_normal_face_motion_home();
    }
    if (ret == 0) {
        service_v2_gait_action_t started = s_status.current;
        if (started == SERVICE_V2_GAIT_ACTION_DANCE)
            service_v2_rgb_effect_show_start(SERVICE_V2_RGB_SHOW_DANCE);
        /* Background motion must not refresh standby or acquire a cloud audio hold. */
        if (!s_background_command)
            (void)voice_msg_pub(VOICE_MSG_V2_GAIT_ACTION_STARTED, &started, sizeof(started));
        LISA_LOGI(TAG, "motion started after safety checks: %s",
                  service_v2_gait_action_name(started));
    }

    return ret;
}

static void run_flip_rise(void)
{
    status_set(true, SERVICE_V2_GAIT_ACTION_FLIP_RISE, 0U, 0);

    int ret = flip_pose_check(false, "flip-rise");

    if (ret == 0) {
        ret = play_frame(s_pose_stand, GAIT_FLIP_HOME_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_FLIP_AFTER_HOME_HOLD_MS);
    }
    if (ret == 0) {
        ret = play_frame(s_flip_front_retract, GAIT_FLIP_FRONT_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_FLIP_AFTER_FRONT_HOLD_MS);
    }
    if (ret == 0) {
        ret = play_frame(s_flip_rear_load, GAIT_FLIP_REAR_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_FLIP_AFTER_REAR_HOLD_MS);
    }
    if (ret == 0) {
        ret = play_frame(s_flip_front_hook, GAIT_FLIP_HOOK_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }

    if (ret == 0) {
        /* 最后一帧必须继续保持扭矩，不回 HOME，也不卸 PWM。 */
        LISA_LOGI(TAG, "flip-rise complete in %ums, holding final pose",
                  (unsigned)SERVICE_V2_GAIT_FLIP_RISE_DURATION_MS);
    } else if (ret != -ECANCELED) {
        LISA_LOGE(TAG, "flip-rise failed: %d", ret);
    }

    status_set(false, SERVICE_V2_GAIT_ACTION_FLIP_RISE, 0U, ret == -ECANCELED ? 0 : ret);
}

/*
 * 普通翻滚等待 IMU 确认目标面：连续 5 帧满足对应姿态条件才放行。
 * 后空翻按产品要求免除 IMU 门，改为可取消的短暂定时保压。
 */
static int wait_for_roll_landing(bool reverse_face, const char *phase)
{
    if (s_status.current == SERVICE_V2_GAIT_ACTION_BACKFLIP) {
        /* Keep a short settling interval without an IMU gate. hold_pose()
         * still services the watchdog and honours STOP/wake/USB cancellation. */
        return hold_pose(GAIT_SERVO_PHYSICAL_SETTLE_MS);
    }

    int ret = service_v2_mpu6050_init(false);
    uint32_t stable = 0U;
    service_v2_mpu6050_raw_t last = {0};

    if (ret != 0) {
        LISA_LOGE(TAG, "%s MPU6050 init failed: %d", phase, ret);
        return ret;
    }

    for (uint32_t waited = 0U; waited < GAIT_ROLL_LANDING_TIMEOUT_MS;
         waited += GAIT_ROLL_LANDING_SAMPLE_MS) {
        if (cmd_pending()) {
            return -ECANCELED;
        }

        ret = service_v2_mpu6050_read_raw(&last);
        if (ret != 0) {
            LISA_LOGE(TAG, "%s MPU6050 read failed: %d", phase, ret);
            return ret;
        }

        bool gyro_still = abs_i32(last.gyro_x) <= GAIT_FLIP_GYRO_ABS_MAX &&
                          abs_i32(last.gyro_y) <= GAIT_FLIP_GYRO_ABS_MAX &&
                          abs_i32(last.gyro_z) <= GAIT_FLIP_GYRO_ABS_MAX;
        bool face_matches = gait_safety_gates_enabled()
                                ? flip_pose_vector_is_safe(last.accel_x, last.accel_y,
                                                           last.accel_z, reverse_face)
                                : (reverse_face ? last.accel_z >= 0 : last.accel_z < 0);
        if (face_matches && (!gait_safety_gates_enabled() || gyro_still)) {
            stable++;
            if (stable >= GAIT_FLIP_IMU_SAMPLE_COUNT) {
                LISA_LOGI(TAG, "%s landing confirmed: accel=%d,%d,%d", phase, (int)last.accel_x,
                          (int)last.accel_y, (int)last.accel_z);
                return 0;
            }
        } else {
            stable = 0U;
        }

        (void)service_v2_actuators_servo_watchdog_kick();
        lisa_thread_mdelay(GAIT_ROLL_LANDING_SAMPLE_MS);
    }

    LISA_LOGE(TAG, "%s landing timeout: accel=%d,%d,%d gyro=%d,%d,%d", phase, (int)last.accel_x,
              (int)last.accel_y, (int)last.accel_z, (int)last.gyro_x, (int)last.gyro_y,
              (int)last.gyro_z);
    return -ETIMEDOUT;
}

/*
 * 重启后舵机命令态是 1500 us、PWM 关闭。先按 IMU 判出的面同步对应 HOME，
 * 再原子使能同一姿态，避免第一帧错误经过 1500 us。
 */
static int sync_roll_home_from_disabled(roll_face_t face)
{
    const uint16_t *pose = face == ROLL_FACE_REVERSE ? s_pose_reverse_home : s_pose_stand;
    int ret = service_v2_actuators_servo_disable_all();

    for (uint32_t ch = 0U; ret == 0 && ch < SERVICE_V2_SERVO_COUNT; ++ch) {
        ret = service_v2_actuators_servo_set_us((uint8_t)ch, pose[ch]);
    }
    if (ret == 0) {
        ret = service_v2_actuators_servo_frame(pose, 0U, SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_ROLL_START_HOLD_MS);
    }
    return ret;
}

/* Spatial front/rear reflection, NOT temporal reversal. V2.1 pairs are
 * FL<->RL (0<->3), FR<->RR (1<->2), with opposite pulse polarity. HOME stays
 * unchanged. This is a candidate backward trajectory, not a measured flip. */
static int backflip_frame(const uint16_t *source, uint16_t *target)
{
    for (uint32_t ch = 0U; ch < SERVICE_V2_SERVO_COUNT; ++ch) {
        uint32_t peer = SERVICE_V2_SERVO_COUNT - 1U - ch;
        int32_t pulse = (int32_t)s_pose_stand[ch] -
                        ((int32_t)source[peer] - (int32_t)s_pose_stand[peer]);
        if (pulse < 650 || pulse > 2350) return -ERANGE;
        target[ch] = (uint16_t)pulse;
    }
    return 0;
}

static int play_roll_frame(const uint16_t *pulse_us, uint32_t duration_ms,
                           service_v2_servo_easing_t easing)
{
    if (cmd_pending()) return -ECANCELED;
    if (s_status.current == SERVICE_V2_GAIT_ACTION_BACKFLIP) {
        uint16_t mirrored[SERVICE_V2_SERVO_COUNT];
        int ret = backflip_frame(pulse_us, mirrored);
        if (ret != 0) return ret;
        return play_frame(mirrored, duration_ms, easing);
    }
    return play_frame(pulse_us, duration_ms, easing);
}

/* Existing forward second half is field-tested; mirrored backward use still
 * requires floor validation. */
static int run_reverse_half_roll(void)
{
    int ret = play_roll_frame(s_roll_reverse_front_retract, GAIT_ROLL_REVERSE_FRONT_RETRACT_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) {
        ret = play_roll_frame(s_roll_reverse_rear_stage1, GAIT_ROLL_REVERSE_REAR_STAGE_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_roll_reverse_rear_load, GAIT_ROLL_REVERSE_REAR_STAGE_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_roll_reverse_front_hook, GAIT_ROLL_REVERSE_HOOK_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_roll_reverse_rear_home, GAIT_ROLL_REAR_RETURN_MS,
                         SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_roll_reverse_cross, GAIT_ROLL_CROSS_MS, SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        ret = wait_for_roll_landing(false, "reverse-half -> normal");
    }
    if (ret == 0) {
        /* 当前世界前腿 CH0/CH1 先落地，再放当前世界后腿 CH2/CH3。 */
        ret = play_roll_frame(s_roll_recover_normal_front, GAIT_ROLL_RECOVER_PAIR_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_pose_stand, GAIT_ROLL_RECOVER_PAIR_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        /* 恢复腿位会再次激起壳体余振；等待连续稳定样本，不能因首帧振动误断电。 */
        ret = wait_for_roll_landing(false, "normal HOME settled after reverse-half");
    }
    return ret;
}

/* 正常面 180°：三拍起身、六轮蹬腿、越点；仅接下一半圈时恢复反面 HOME。 */
static int run_normal_half_roll(bool prepare_reverse_home)
{
    int ret = play_roll_frame(s_pose_stand, GAIT_FLIP_HOME_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) {
        ret = hold_pose(GAIT_FLIP_AFTER_HOME_HOLD_MS);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_flip_front_retract, GAIT_FLIP_FRONT_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_FLIP_AFTER_FRONT_HOLD_MS);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_flip_rear_load, GAIT_FLIP_REAR_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = hold_pose(GAIT_FLIP_AFTER_REAR_HOLD_MS);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_flip_front_hook, GAIT_FLIP_HOOK_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_roll_normal_dance_base, GAIT_ROLL_DANCE_ENTRY_MS,
                         SERVICE_V2_SERVO_EASING_LINEAR);
    }
    for (uint32_t cycle = 0U; ret == 0 && cycle < GAIT_ROLL_DANCE_CYCLES; ++cycle) {
        ret = play_roll_frame(s_roll_normal_dance_kick, GAIT_ROLL_DANCE_HALF_BEAT_MS,
                         SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) {
            ret = play_roll_frame(s_roll_normal_dance_base, GAIT_ROLL_DANCE_HALF_BEAT_MS,
                             SERVICE_V2_SERVO_EASING_LINEAR);
        }
    }
    if (ret == 0) {
        ret = play_roll_frame(s_roll_normal_rear_home, GAIT_ROLL_REAR_RETURN_MS,
                         SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_roll_normal_cross, GAIT_ROLL_CROSS_MS, SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        ret = wait_for_roll_landing(true, "normal-half -> reverse");
    }
    if (!prepare_reverse_home) {
        /* 末次背部落地后由 run_roll 直接进入四脚朝天，避免先反向撑起机身。 */
        return ret;
    }
    if (ret == 0) {
        /* 当前世界前腿 CH2/CH3 先落地，再放当前世界后腿 CH0/CH1。 */
        ret = play_roll_frame(s_roll_recover_reverse_front, GAIT_ROLL_RECOVER_PAIR_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_roll_frame(s_pose_reverse_home, GAIT_ROLL_RECOVER_PAIR_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        /* 恢复腿位会再次激起壳体余振；等待连续稳定样本，不能因首帧振动误断电。 */
        ret = wait_for_roll_landing(true, "reverse HOME settled after normal-half");
    }
    return ret;
}

static int play_backward_first_frame(const uint16_t *pose, uint32_t ms, service_v2_servo_easing_t easing)
{
    if (cmd_pending()) return -ECANCELED;
    int ret = play_frame(pose, ms, easing);
    /* The interpolation worker may lag the gait timer by a tick. Do not
     * replace an unfinished frame. This confirms PWM output, not shaft angle. */
    return ret == 0 ? wait_frame_target(pose) : ret;
}

/* Best-effort early handoff after reaching the reverse face. Three gravity-like
 * samples reject a single spike. Missing/unusable IMU data must not block the
 * bounded drive sequence; only command cancellation can stop it here. */
static int backward_reverse_seen(void)
{
    for (uint32_t sample = 0U; sample < 3U; ++sample) {
        if (cmd_pending()) return -ECANCELED;
        service_v2_mpu6050_raw_t raw = {0};
        int ret = service_v2_mpu6050_read_raw(&raw);
        if (ret != 0) return 0;
        uint64_t norm = square_i32(raw.accel_x) + square_i32(raw.accel_y) + square_i32(raw.accel_z);
        if (raw.accel_z < GAIT_FLIP_FACE_REVERSE_Z_MIN ||
            norm < square_i32(GAIT_FLIP_ACCEL_NORM_MIN) ||
            norm > square_i32(GAIT_FLIP_ACCEL_NORM_MAX)) return 0;
        if (sample < 2U) {
            (void)service_v2_actuators_servo_watchdog_kick();
            lisa_thread_mdelay(GAIT_FLIP_IMU_SAMPLE_INTERVAL_MS);
        }
    }
    return 1;
}

static int run_backward_first_half(void)
{
    int ret = play_backward_first_frame(s_backflip_rear_support, 700U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = hold_pose(GAIT_SERVO_PHYSICAL_SETTLE_MS);
    if (ret == 0) ret = play_backward_first_frame(s_backflip_front_press, 180U, SERVICE_V2_SERVO_EASING_LINEAR);
    for (uint32_t cycle = 0U; ret == 0 && cycle < GAIT_BACKFLIP_DRIVE_CYCLES; ++cycle) {
        /* Preparation or the preceding kick can complete a delayed flip. */
        int turned = backward_reverse_seen();
        if (turned != 0) {
            if (turned < 0) ret = turned;
            break;
        }
        LISA_LOGI(TAG, "backflip rear kick %u/%u", (unsigned)(cycle + 1U), (unsigned)GAIT_BACKFLIP_DRIVE_CYCLES);
        ret = play_backward_first_frame(s_backflip_rear_preload, GAIT_BACKFLIP_RELOAD_MS,
                                        SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret != 0) break;
        turned = backward_reverse_seen();
        if (turned != 0) {
            if (turned < 0) ret = turned;
            break;
        }
        ret = play_backward_first_frame(s_backflip_rear_drive, GAIT_BACKFLIP_REAR_DRIVE_MS,
                                        SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret != 0) break;
        /* Let the body rotate before reloading; this hold remains cancellable
         * and services the servo watchdog without issuing another leg stroke. */
        ret = hold_pose(GAIT_BACKFLIP_KICK_HOLD_MS);
        if (ret != 0) break;
        turned = backward_reverse_seen();
        if (turned != 0) {
            if (turned < 0) ret = turned;
            break;
        }
    }
    if (ret == 0) ret = wait_for_roll_landing(true, "backward first-half -> reverse");
    /* Preserve the field-confirmed second half's exact entry pose and timing. */
    if (ret == 0) ret = play_backward_first_frame(s_backflip_recover_front, 400U,
                                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_roll_frame(s_roll_recover_reverse_front, GAIT_ROLL_RECOVER_PAIR_MS,
                                      SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_roll_frame(s_pose_reverse_home, GAIT_ROLL_RECOVER_PAIR_MS,
                                      SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = wait_for_roll_landing(true, "reverse HOME settled after backward first-half");
    return ret;
}

static void run_roll(service_v2_gait_action_t action, uint32_t half_turns)
{
    uint32_t done = 0U;
    roll_face_t start_face = ROLL_FACE_NORMAL;
    roll_face_t current_face = ROLL_FACE_NORMAL;
    int ret;

    status_set(true, action, 0U, 0);

    ret = roll_power_check("roll start");
    if (ret == 0 && action != SERVICE_V2_GAIT_ACTION_BACKFLIP) {
        ret = detect_roll_start_face(&start_face);
    }
    /* Backflip is a fixed sequence intended to start standing normally; it
     * no longer consults IMU to validate that pose or choose a recovery. */
    if (ret == 0 && action == SERVICE_V2_GAIT_ACTION_BACKFLIP &&
        (start_face != ROLL_FACE_NORMAL || half_turns != 2U)) ret = -EINVAL;
    if (ret == 0) {
        current_face = start_face;
        ret = sync_roll_home_from_disabled(current_face);
    }

    while (ret == 0 && done < half_turns) {
        ret = roll_power_check("roll half-turn");
        if (ret == 0) {
            LISA_LOGI(TAG, "roll half-turn %u: %s -> %s", (unsigned)(done + 1U),
                      roll_face_name(current_face),
                      roll_face_name(current_face == ROLL_FACE_NORMAL ? ROLL_FACE_REVERSE : ROLL_FACE_NORMAL));
            ret = current_face == ROLL_FACE_NORMAL
                      ? (action == SERVICE_V2_GAIT_ACTION_BACKFLIP
                             ? run_backward_first_half()
                             : run_normal_half_roll(done + 1U < half_turns))
                      : run_reverse_half_roll();
        }
        if (ret == 0) {
            current_face = current_face == ROLL_FACE_NORMAL ? ROLL_FACE_REVERSE : ROLL_FACE_NORMAL;
            done++;
            status_set(true, action, done, 0);

            /* 保留实机通过的节奏：两个半圈紧接，只在回到起始面且还要继续时停 1.5 s。 */
            bool has_more = done < half_turns;
            if (has_more && current_face == start_face) {
                ret = hold_pose(GAIT_ROLL_CYCLE_HOLD_MS);
            }
        }
    }

    if (ret == 0 && current_face == ROLL_FACE_REVERSE) {
        /* Only the final landing is a feet-up gesture. Intermediate reverse
         * HOME remains inside the half-roll chain for the next flip. */
        ret = play_frame(s_pose_stand, GAIT_TRICK_MOVE_MS,
                         SERVICE_V2_SERVO_EASING_SMOOTH);
        for (uint32_t i = 0U; ret == 0 && i < SERVICE_V2_GAIT_UPSIDE_WIGGLE_CYCLES; ++i) {
            ret = play_frame(s_upside_feedback_wiggle_a, GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS,
                             SERVICE_V2_SERVO_EASING_LINEAR);
            if (ret == 0) ret = play_frame(s_upside_feedback_wiggle_b, GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS,
                                           SERVICE_V2_SERVO_EASING_LINEAR);
        }
        if (ret == 0) ret = play_frame(s_pose_stand, GAIT_TRICK_MOVE_MS,
                                       SERVICE_V2_SERVO_EASING_SMOOTH);
    }

    if (ret != 0) {
        int off_ret = service_v2_actuators_servo_disable_all();
        if (off_ret != 0) {
            LISA_LOGE(TAG, "roll emergency PWM off failed: %d", off_ret);
        }
        if (ret != -ECANCELED) {
            LISA_LOGE(TAG, "roll stopped after %u half-turn(s): %d", (unsigned)done, ret);
        }
    } else {
        LISA_LOGI(TAG, "roll complete after %u half-turn(s), %s", (unsigned)done,
                  current_face == ROLL_FACE_REVERSE ? "feet up" : "normal HOME");
    }

    status_set(false, action, done, ret == -ECANCELED ? 0 : ret);
}

static void background_motion_pause(void)
{
    service_v2_servo_status_t servo = {0};
    if (s_status.current != SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME) {
        s_background_home_action = s_status.current;
    }
    /* Hold the interpolated position; never move during speech just to tidy up. */
    int ret = service_v2_actuators_servo_frame_stop();
    s_background_home_pending = ret == 0 &&
        service_v2_actuators_servo_get_status(&servo) == 0 &&
        servo.enabled_mask == SERVICE_V2_SERVO_ALL_MASK;
    s_background_home_snapshot = servo;
}

static bool background_home_owns_pose(const service_v2_servo_status_t *servo)
{
    if (servo->sequence != s_background_home_snapshot.sequence ||
        servo->enabled_mask != SERVICE_V2_SERVO_ALL_MASK || servo->frame_active ||
        servo->sweep_active || servo->watchdog_ms != 0U || servo->watchdog_expired) return false;
    for (uint32_t ch = 0U; ch < SERVICE_V2_SERVO_COUNT; ++ch) {
        if (servo->current_us[ch] != s_background_home_snapshot.current_us[ch] ||
            servo->target_us[ch] != s_background_home_snapshot.target_us[ch]) return false;
    }
    return true;
}

static void background_home_retry(void)
{
    if (!s_background_home_pending || !service_v2_background_motion_allowed()) return;
    service_v2_servo_status_t servo = {0};
    if (service_v2_actuators_servo_get_status(&servo) != 0) return;
    /* Host/manual control or fail-off owns any change since we stopped. */
    if (!background_home_owns_pose(&servo)) {
        s_background_home_pending = false;
        return;
    }
    if (service_v2_gait_submit_background(SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME, 0U) == 0)
        s_background_home_pending = false;
}

/*
 * PA13 单触摸动作：后腿先用 700 ms 到 [2150,800]，再用 400 ms
 * 到后壳刚好悬空的 [2200,750]，避免拔掉 Type-C 后同步猛收导致机身先触地。
 * 然后右前腿分两段前送，再只朝前快速蹬六次。
 * 所有帧都是完整四路目标，未参与动作的腿也会锁在实机确认的位置，避免单路
 * servo_set 与插值器争用。正常完成的最后一帧必须回整机正常面 HOME。
 */
static void run_right_front_kick(void)
{
    uint32_t done = 0U;
    bool motion_started = false;

    status_set(true, SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK, 0U, 0);

    int ret = prepare_normal_face_motion("right-front kick", &motion_started);
    if (ret == 0) {
        ret = play_frame_settled(s_right_front_kick_rear_preload_stage1,
                                 GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_STAGE1_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_right_front_kick_rear_preload, GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_right_front_kick_approach, GAIT_RIGHT_FRONT_KICK_APPROACH_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_right_front_kick_base, GAIT_RIGHT_FRONT_KICK_EXTEND_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }

    for (uint32_t cycle = 0U; ret == 0 && cycle < SERVICE_V2_GAIT_RIGHT_FRONT_KICK_CYCLES; ++cycle) {
        ret = play_frame_settled(s_right_front_kick_forward, GAIT_RIGHT_FRONT_KICK_HALF_BEAT_MS,
                                 SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) {
            ret = play_frame_settled(s_right_front_kick_base, GAIT_RIGHT_FRONT_KICK_HALF_BEAT_MS,
                                     SERVICE_V2_SERVO_EASING_LINEAR);
        }
        if (ret == 0) {
            done++;
            status_set(true, SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK, done, 0);
        }
    }

    if (ret == 0) {
        ret = recover_normal_face_home_two_stage();
    }

    if (ret == 0) {
        LISA_LOGI(TAG, "right-front kick complete: %u kicks, returned normal HOME", (unsigned)done);
    } else {
        if (motion_started) {
            int off_ret = service_v2_actuators_servo_disable_all();
            if (off_ret != 0) {
                LISA_LOGE(TAG, "right-front kick emergency PWM off failed: %d", off_ret);
            }
        }
        if (ret != -ECANCELED) {
            LISA_LOGE(TAG, "right-front kick stopped after %u kick(s): %d", (unsigned)done, ret);
        }
    }

    status_set(false, SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK, done, ret == -ECANCELED ? 0 : ret);
}

/*
 * 网球发球 v0 严格复用 PA13 已逐拍确认的动作包络，但只完成一次爆发：
 * 后腿蓄力 -> 右前抬臂 -> 前蹬加速 -> 向下挥拍 -> 收拍 -> 正常面 HOME。
 * 第一版不引入任何未经实机确认的新坐标，避免为追求视觉幅度破坏承重边界。
 */
static int play_tennis_run_approach(void)
{
    uint16_t frame[SERVICE_V2_SERVO_COUNT];
    int ret = play_frame(s_gait_center, 160U, SERVICE_V2_SERVO_EASING_EASE_OUT);

    /* 两个快跑周期，形成“先追球”的视觉语义，同时每帧都保留抢占检查。 */
    for (uint32_t cycle = 0U; ret == 0 && cycle < 2U; ++cycle) {
        for (uint32_t step = 0U; ret == 0 && step < GAIT_WALK_FRAMES; ++step) {
            gait_frame_at((2U + step) % GAIT_WALK_FRAMES, frame);
            ret = play_frame(frame, GAIT_RUN_FRAME_MS, SERVICE_V2_SERVO_EASING_LINEAR);
        }
    }
    return ret;
}

static bool choose_tennis_turn_left(void)
{
    /* 单 gait worker 内使用轻量 xorshift；tick 混入避免每次上电方向序列完全相同。 */
    static uint32_t state = 0x6D2B79F5U;
    state ^= (uint32_t)xTaskGetTickCount() + 0x9E3779B9U;
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return (state & 1U) != 0U;
}

static int play_tennis_turn_approach(bool left)
{
    const uint16_t(*frames)[SERVICE_V2_SERVO_COUNT] = left ? s_turn_left : s_turn_right;
    int ret = 0;

    /* 三个快速转向周期，表现跑位后的急停变向；每帧仍保留 STOP/Type-C 抢占。 */
    for (uint32_t cycle = 0U; ret == 0 && cycle < GAIT_TENNIS_SHARP_TURN_CYCLES; ++cycle) {
        for (uint32_t step = 0U; ret == 0 && step < GAIT_TURN_FRAMES; ++step) {
            ret = play_frame(frames[step], GAIT_TENNIS_SHARP_TURN_FRAME_MS,
                             SERVICE_V2_SERVO_EASING_LINEAR);
        }
    }
    return ret;
}

static void run_tennis_groundstroke(service_v2_gait_action_t action)
{
    bool motion_started = false;
    uint32_t done = 0U;
    const bool forehand = action == SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND;
    const bool double_backhand = action == SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND;
    bool turn_left = false;
    const uint16_t *ready = forehand ? s_tennis_forehand_ready
                                    : (double_backhand ? s_tennis_double_backhand_ready
                                                       : s_tennis_backhand_ready);
    const uint16_t *strike = forehand ? s_tennis_forehand_strike
                                     : (double_backhand ? s_tennis_double_backhand_strike
                                                        : s_tennis_backhand_strike);

    status_set(true, action, 0U, 0);
    int ret = prepare_normal_face_motion(service_v2_gait_action_name(action), &motion_started);
    if (ret == 0) ret = play_tennis_run_approach();
    if (ret == 0) {
        turn_left = choose_tennis_turn_left();
        LISA_LOGI(TAG, "%s sharp turn=%s after run", service_v2_gait_action_name(action),
                  turn_left ? "left" : "right");
        ret = play_tennis_turn_approach(turn_left);
    }
    if (ret == 0) ret = play_frame_settled(s_pose_sit, GAIT_JUMP_SIT_MS,
                                           SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(ready, 260U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(strike, GAIT_TENNIS_STRIKE_MS,
                                           SERVICE_V2_SERVO_EASING_LINEAR);
    if (ret == 0) {
        done = 1U;
        ret = recover_normal_face_home_two_stage();
    }
    if (ret != 0 && motion_started) (void)service_v2_actuators_servo_disable_all();
    if (ret == 0) {
        LISA_LOGI(TAG, "%s complete, returned HOME", service_v2_gait_action_name(action));
    } else if (ret != -ECANCELED) {
        LISA_LOGE(TAG, "%s failed: %d", service_v2_gait_action_name(action), ret);
    }
    status_set(false, action, done, ret == -ECANCELED ? 0 : ret);
}

static void run_tennis_serve(void)
{
    bool motion_started = false;
    uint32_t done = 0U;

    status_set(true, SERVICE_V2_GAIT_ACTION_TENNIS_SERVE, 0U, 0);

    int ret = prepare_normal_face_motion("tennis serve", &motion_started);
    if (ret == 0) {
        ret = play_frame_settled(s_tennis_rear_preload_stage1,
                                 GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_STAGE1_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_tennis_rear_preload, GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_tennis_approach, GAIT_RIGHT_FRONT_KICK_APPROACH_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_tennis_base, GAIT_RIGHT_FRONT_KICK_EXTEND_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_tennis_forward, GAIT_RIGHT_FRONT_KICK_HALF_BEAT_MS,
                                 SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_tennis_base, GAIT_TENNIS_STRIKE_MS,
                                 SERVICE_V2_SERVO_EASING_LINEAR);
    }
    if (ret == 0) {
        done = SERVICE_V2_GAIT_TENNIS_SERVE_CYCLES;
        status_set(true, SERVICE_V2_GAIT_ACTION_TENNIS_SERVE, done, 0);
        ret = recover_tennis_serve_home();
    }

    if (ret == 0) {
        LISA_LOGI(TAG, "tennis serve complete, returned normal HOME");
    } else {
        if (motion_started) {
            int off_ret = service_v2_actuators_servo_disable_all();
            if (off_ret != 0) {
                LISA_LOGE(TAG, "tennis serve emergency PWM off failed: %d", off_ret);
            }
        }
        if (ret != -ECANCELED) {
            LISA_LOGE(TAG, "tennis serve failed: %d", ret);
        }
    }

    status_set(false, SERVICE_V2_GAIT_ACTION_TENNIS_SERVE, done, ret == -ECANCELED ? 0 : ret);
}

static void run_paw_wave(service_v2_gait_action_t action, bool left,
                         uint32_t cycles, uint8_t speed_percent)
{
    bool motion_started = false;
    uint32_t done = 0U;
    const uint16_t *base = left ? s_left_front_handshake_base : s_right_front_handshake_base;
    const uint16_t *wave = left ? s_left_front_handshake_wave : s_right_front_handshake_wave;
    if (action == SERVICE_V2_GAIT_ACTION_GREETING) {
        base = s_greeting_base;
        wave = s_greeting_wave;
    } else if (action == SERVICE_V2_GAIT_ACTION_SCRATCH) {
        base = s_scratch_base;
        wave = s_scratch_wave;
    }

    status_set(true, action, 0U, 0);
    int ret = prepare_normal_face_motion(service_v2_gait_action_name(action), &motion_started);
    if (ret == 0) ret = play_frame_settled(s_right_front_kick_rear_preload_stage1,
                                           GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_STAGE1_MS,
                                           SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(s_right_front_kick_rear_preload,
                                           GAIT_RIGHT_FRONT_KICK_REAR_PRELOAD_MS,
                                           SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(base, GAIT_TRICK_MOVE_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (cycles == 0U) cycles = SERVICE_V2_GAIT_HANDSHAKE_CYCLES;
    for (uint32_t i = 0U; ret == 0 && i < cycles; ++i) {
        uint32_t beat_ms = gait_duration_scaled(GAIT_PAW_WAVE_HALF_BEAT_MS, speed_percent);
        ret = play_frame(wave, beat_ms, SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) ret = play_frame(base, beat_ms, SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) done++;
    }
    if (ret == 0) ret = recover_normal_face_home_two_stage();
    if (ret != 0 && motion_started) (void)service_v2_actuators_servo_disable_all();
    if (ret == 0) {
        LISA_LOGI(TAG, "%s complete: cycles=%u, returned HOME", service_v2_gait_action_name(action),
                  (unsigned)done);
    } else if (ret != -ECANCELED) {
        LISA_LOGE(TAG, "%s failed: %d", service_v2_gait_action_name(action), ret);
    }
    status_set(false, action, done, ret == -ECANCELED ? 0 : ret);
}

static int recover_codex_squat_home(void)
{
    return play_frame_settled(s_pose_stand, 300U, SERVICE_V2_SERVO_EASING_SMOOTH);
}

static void run_product_trick(service_v2_gait_action_t action)
{
    bool motion_started = false;
    uint32_t done = 0U;
    status_set(true, action, 0U, 0);
    int ret = prepare_normal_face_motion(service_v2_gait_action_name(action), &motion_started);
    /* A rejected background preflight must not release the current pose. */
    if (ret != 0 && s_background_command) motion_started = false;

    if (action == SERVICE_V2_GAIT_ACTION_SIT) {
        if (ret == 0) ret = play_frame_settled(s_pose_sit, GAIT_TRICK_MOVE_MS,
                                               SERVICE_V2_SERVO_EASING_SMOOTH);
    } else if (action == SERVICE_V2_GAIT_ACTION_KISS) {
        for (uint32_t i = 0U; ret == 0 && i < 3U; ++i) {
            ret = play_frame(s_kiss_left, GAIT_DANCE_HALF_BEAT_MS, SERVICE_V2_SERVO_EASING_SMOOTH);
            if (ret == 0) ret = play_frame(s_kiss_right, GAIT_DANCE_HALF_BEAT_MS,
                                           SERVICE_V2_SERVO_EASING_SMOOTH);
            if (ret == 0) done++;
        }
        if (ret == 0) ret = recover_normal_face_home_two_stage();
    } else if (action == SERVICE_V2_GAIT_ACTION_DANCE) {
        for (uint32_t i = 0U; ret == 0 && i < GAIT_DANCE_CYCLES; ++i) {
            const uint16_t *frame;
            if (i < 4U) {
                /* 1 s：从四脚站立进入一次轻蹲弹起，先确认承重稳定。 */
                switch (i) {
                case 1U: frame = s_dance_squat; break;
                case 2U: frame = s_dance_pop; break;
                default: frame = s_pose_stand; break;
                }
            } else if (i < 12U) {
                /* 2 s：两组对角腿锁点踏步。 */
                frame = (i & 1U) == 0U ? s_dance_diagonal_a : s_dance_diagonal_b;
            } else if (i < 24U) {
                /* 3 s：左爪、回中、右爪、回中；抬爪后必回四脚承重。 */
                switch (i & 3U) {
                case 0U: frame = s_dance_left_paw; break;
                case 2U: frame = s_dance_right_paw; break;
                default: frame = s_pose_stand; break;
                }
            } else if (i < 32U) {
                /* 2 s：前腿支撑，后腿做节拍化左右锁点。 */
                frame = (i & 1U) == 0U ? s_dance_hip_left : s_dance_hip_right;
            } else if (i < 40U) {
                /* 2 s：小幅蹲起，不再进入会让屁股落地的深蹲位置。 */
                frame = (i & 1U) == 0U ? s_dance_squat : s_dance_pop;
            } else {
                /* 1.25 s：对角锁点收尾，最后两拍主动回四脚站立。 */
                switch (i) {
                case 40U: frame = s_dance_diagonal_a; break;
                case 41U: frame = s_dance_diagonal_b; break;
                case 42U: frame = s_dance_left_paw; break;
                default: frame = s_pose_stand; break;
                }
            }
            ret = play_frame(frame, GAIT_DANCE_HALF_BEAT_MS, SERVICE_V2_SERVO_EASING_LINEAR);
            if (ret == 0) done++;
        }
        if (ret == 0) ret = recover_dance_home_two_stage();
    } else if (action == SERVICE_V2_GAIT_ACTION_JUMP) {
        LISA_LOGI(TAG, "jump stage=sit-preload");
        if (ret == 0) ret = play_frame_settled(s_pose_sit, GAIT_JUMP_SIT_MS,
                                               SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) LISA_LOGI(TAG, "jump stage=kick-out");
        if (ret == 0) ret = play_frame_settled(s_jump_kick, GAIT_JUMP_KICK_MS,
                                               SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) LISA_LOGI(TAG, "jump stage=release");
        if (ret == 0) ret = play_frame_settled(s_pose_stand, GAIT_JUMP_RELEASE_MS,
                                               SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) done = 1U;
    } else if (action == SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT) {
        for (uint32_t i = 0U; ret == 0 && i < SERVICE_V2_GAIT_WIGGLE_BUTT_CYCLES; ++i) {
            ret = play_frame(s_butt_wiggle_left, GAIT_BUTT_WIGGLE_HALF_BEAT_MS,
                             SERVICE_V2_SERVO_EASING_LINEAR);
            if (ret == 0) ret = play_frame(s_butt_wiggle_right, GAIT_BUTT_WIGGLE_HALF_BEAT_MS,
                                           SERVICE_V2_SERVO_EASING_LINEAR);
            if (ret == 0) done++;
        }
        if (ret == 0) ret = recover_normal_face_home_two_stage();
    } else if (action == SERVICE_V2_GAIT_ACTION_CODEX_SQUAT) {
        if (ret == 0) ret = play_frame_settled(s_dance_squat, 280U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = hold_pose(160U);
        if (ret == 0) ret = recover_codex_squat_home();
        if (ret == 0) done = 1U;
    }

    if (ret == -ECANCELED && motion_started && s_background_command) background_motion_pause();
    else if (ret != 0 && motion_started) (void)service_v2_actuators_servo_disable_all();
    if (ret == 0) {
        LISA_LOGI(TAG, "%s complete: steps=%u", service_v2_gait_action_name(action), (unsigned)done);
    } else if (ret != -ECANCELED) {
        LISA_LOGE(TAG, "%s failed: %d", service_v2_gait_action_name(action), ret);
    }
    status_set(false, action, done, ret == -ECANCELED ? 0 : ret);
}

static void run_upside_wiggle(void)
{
    uint32_t done = 0U;
    status_set(true, SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE, 0U, 0);
    /* This is a slow +/-60 us feedback gesture, not a roll.  Keep the battery
     * gate but allow it during Type-C diagnostics. */
    int ret = motion_battery_check("upside wiggle");
    if (ret == 0) ret = flip_pose_check(true, "upside wiggle");
    if (ret == 0) ret = sync_roll_home_from_disabled(ROLL_FACE_NORMAL);
    for (uint32_t i = 0U; ret == 0 && i < SERVICE_V2_GAIT_UPSIDE_WIGGLE_CYCLES; ++i) {
        ret = play_frame(s_upside_feedback_wiggle_a, GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS,
                         SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) ret = play_frame(s_upside_feedback_wiggle_b, GAIT_UPSIDE_WIGGLE_HALF_BEAT_MS,
                                       SERVICE_V2_SERVO_EASING_LINEAR);
        if (ret == 0) done++;
    }
    if (ret == 0) ret = play_frame(s_pose_stand, GAIT_TRICK_MOVE_MS,
                                    SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret != 0) (void)service_v2_actuators_servo_disable_all();
    if (ret == 0) {
        LISA_LOGI(TAG, "upside wiggle complete: cycles=%u", (unsigned)done);
    } else if (ret != -ECANCELED) {
        LISA_LOGE(TAG, "upside wiggle failed: %d", ret);
    }
    status_set(false, SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE, done, ret == -ECANCELED ? 0 : ret);
}

static int recover_reverse_face_for_stand(void)
{
    roll_face_t face = ROLL_FACE_NORMAL;
    bool reserved_here = false;
    int ret = detect_roll_start_face(&face);
    if (ret != 0) {
        if (!gait_safety_gates_enabled() && ret != -ECANCELED) {
            LISA_LOGW(TAG, "IMU face unavailable (%d), proceeding because safety gates are disabled", ret);
            return 0;
        }
        return ret;
    }
    if (face == ROLL_FACE_NORMAL) return 0;
    ret = roll_power_check("stand reverse recovery");
    if (ret == 0) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        /* FULL_ROLL_LOOP owns the reservation from submit time.  Reuse that
         * ownership during its preflight and let command_end release it. */
        if (!s_roll_reserved) {
            if (s_pending_commands != 0U || uxQueueMessagesWaiting(s_queue) != 0U) {
                ret = -ECANCELED;
            } else {
                s_roll_reserved = true;
                reserved_here = true;
            }
        }
        lisa_mutex_unlock(s_lock);
    }
    if (ret == 0) ret = sync_roll_home_from_disabled(ROLL_FACE_REVERSE);
    if (ret == 0) ret = run_reverse_half_roll();
    if (ret != 0) (void)service_v2_actuators_servo_disable_all();
    if (reserved_here) {
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        s_roll_reserved = false;
        lisa_mutex_unlock(s_lock);
    }
    return ret;
}

static int gait_preflight_normal_face(service_v2_gait_action_t action)
{
    if (!service_v2_gait_action_requires_normal_face(action)) {
        return 0;
    }

    LISA_LOGI(TAG, "preflight face check: %s", service_v2_gait_action_name(action));
    int ret = recover_reverse_face_for_stand();
    if (ret == 0) {
        LISA_LOGI(TAG, "preflight normal face ready: %s", service_v2_gait_action_name(action));
    } else if (ret != -ECANCELED) {
        LISA_LOGE(TAG, "preflight face recovery failed: %s result=%d",
                  service_v2_gait_action_name(action), ret);
    }
    return ret;
}

static void run_all_off(void)
{
    status_set(true, SERVICE_V2_GAIT_ACTION_ALL_OFF, 0U, 0);
    int ret = service_v2_actuators_servo_disable_all();
    if (ret == 0) {
        LISA_LOGI(TAG, "all servo PWM outputs disabled");
    } else {
        LISA_LOGE(TAG, "all-off failed: %d", ret);
    }
    status_set(false, SERVICE_V2_GAIT_ACTION_ALL_OFF, 0U, ret);
}

static void run_standby_fold(void)
{
    status_set(true, SERVICE_V2_GAIT_ACTION_STANDBY_FOLD, 0U, 0);

    int ret = play_frame_settled(s_pose_stand, GAIT_STANDBY_STAND_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) {
        ret = play_frame_settled(s_standby_rear_folded,
                                 GAIT_STANDBY_REAR_FOLD_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }
    if (ret == 0) {
        ret = play_frame_settled(s_standby_all_folded,
                                 GAIT_STANDBY_FRONT_FOLD_MS,
                                 SERVICE_V2_SERVO_EASING_SMOOTH);
    }

    if (ret == 0) {
        LISA_LOGI(TAG,
                  "standby fold complete (PA04/PA05/PA06/PA07=500/2500/2500/500us), PWM held");
    } else if (ret != -ECANCELED) {
        LISA_LOGE(TAG, "standby fold failed: %d", ret);
    }
    status_set(false, SERVICE_V2_GAIT_ACTION_STANDBY_FOLD, 0U,
               ret == -ECANCELED ? 0 : ret);
}

static void run_pose(service_v2_gait_action_t action)
{
    const uint16_t *pose = s_pose_stand;
    uint32_t duration_ms = GAIT_POSE_MS;

    if (action == SERVICE_V2_GAIT_ACTION_PRONE) {
        pose = s_pose_prone;
    } else if (action == SERVICE_V2_GAIT_ACTION_ASSEMBLY) {
        pose = s_pose_assembly;
        duration_ms = GAIT_ASSEMBLY_MOVE_MS;
    }

    status_set(true, action, 0U, 0);

    int ret = action == SERVICE_V2_GAIT_ACTION_STAND ? recover_reverse_face_for_stand() : 0;
    if (ret == 0) ret = play_frame(pose, duration_ms, SERVICE_V2_SERVO_EASING_EASE_OUT);

    if (ret == 0 && action == SERVICE_V2_GAIT_ACTION_PRONE) {
        /*
         * 趴下到位之后身体已经压在地上，继续给扭矩只是让舵机空耗、发热并顶着
         * 机械限位。卸力是安全的，起立时 servo_frame 会自己把四路使能回来。
         */
        ret = service_v2_actuators_servo_disable_all();
    }

    if (ret != 0 && ret != -ECANCELED) {
        LISA_LOGE(TAG, "%s failed: %d", service_v2_gait_action_name(action), ret);
    } else if (ret == 0 && action == SERVICE_V2_GAIT_ACTION_ASSEMBLY) {
        LISA_LOGI(TAG, "assembly pose reached (FL/FR/RL/RR=1500/1500/1615/1315us), PWM held");
    }
    status_set(false, action, 0U, ret == -ECANCELED ? 0 : ret);
}

static void run_walk(service_v2_gait_action_t action, uint32_t cycles, uint8_t speed_percent)
{
    bool forward = action == SERVICE_V2_GAIT_ACTION_FORWARD || action == SERVICE_V2_GAIT_ACTION_RUN_FORWARD;
    bool running = action == SERVICE_V2_GAIT_ACTION_RUN_FORWARD || action == SERVICE_V2_GAIT_ACTION_RUN_BACKWARD;
    uint32_t frame_ms = gait_duration_scaled(running ? GAIT_RUN_FRAME_MS : GAIT_WALK_FRAME_MS,
                                             speed_percent);
    uint32_t done = 0U;
    uint16_t frame[SERVICE_V2_SERVO_COUNT];

    status_set(true, action, 0U, 0);

    /*
     * 先四腿一起进入步态中心，再进到已验证的相位 1。旧版从 Home 直接插值到
     * 原始帧 0，启动时只有一条腿显眼地先伸出；这两段过渡已在实机消掉该问题。
     */
    int ret = play_frame(s_gait_center, GAIT_APPROACH_CENTER_MS, SERVICE_V2_SERVO_EASING_EASE_OUT);

    if (ret == 0) {
        gait_frame_at(1U, frame);
        ret = play_frame(frame, GAIT_APPROACH_PHASE_MS, SERVICE_V2_SERVO_EASING_EASE_OUT);
    }

    while (ret == 0 && (cycles == 0U || done < cycles)) {
        for (uint32_t step = 0U; step < GAIT_WALK_FRAMES; ++step) {
            uint32_t raw_index =
                forward ? (2U + step) % GAIT_WALK_FRAMES : (GAIT_WALK_FRAMES - step) % GAIT_WALK_FRAMES;

            /* 从相位 1 无停顿接上：前进 2..7,0,1；后退 0,7..2,1。 */
            gait_frame_at(raw_index, frame);
            ret = play_frame(frame, frame_ms, SERVICE_V2_SERVO_EASING_LINEAR);
            if (ret != 0) {
                break;
            }
        }
        if (ret != 0) {
            break;
        }
        done++;
        status_set(true, action, done, 0);
    }

    if (ret != 0 && ret != -ECANCELED) {
        LISA_LOGE(TAG, "%s failed after %u cycle(s): %d", service_v2_gait_action_name(action), (unsigned)done, ret);
    }

    /*
     * 有限周期正常走完必须平滑回 Home，不能停在最后一张步态帧上。被新命令抢占时
     * 不在这里插 Home：下一条若是 prone 应直接趴下，若是 stop/stand 会自己回 Home。
     */
    if (ret == 0) {
        ret = play_frame(s_pose_stand, GAIT_HOME_RETURN_MS, SERVICE_V2_SERVO_EASING_EASE_OUT);
        if (ret == 0) {
            LISA_LOGI(TAG, "%s complete, returned home", service_v2_gait_action_name(action));
        }
    }

    /* 回到 Home 后保持扭矩站住，不卸力——卸力会直接趴下去。 */
    status_set(false, action, done, ret == -ECANCELED ? 0 : ret);
}

static void run_turn(service_v2_gait_action_t action, uint32_t cycles, uint8_t speed_percent)
{
    const uint16_t(*frames)[SERVICE_V2_SERVO_COUNT] =
        action == SERVICE_V2_GAIT_ACTION_TURN_LEFT ? s_turn_left : s_turn_right;
    uint32_t done = 0U;

    status_set(true, action, 0U, 0);

    /* 从趴下或半步被抢占的姿态进来时，先回 Home 再转，避免突然单腿承重。 */
    int ret = play_frame(s_pose_stand, GAIT_APPROACH_CENTER_MS, SERVICE_V2_SERVO_EASING_EASE_OUT);

    while (ret == 0 && (cycles == 0U || done < cycles)) {
        for (uint32_t i = 0U; i < GAIT_TURN_FRAMES; ++i) {
            ret = play_frame(frames[i], gait_duration_scaled(GAIT_TURN_FRAME_MS, speed_percent),
                             SERVICE_V2_SERVO_EASING_LINEAR);
            if (ret != 0) {
                break;
            }
        }
        if (ret != 0) {
            break;
        }
        done++;
        status_set(true, action, done, 0);
    }

    if (ret != 0 && ret != -ECANCELED) {
        LISA_LOGE(TAG, "%s failed after %u cycle(s): %d", service_v2_gait_action_name(action), (unsigned)done, ret);
    }

    /* 有限转向和前后走一样，完成后必须回逐腿标定的精确 Home。 */
    if (ret == 0) {
        ret = play_frame(s_pose_stand, GAIT_HOME_RETURN_MS, SERVICE_V2_SERVO_EASING_EASE_OUT);
        if (ret == 0) {
            LISA_LOGI(TAG, "%s complete, returned home", service_v2_gait_action_name(action));
        }
    }

    status_set(false, action, done, ret == -ECANCELED ? 0 : ret);
}

static void run_stop(void)
{
    status_set(true, SERVICE_V2_GAIT_ACTION_STOP, 0U, 0);

    /*
     * STOP 的产品语义是停止移动并站稳，不是冻结在半步抬腿姿态。
     * 真正冷启动时 sequence=0，先预装 HOME 再原子使能，避免先给出默认
     * 1500 us。动作中途卸力后 sequence>0，必须保留当前脉宽并平滑回 HOME；
     * 否则实物还在中途姿态时会瞬间吃到 HOME 目标。部分通道使能时无法
     * 可靠知道四条腿的实际姿态，因此关闭全部 PWM 并拒绝自动拉回。
     */
    service_v2_servo_status_t servo = {0};
    int ret = cmd_pending() ? -ECANCELED : service_v2_actuators_servo_get_status(&servo);

    if (ret == 0 && servo_status_is_pristine(&servo)) {
        ret = enable_normal_home_from_disabled(GAIT_RIGHT_FRONT_KICK_COLD_HOLD_MS);
    } else if (ret == 0 &&
               (servo.enabled_mask == 0U || servo.enabled_mask == SERVICE_V2_SERVO_ALL_MASK)) {
        ret = play_frame_settled(s_pose_stand, GAIT_HOME_RETURN_MS, SERVICE_V2_SERVO_EASING_EASE_OUT);
    } else if (ret == 0) {
        ret = -EACCES;
    }

    if (ret != 0) {
        (void)service_v2_actuators_servo_disable_all();
        LISA_LOGE(TAG, "stop/home failed: %d", ret);
    } else {
        LISA_LOGI(TAG, "stop complete, returned home");
    }
    status_set(false, SERVICE_V2_GAIT_ACTION_STOP, 0U, ret == -ECANCELED ? 0 : ret);
}

/* 131_music: 18.8s, measured onsets 140ms + n*270ms (~111 BPM).
 * Alternate physical RR (CH2) / RL (CH3) by 90us (~8deg), returning
 * to HOME on each intervening half-beat. Front legs keep their support,
 * and the rear pair never lifts together. */
static void music_beat_pose(uint32_t phase, uint16_t *frame)
{
    memcpy(frame, s_pose_stand, sizeof(s_pose_stand));
    if ((phase % 4U) == 0U) frame[2] += 90U;
    else if ((phase % 4U) == 2U) frame[3] -= 90U;
}

static void run_music_beat(void)
{
    bool started = false;
    uint32_t ticket = 0U, done = 0U, elapsed = 0U, last_phase = UINT32_MAX;
    status_set(true, SERVICE_V2_GAIT_ACTION_MUSIC_BEAT, 0U, 0);
    int ret = prepare_normal_face_motion("music-beat", &started);
    if (ret == 0 && cmd_pending()) ret = -ECANCELED;
    if (ret == 0) ticket = voice_player_beat_music_begin();
    bool playing = false;
    for (; ret == 0 && elapsed < 25000U; elapsed += GAIT_TICK_MS) {
        if (cmd_pending()) { ret = -ECANCELED; break; }
        uint32_t position = 0U;
        int state = voice_player_beat_music_position(ticket, &position);
        if (state == 1) break;
        if (state == -EAGAIN) {
            if ((!playing && elapsed >= 3000U) || (playing && elapsed >= 22000U)) {
                ret = -ETIMEDOUT;
                break;
            }
        } else if (state != 0) {
            ret = state;
            break;
        } else {
            playing = true;
            /* Schedule arrival near the next onset, synchronized to decoder
             * progress, rather than accumulate frame and scheduler delays. */
            uint32_t phase = position < 140U ? 0U : 1U + (position - 140U) / 270U;
            if (phase != last_phase && position < 18600U) {
                uint16_t frame[SERVICE_V2_SERVO_COUNT];
                music_beat_pose(phase, frame);
                uint32_t next_ms = 140U + phase * 270U;
                uint32_t move_ms = next_ms > position ? next_ms - position : GAIT_TICK_MS;
                move_ms = ((move_ms + GAIT_TICK_MS - 1U) / GAIT_TICK_MS) * GAIT_TICK_MS;
                if (move_ms > 280U) move_ms = 280U;
                ret = play_frame(frame, move_ms, SERVICE_V2_SERVO_EASING_SMOOTH);
                elapsed += move_ms;
                last_phase = phase;
                done++;
            }
        }
        if (ret == 0) ret = hold_pose(GAIT_TICK_MS);
    }
    if (elapsed >= 25000U && ret == 0) ret = -ETIMEDOUT;
    voice_player_beat_music_end(ticket);
    if (ret == 0) ret = play_frame_settled(s_pose_stand, 400U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret != 0 && started) (void)service_v2_actuators_servo_disable_all();
    status_set(false, SERVICE_V2_GAIT_ACTION_MUSIC_BEAT, done, ret == -ECANCELED ? 0 : ret);
}

static int recover_pushup_home(void)
{
    /* Front support first, then bring the extended rear pair back under body. */
    int ret = play_frame_settled(s_pushup_rear, 700U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(s_pose_stand, 1100U, SERVICE_V2_SERVO_EASING_SMOOTH);
    return ret;
}

static void run_pushup(uint32_t cycles)
{
    bool started = false;
    uint32_t done = 0U;
    int ret = prepare_normal_face_motion("pushup", &started);
    if (ret == 0) ret = play_frame_settled(s_pushup_rear, 1100U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(s_pushup_up, 700U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = hold_pose(400U);
    for (uint32_t i = 0U; ret == 0 && i < cycles; ++i) {
        ret = play_frame_settled(s_pushup_down, 700U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = hold_pose(180U);
        if (ret == 0) ret = play_frame_settled(s_pushup_up, 900U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = hold_pose(300U);
        if (ret == 0) done++;
    }
    if (ret == 0) ret = recover_pushup_home();
    if (ret == -ECANCELED && s_background_command) background_motion_pause();
    else if (ret != 0 && started) (void)service_v2_actuators_servo_disable_all();
    status_set(false, SERVICE_V2_GAIT_ACTION_PUSHUP, done, ret == -ECANCELED ? 0 : ret);
}

static int recover_idle_stretch_home(void)
{
    /* Restore front support before straightening the slightly bent rear pair. */
    int ret = play_frame_settled(s_idle_stretch_support, 1200U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(s_pose_stand, 1000U, SERVICE_V2_SERVO_EASING_SMOOTH);
    return ret;
}

static void run_idle_stretch(void)
{
    bool started = false;
    uint32_t done = 0U;
    int ret = prepare_normal_face_motion("idle_stretch", &started);
    if (ret == 0) ret = play_frame_settled(s_idle_stretch_support, 1000U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(s_idle_stretch_extend, 1400U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = hold_pose(1000U);
    for (uint32_t i = 0U; ret == 0 && i < 2U; ++i) {
        ret = play_frame(s_idle_stretch_a, 650U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = play_frame(s_idle_stretch_b, 500U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = play_frame(s_idle_stretch_extend, 650U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = hold_pose(500U);
        if (ret == 0) done++;
    }
    if (ret == 0) ret = hold_pose(700U);
    if (ret == 0) ret = recover_idle_stretch_home();
    if (ret == -ECANCELED) background_motion_pause();
    else if (ret != 0 && started) (void)service_v2_actuators_servo_disable_all();
    status_set(false, SERVICE_V2_GAIT_ACTION_IDLE_STRETCH, done, ret == -ECANCELED ? 0 : ret);
}

static uint32_t idle_scratch_channel(void)
{
    /* Seed once: fixed one-minute timing must not reuse a timer's low bit. */
    if (s_idle_scratch_random_state == 0U) s_idle_scratch_random_state = lisa_rand32() | 1U;
    s_idle_scratch_random_state ^= s_idle_scratch_random_state << 13;
    s_idle_scratch_random_state ^= s_idle_scratch_random_state >> 17;
    s_idle_scratch_random_state ^= s_idle_scratch_random_state << 5;
    return (s_idle_scratch_random_state & 1U) != 0U ? 1U : 0U;
}

static int recover_idle_scratch_home(void)
{
    /* Return the selected front paw to the sit pose before standing. */
    int ret = play_frame_settled(s_pose_sit, 500U, SERVICE_V2_SERVO_EASING_SMOOTH);
    if (ret == 0) ret = play_frame_settled(s_pose_stand, 900U, SERVICE_V2_SERVO_EASING_SMOOTH);
    return ret;
}

static void run_idle_scratch(void)
{
    bool started = false;
    uint32_t done = 0U;
    int ret = prepare_normal_face_motion("idle_scratch", &started);
    if (ret == 0) {
        const uint32_t ch = idle_scratch_channel();
        /* User-selected physical front-paw endpoints: CH0 800/1100 us,
         * CH1 2200/1900 us. Keep the other front paw and both rear legs in
         * the shared sit pose; these are motion targets, not joint limits. */
        uint16_t far[SERVICE_V2_SERVO_COUNT];
        uint16_t near[SERVICE_V2_SERVO_COUNT];
        memcpy(far, s_pose_sit, sizeof(far));
        memcpy(near, s_pose_sit, sizeof(near));
        far[ch] = ch == 0U ? 800U : 2200U;
        near[ch] = ch == 0U ? 1100U : 1900U;

        ret = play_frame_settled(s_pose_sit, 700U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = play_frame_settled(far, 600U, SERVICE_V2_SERVO_EASING_SMOOTH);
        if (ret == 0) ret = hold_pose(120U);
        for (uint32_t i = 0U; ret == 0 && i < 5U; ++i) {
            /* Far -> near -> far is one complete round trip. */
            ret = play_frame_settled(near, 200U, SERVICE_V2_SERVO_EASING_SMOOTH);
            if (ret == 0) ret = play_frame_settled(far, 200U, SERVICE_V2_SERVO_EASING_SMOOTH);
            if (ret == 0) done++;
        }
        if (ret == 0) ret = recover_idle_scratch_home();
    }
    if (ret == -ECANCELED) background_motion_pause();
    else if (ret != 0 && started) (void)service_v2_actuators_servo_disable_all();
    status_set(false, SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH, done, ret == -ECANCELED ? 0 : ret);
}

static void run_background_home(void)
{
    bool started = false;
    service_v2_servo_status_t servo = {0};
    /* Recheck after queueing as a manual command may have won that window. */
    int ret = service_v2_actuators_servo_get_status(&servo);
    if (ret == 0 && !background_home_owns_pose(&servo)) ret = -EACCES;
    if (ret == 0) ret = prepare_normal_face_motion("background_home", &started);
    if (ret == 0) {
        if (s_background_home_action == SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH)
            ret = recover_idle_scratch_home();
        else if (s_background_home_action == SERVICE_V2_GAIT_ACTION_PUSHUP)
            ret = recover_pushup_home();
        else if (s_background_home_action == SERVICE_V2_GAIT_ACTION_CODEX_SQUAT)
            ret = recover_codex_squat_home();
        else
            ret = recover_idle_stretch_home();
    }
    if (ret == -ECANCELED) background_motion_pause();
    else if (ret != 0 && started) (void)service_v2_actuators_servo_disable_all();
    status_set(false, SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME, 0U,
               ret == -ECANCELED ? 0 : ret);
}

static int command_preflight(const gait_cmd_t *cmd)
{
    /* Foreground preflight may flip an upside-down dog upright. Autonomous
     * commands must never do that: their prepare step requires normal pose. */
    if (cmd->background) return cmd_pending() ? -ECANCELED : 0;
    return gait_preflight_normal_face(cmd->action);
}

static void gait_task(void *arg)
{
    (void)arg;

    for (;;) {
        gait_cmd_t cmd;

        if (xQueueReceive(s_queue, &cmd,
                          s_background_home_pending ? pdMS_TO_TICKS(100U) : portMAX_DELAY) != pdTRUE) {
            background_home_retry();
            continue;
        }

        command_begin(&cmd);

        int preflight_ret = command_preflight(&cmd);
        if (preflight_ret != 0) {
            if (preflight_ret == -ECANCELED &&
                cmd.action == SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME) {
                /* Nothing moved yet. Keep the old snapshot so a manual change
                 * during queueing is detected instead of being claimed by us. */
                s_background_home_pending = true;
            }
            status_set(false, cmd.action, 0U,
                       preflight_ret == -ECANCELED ? 0 : preflight_ret);
        } else switch (cmd.action) {
        case SERVICE_V2_GAIT_ACTION_STAND:
        case SERVICE_V2_GAIT_ACTION_PRONE:
        case SERVICE_V2_GAIT_ACTION_ASSEMBLY:
            run_pose(cmd.action);
            break;
        case SERVICE_V2_GAIT_ACTION_IDLE_STRETCH:
            run_idle_stretch();
            break;
        case SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH:
            run_idle_scratch();
            break;
        case SERVICE_V2_GAIT_ACTION_PUSHUP:
            run_pushup(cmd.cycles);
            break;
        case SERVICE_V2_GAIT_ACTION_MUSIC_BEAT:
            run_music_beat();
            break;
        case SERVICE_V2_GAIT_ACTION_BACKFLIP:
            run_roll(cmd.action, 2U);
            break;
        case SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME:
            run_background_home();
            break;
        case SERVICE_V2_GAIT_ACTION_STANDBY_FOLD:
            run_standby_fold();
            break;
        case SERVICE_V2_GAIT_ACTION_FLIP_RISE:
            run_flip_rise();
            break;
        case SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP:
            run_roll(cmd.action, cmd.cycles);
            break;
        case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK:
            run_right_front_kick();
            break;
        case SERVICE_V2_GAIT_ACTION_TENNIS_SERVE:
            run_tennis_serve();
            break;
        case SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND:
        case SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND:
        case SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND:
            run_tennis_groundstroke(cmd.action);
            break;
        case SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE:
            run_paw_wave(cmd.action, true, cmd.cycles, cmd.speed_percent);
            break;
        case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE:
            run_paw_wave(cmd.action, false, cmd.cycles, cmd.speed_percent);
            break;
        case SERVICE_V2_GAIT_ACTION_GREETING:
        case SERVICE_V2_GAIT_ACTION_SCRATCH:
            run_paw_wave(cmd.action, false, cmd.cycles, cmd.speed_percent);
            break;
        case SERVICE_V2_GAIT_ACTION_SPIN:
            run_turn(((uint32_t)xTaskGetTickCount() & 1U) != 0U ? SERVICE_V2_GAIT_ACTION_TURN_LEFT
                                                                : SERVICE_V2_GAIT_ACTION_TURN_RIGHT,
                     cmd.cycles, cmd.speed_percent);
            break;
        case SERVICE_V2_GAIT_ACTION_SIT:
        case SERVICE_V2_GAIT_ACTION_KISS:
        case SERVICE_V2_GAIT_ACTION_DANCE:
        case SERVICE_V2_GAIT_ACTION_JUMP:
        case SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT:
        case SERVICE_V2_GAIT_ACTION_CODEX_SQUAT:
            run_product_trick(cmd.action);
            break;
        case SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE:
            run_upside_wiggle();
            break;
        case SERVICE_V2_GAIT_ACTION_ALL_OFF:
            run_all_off();
            break;
        case SERVICE_V2_GAIT_ACTION_FORWARD:
        case SERVICE_V2_GAIT_ACTION_BACKWARD:
        case SERVICE_V2_GAIT_ACTION_RUN_FORWARD:
        case SERVICE_V2_GAIT_ACTION_RUN_BACKWARD:
            run_walk(cmd.action, cmd.cycles, cmd.speed_percent);
            break;
        case SERVICE_V2_GAIT_ACTION_TURN_LEFT:
        case SERVICE_V2_GAIT_ACTION_TURN_RIGHT:
            run_turn(cmd.action, cmd.cycles, cmd.speed_percent);
            break;
        case SERVICE_V2_GAIT_ACTION_STOP:
        default:
            run_stop();
            break;
        }

        service_v2_gait_action_finished_t finished = {
            .action = cmd.action,
            .result = command_result(),
            .background = cmd.background,
        };
        if (cmd.action == SERVICE_V2_GAIT_ACTION_DANCE)
            service_v2_rgb_effect_show_stop(SERVICE_V2_RGB_SHOW_DANCE);
        command_end();
        (void)voice_msg_pub(VOICE_MSG_V2_GAIT_ACTION_FINISHED, &finished, sizeof(finished));
        LISA_LOGI(TAG, "motion finished: %s result=%d",
                  service_v2_gait_action_name(finished.action), finished.result);
    }
}

int service_v2_gait_init(void)
{
    if (s_ready) {
        return 0;
    }

    if (!service_v2_actuators_is_ready()) {
        LISA_LOGE(TAG, "actuator service is not ready");
        return -ENODEV;
    }

    if (s_lock == NULL) {
        s_lock = lisa_mutex_create();
        if (s_lock == NULL) {
            LISA_LOGE(TAG, "failed to create gait lock");
            return -ENOMEM;
        }
    }

    if (s_queue == NULL) {
        s_queue = xQueueCreate(GAIT_QUEUE_DEPTH, sizeof(gait_cmd_t));
        if (s_queue == NULL) {
            LISA_LOGE(TAG, "failed to create gait queue");
            return -ENOMEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.current = SERVICE_V2_GAIT_ACTION_STOP;
    s_pending_commands = 0U;
    s_worker_active = false;
    s_roll_reserved = false;
    s_normal_face_motion_reserved = false;
    s_normal_face_motion_terminal_pending = 0U;
    s_normal_face_motion_pose_validated = false;

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"v2_gait",
        .stack_size = GAIT_TASK_STACK,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    if (lisa_thread_create(&attr, gait_task, NULL) == NULL) {
        LISA_LOGE(TAG, "failed to create gait worker");
        return -ENOMEM;
    }

    s_ready = true;
    s_status.ready = true;

    LISA_LOGI(TAG, "gait ready (walk=%ums run=%ums turn=%ums safety_gates=%s)",
              (unsigned)SERVICE_V2_GAIT_WALK_CYCLE_MS,
              (unsigned)SERVICE_V2_GAIT_RUN_CYCLE_MS,
              (unsigned)SERVICE_V2_GAIT_TURN_CYCLE_MS,
              gait_safety_gates_enabled() ? "on" : "off");
    return 0;
}

static int normalize_command(service_v2_gait_action_t action, uint32_t *cycles)
{
    switch (action) {
    case SERVICE_V2_GAIT_ACTION_PUSHUP:
        if (*cycles == 0U) *cycles = 4U;
        if (*cycles < 3U || *cycles > 5U) return -EINVAL;
        break;
    case SERVICE_V2_GAIT_ACTION_BACKFLIP:
        if (*cycles == 0U) *cycles = 1U;
        if (*cycles != 1U) return -EINVAL;
        break;
    case SERVICE_V2_GAIT_ACTION_IDLE_STRETCH:
    case SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH:
    case SERVICE_V2_GAIT_ACTION_MUSIC_BEAT:
    case SERVICE_V2_GAIT_ACTION_STOP:
    case SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME:
    case SERVICE_V2_GAIT_ACTION_STAND:
    case SERVICE_V2_GAIT_ACTION_PRONE:
    case SERVICE_V2_GAIT_ACTION_ASSEMBLY:
    case SERVICE_V2_GAIT_ACTION_STANDBY_FOLD:
    case SERVICE_V2_GAIT_ACTION_FLIP_RISE:
    case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK:
    case SERVICE_V2_GAIT_ACTION_TENNIS_SERVE:
    case SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND:
    case SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND:
    case SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND:
    case SERVICE_V2_GAIT_ACTION_SIT:
    case SERVICE_V2_GAIT_ACTION_KISS:
    case SERVICE_V2_GAIT_ACTION_DANCE:
    case SERVICE_V2_GAIT_ACTION_JUMP:
    case SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT:
    case SERVICE_V2_GAIT_ACTION_CODEX_SQUAT:
    case SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE:
    case SERVICE_V2_GAIT_ACTION_ALL_OFF:
        *cycles = 0U;
        break;
    case SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE:
    case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE:
    case SERVICE_V2_GAIT_ACTION_GREETING:
    case SERVICE_V2_GAIT_ACTION_SCRATCH:
    case SERVICE_V2_GAIT_ACTION_SPIN:
        if (*cycles > SERVICE_V2_GAIT_MAX_CYCLES) return -EINVAL;
        break;
    case SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP:
        if (*cycles == 0U || *cycles > SERVICE_V2_GAIT_MAX_ROLL_CYCLES) {
            return -EINVAL;
        }
        break;
    case SERVICE_V2_GAIT_ACTION_FORWARD:
    case SERVICE_V2_GAIT_ACTION_BACKWARD:
    case SERVICE_V2_GAIT_ACTION_RUN_FORWARD:
    case SERVICE_V2_GAIT_ACTION_RUN_BACKWARD:
    case SERVICE_V2_GAIT_ACTION_TURN_LEFT:
    case SERVICE_V2_GAIT_ACTION_TURN_RIGHT:
        if (*cycles > SERVICE_V2_GAIT_MAX_CYCLES) {
            return -EINVAL;
        }
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int submit_command(service_v2_gait_action_t action, uint32_t cycles,
                          uint8_t speed_percent, bool require_idle, bool background)
{
    if (!s_ready) {
        return -ENODEV;
    }

    int ret = normalize_command(action, &cycles);
    if (ret != 0) {
        return ret;
    }

    if (speed_percent == 0U) speed_percent = 50U;
    if (speed_percent > 100U) speed_percent = 100U;
    if (action == SERVICE_V2_GAIT_ACTION_CODEX_SQUAT && !background) return -EINVAL;
    if (background && !service_v2_background_motion_allowed()) return -EBUSY;
    gait_cmd_t cmd = {.action = action, .cycles = cycles, .speed_percent = speed_percent,
                      .background = background, .generation = service_offline_action_generation()};

    /*
     * 不阻塞投递：这个函数会被 ebus 的消息分发线程直接调到（语音识别结果就是
     * 从那里来的），在里面等队列会把整条消息总线堵住。
     *
     * pending 计数和入队必须位于同一个锁区间。xQueueSend 可能立刻唤醒
     * worker，但 worker 在 command_begin() 里需要同一把锁，所以不会看到
     * “队列已取空但 pending 尚未增加”的中间态。
     */
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);

    if (s_wake_resetting || s_wake_standing) {
        lisa_mutex_unlock(s_lock);
        return -EBUSY;
    }
    if (!s_boot_pose_authorized &&
        action != SERVICE_V2_GAIT_ACTION_ALL_OFF) {
        lisa_mutex_unlock(s_lock);
        return -EACCES;
    }

    bool roll_context = s_roll_reserved || s_status.current == SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP ||
                        s_status.current == SERVICE_V2_GAIT_ACTION_BACKFLIP ||
                        s_status.current == SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE;
    if (roll_context && action == SERVICE_V2_GAIT_ACTION_STOP) {
        /* 单次翻完可能停在反面；STOP 绝不能盲目套用正常 HOME，只能安全卸力。 */
        cmd.action = SERVICE_V2_GAIT_ACTION_ALL_OFF;
        cmd.cycles = 0U;
    }

    bool not_idle = s_status.busy || s_worker_active || s_pending_commands != 0U ||
                    uxQueueMessagesWaiting(s_queue) != 0U;

    /* current 是“正在执行/最后执行”的粘滞状态，不能拿它当活动锁；reservation
     * 已完整覆盖入队、pending、worker 和 command_end。 */
    bool normal_face_motion_context = s_normal_face_motion_reserved;
    if (normal_face_motion_context && cmd.action == SERVICE_V2_GAIT_ACTION_STOP &&
        !s_normal_face_motion_pose_validated) {
        /* IMU 正常面门尚未通过，STOP 只能保守卸力，不得盲套正常 HOME。 */
        cmd.action = SERVICE_V2_GAIT_ACTION_ALL_OFF;
        cmd.cycles = 0U;
    }
    bool normal_face_motion_terminal = normal_face_motion_context &&
                                       (cmd.action == SERVICE_V2_GAIT_ACTION_STOP ||
                                        cmd.action == SERVICE_V2_GAIT_ACTION_ALL_OFF);

    if (s_roll_reserved && cmd.action != SERVICE_V2_GAIT_ACTION_ALL_OFF) {
        /* 从入队开始即独占：action_list 同批后续命令也不能插进翻滚后面。 */
        ret = -EBUSY;
    } else if (normal_face_motion_context && cmd.action != SERVICE_V2_GAIT_ACTION_STOP &&
               cmd.action != SERVICE_V2_GAIT_ACTION_ALL_OFF) {
        /* 正常面大幅动作必须完整跑回 HOME；只允许 STOP 回 HOME 或 ALL_OFF 急停。 */
        ret = -EBUSY;
    } else if ((require_idle || cmd.action == SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP ||
                cmd.action == SERVICE_V2_GAIT_ACTION_BACKFLIP ||
                cmd.action == SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE ||
                action_is_normal_face_motion(cmd.action)) &&
               not_idle) {
        /* 翻滚、触摸蹬腿和网球发球都必须从完整 HOME 起步，不能抢占其它动作。 */
        ret = -EBUSY;
    } else if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        ret = -EBUSY;
    } else {
        s_pending_commands++;
        if (cmd.action == SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP ||
            cmd.action == SERVICE_V2_GAIT_ACTION_BACKFLIP ||
            cmd.action == SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE) {
            s_roll_reserved = true;
        }
        if (action_is_normal_face_motion(cmd.action)) {
            s_normal_face_motion_reserved = true;
            s_normal_face_motion_pose_validated = false;
        }
        if (normal_face_motion_terminal) {
            s_normal_face_motion_terminal_pending++;
        }
        ret = 0;
    }

    lisa_mutex_unlock(s_lock);

    if (ret != 0) {
        LISA_LOGW(TAG, require_idle ? "gait is not idle, dropping %s" : "gait queue full, dropping %s",
                  service_v2_gait_action_name(action));
        return -EBUSY;
    }

    LISA_LOGI(TAG, "queued %s cycles=%u speed=%u", service_v2_gait_action_name(cmd.action),
              (unsigned)cmd.cycles, (unsigned)cmd.speed_percent);
    return 0;
}

int service_v2_gait_submit_background(service_v2_gait_action_t action, uint32_t cycles)
{
    if (action != SERVICE_V2_GAIT_ACTION_IDLE_STRETCH &&
        action != SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH &&
        action != SERVICE_V2_GAIT_ACTION_PUSHUP &&
        action != SERVICE_V2_GAIT_ACTION_CODEX_SQUAT &&
        action != SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME) return -EINVAL;
    return submit_command(action, cycles, 50U, true, true);
}

int service_v2_gait_submit(service_v2_gait_action_t action, uint32_t cycles)
{
    return submit_command(action, cycles, 50U, false, false);
}

/* Worker-only wake transaction: never block the voice event dispatcher. */
int service_v2_gait_wake_pose(service_v2_gait_action_t pose, uint32_t timeout_ms)
{
    if (!s_ready) return -ENODEV;
    if (pose != SERVICE_V2_GAIT_ACTION_STAND && pose != SERVICE_V2_GAIT_ACTION_ASSEMBLY)
        return -EINVAL;
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    if (s_wake_resetting) {
        lisa_mutex_unlock(s_lock);
        return -EBUSY;
    }
    __atomic_store_n(&s_wake_resetting, true, __ATOMIC_RELEASE);
    s_wake_standing = false;
    /* Cancel old commands before starting the new pose. A command already
     * dequeued remains in pending until command_begin(), so we wait for it too. */
    int ret = 0;
    gait_cmd_t discarded;
    while (xQueueReceive(s_queue, &discarded, 0) == pdTRUE) {
        if (s_pending_commands != 0U) --s_pending_commands;
    }
    lisa_mutex_unlock(s_lock);

    ret = service_v2_actuators_servo_sweep_stop();
    uint32_t waited = 0U;
    for (;;) {
        service_v2_servo_status_t servo = {0};
        int status_ret = service_v2_actuators_servo_get_status(&servo);
        lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
        bool idle = !s_worker_active && s_pending_commands == 0U &&
                    status_ret == 0 && !servo.sweep_active;
        if (ret != 0 || idle || waited >= timeout_ms) break;
        lisa_mutex_unlock(s_lock);
        lisa_thread_mdelay(GAIT_TICK_MS);
        waited += GAIT_TICK_MS;
    }
    if (ret == 0 && (s_worker_active || s_pending_commands != 0U)) ret = -ETIMEDOUT;
    if (ret == 0) ret = service_v2_actuators_servo_disable_all();
    if (ret == 0) {
        s_roll_reserved = false;
        s_normal_face_motion_reserved = false;
        s_normal_face_motion_terminal_pending = 0U;
        s_normal_face_motion_pose_validated = false;
        gait_cmd_t stand = {.action = pose, .speed_percent = 50U};
        if (xQueueSend(s_queue, &stand, 0) != pdTRUE) ret = -EBUSY;
        else {
            ++s_pending_commands;
            s_wake_standing = true;
            __atomic_store_n(&s_boot_pose_authorized, true, __ATOMIC_RELEASE);
        }
    }
    if (ret != 0) (void)service_v2_actuators_servo_disable_all();
    __atomic_store_n(&s_wake_resetting, false, __ATOMIC_RELEASE);
    lisa_mutex_unlock(s_lock);
    LISA_LOGI(TAG, "wake pose=%s: old motions drained, wait=%ums ret=%d",
              service_v2_gait_action_name(pose), (unsigned)waited, ret);
    return ret;
}

bool service_v2_gait_has_local_pose(void)
{
    return __atomic_load_n(&s_boot_pose_authorized, __ATOMIC_ACQUIRE);
}

int service_v2_gait_submit_speed(service_v2_gait_action_t action, uint32_t cycles,
                                 uint8_t speed_percent)
{
    return submit_command(action, cycles, speed_percent, false, false);
}

int service_v2_gait_submit_if_idle(service_v2_gait_action_t action, uint32_t cycles)
{
    return submit_command(action, cycles, 50U, true, false);
}

int service_v2_gait_release_idle_pwm(uint32_t expected_generation)
{
    if (!s_ready) return -ENODEV;
    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    /* Same lock as submit/wake/command_begin: a dequeued command is still
     * represented by pending until the worker has claimed it. Never cancel
     * a frame here, and never take business/audio locks while holding gait. */
    bool busy = s_status.busy || s_worker_active || s_pending_commands != 0U ||
                s_wake_resetting || s_wake_standing || s_roll_reserved ||
                s_normal_face_motion_reserved || s_normal_face_motion_terminal_pending != 0U ||
                uxQueueMessagesWaiting(s_queue) != 0U;
    int ret = -EBUSY;
    if (!busy && power_is_usb_plugged() &&
        service_offline_action_generation() == expected_generation) {
        /* Preserve the established gait -> actuator lock order. Raw/manual
         * frames and leases are checked atomically by the actuator service. */
        ret = service_v2_actuators_servo_release_usb_idle();
    }
    lisa_mutex_unlock(s_lock);
    return ret;
}

int service_v2_gait_get_status(service_v2_gait_status_t *status)
{
    if (status == NULL) {
        return -EINVAL;
    }
    if (!s_ready) {
        memset(status, 0, sizeof(*status));
        return -ENODEV;
    }

    lisa_mutex_lock(s_lock, LISA_WAIT_FOREVER);
    *status = s_status;
    /* 对外的 busy 覆盖“已入队但 worker 尚未 command_begin”的窗口。
     * IMU 场景检测依赖它抑制动作起步冲击，不能在预留成功后
     * 还短暂对外暴露“空闲”。 */
    status->busy = s_status.busy || s_worker_active || s_pending_commands != 0U ||
                   s_wake_resetting || s_wake_standing ||
                   s_roll_reserved || s_normal_face_motion_reserved;
    /* Queued work and wake takeover are conservatively foreground until the
     * worker establishes ownership; never infer ownership from action names. */
    status->background = s_worker_active && s_background_command &&
                         s_pending_commands == 0U && !s_wake_resetting && !s_wake_standing;
    lisa_mutex_unlock(s_lock);
    return 0;
}

uint32_t service_v2_gait_default_cycles(service_v2_gait_action_t action)
{
    switch (action) {
    case SERVICE_V2_GAIT_ACTION_PUSHUP:
        return 4U;
    case SERVICE_V2_GAIT_ACTION_BACKFLIP:
        return 1U;
    case SERVICE_V2_GAIT_ACTION_FORWARD:
    case SERVICE_V2_GAIT_ACTION_BACKWARD:
        return SERVICE_V2_GAIT_DEFAULT_WALK_CYCLES;
    case SERVICE_V2_GAIT_ACTION_RUN_FORWARD:
    case SERVICE_V2_GAIT_ACTION_RUN_BACKWARD:
        return SERVICE_V2_GAIT_DEFAULT_RUN_CYCLES;
    case SERVICE_V2_GAIT_ACTION_TURN_LEFT:
    case SERVICE_V2_GAIT_ACTION_TURN_RIGHT:
        return SERVICE_V2_GAIT_DEFAULT_TURN_CYCLES;
    case SERVICE_V2_GAIT_ACTION_SPIN:
        return SERVICE_V2_GAIT_DEFAULT_SPIN_CYCLES;
    case SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE:
    case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE:
    case SERVICE_V2_GAIT_ACTION_GREETING:
    case SERVICE_V2_GAIT_ACTION_SCRATCH:
        return SERVICE_V2_GAIT_HANDSHAKE_CYCLES;
    case SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP:
        return SERVICE_V2_GAIT_DEFAULT_ROLL_CYCLES;
    default:
        return 0U;
    }
}

uint32_t service_v2_gait_cycle_ms(service_v2_gait_action_t action)
{
    switch (action) {
    case SERVICE_V2_GAIT_ACTION_FORWARD:
    case SERVICE_V2_GAIT_ACTION_BACKWARD:
        return SERVICE_V2_GAIT_WALK_CYCLE_MS;
    case SERVICE_V2_GAIT_ACTION_RUN_FORWARD:
    case SERVICE_V2_GAIT_ACTION_RUN_BACKWARD:
        return SERVICE_V2_GAIT_RUN_CYCLE_MS;
    case SERVICE_V2_GAIT_ACTION_TURN_LEFT:
    case SERVICE_V2_GAIT_ACTION_TURN_RIGHT:
    case SERVICE_V2_GAIT_ACTION_SPIN:
        return SERVICE_V2_GAIT_TURN_CYCLE_MS;
    default:
        return 0U;
    }
}

bool service_v2_gait_action_from_name(const char *name, service_v2_gait_action_t *action)
{
    if (name == NULL || action == NULL) {
        return false;
    }

    if (strcmp(name, "pushup") == 0 || strcmp(name, "push-up") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_PUSHUP;
    } else if (strcmp(name, "music") == 0 || strcmp(name, "music-beat") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_MUSIC_BEAT;
    } else if (strcmp(name, "backflip") == 0 || strcmp(name, "back-flip") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_BACKFLIP;
    } else if (strcmp(name, "stretch") == 0 || strcmp(name, "idle_stretch") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_IDLE_STRETCH;
    } else if (strcmp(name, "idle_scratch") == 0 || strcmp(name, "idle-scratch") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH;
    } else if (strcmp(name, "stand") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_STAND;
    } else if (strcmp(name, "prone") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_PRONE;
    } else if (strcmp(name, "assembly") == 0 || strcmp(name, "assemble") == 0 ||
               strcmp(name, "calibrate") == 0 || strcmp(name, "init") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_ASSEMBLY;
    } else if (strcmp(name, "standby-fold") == 0 ||
               strcmp(name, "standby_fold") == 0 ||
               strcmp(name, "sleep-fold") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_STANDBY_FOLD;
    } else if (strcmp(name, "flip_rise") == 0 || strcmp(name, "flip-rise") == 0 || strcmp(name, "fliprise") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_FLIP_RISE;
    } else if (strcmp(name, "flip") == 0 || strcmp(name, "roll") == 0 ||
               strcmp(name, "flip_once") == 0 || strcmp(name, "flip-once") == 0 ||
               strcmp(name, "full_roll") == 0 || strcmp(name, "full-roll") == 0 ||
               strcmp(name, "roll_loop") == 0 || strcmp(name, "roll-loop") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP;
    } else if (strcmp(name, "kick") == 0 || strcmp(name, "paw-kick") == 0 ||
               strcmp(name, "right-front-kick") == 0 || strcmp(name, "right_front_kick") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK;
    } else if (strcmp(name, "serve") == 0 || strcmp(name, "tennis-serve") == 0 ||
               strcmp(name, "tennis_serve") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_TENNIS_SERVE;
    } else if (strcmp(name, "forehand") == 0 || strcmp(name, "tennis-forehand") == 0 ||
               strcmp(name, "tennis_forehand") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND;
    } else if (strcmp(name, "backhand") == 0 || strcmp(name, "tennis-backhand") == 0 ||
               strcmp(name, "tennis_backhand") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND;
    } else if (strcmp(name, "double-backhand") == 0 || strcmp(name, "double_backhand") == 0 ||
               strcmp(name, "tennis-double-backhand") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND;
    } else if (strcmp(name, "left-handshake") == 0 || strcmp(name, "left_handshake") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE;
    } else if (strcmp(name, "handshake") == 0 || strcmp(name, "right-handshake") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE;
    } else if (strcmp(name, "greeting") == 0 || strcmp(name, "hello") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_GREETING;
    } else if (strcmp(name, "scratch") == 0 || strcmp(name, "itch") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_SCRATCH;
    } else if (strcmp(name, "spin") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_SPIN;
    } else if (strcmp(name, "sit") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_SIT;
    } else if (strcmp(name, "kiss") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_KISS;
    } else if (strcmp(name, "dance") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_DANCE;
    } else if (strcmp(name, "jump") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_JUMP;
    } else if (strcmp(name, "wiggle-butt") == 0 || strcmp(name, "wiggle_butt") == 0 ||
               strcmp(name, "butt-wiggle") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT;
    } else if (strcmp(name, "upside-wiggle") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE;
    } else if (strcmp(name, "off") == 0 || strcmp(name, "all_off") == 0 || strcmp(name, "all-off") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_ALL_OFF;
    } else if (strcmp(name, "fwd") == 0 || strcmp(name, "forward") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_FORWARD;
    } else if (strcmp(name, "back") == 0 || strcmp(name, "backward") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_BACKWARD;
    } else if (strcmp(name, "runfwd") == 0 || strcmp(name, "runforward") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_RUN_FORWARD;
    } else if (strcmp(name, "runback") == 0 || strcmp(name, "runbackward") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_RUN_BACKWARD;
    } else if (strcmp(name, "left") == 0 || strcmp(name, "turnleft") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_TURN_LEFT;
    } else if (strcmp(name, "right") == 0 || strcmp(name, "turnright") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_TURN_RIGHT;
    } else if (strcmp(name, "stop") == 0) {
        *action = SERVICE_V2_GAIT_ACTION_STOP;
    } else {
        return false;
    }

    return true;
}

const char *service_v2_gait_action_name(service_v2_gait_action_t action)
{
    switch (action) {
    case SERVICE_V2_GAIT_ACTION_MUSIC_BEAT:
        return "music-beat";
    case SERVICE_V2_GAIT_ACTION_PUSHUP:
        return "pushup";
    case SERVICE_V2_GAIT_ACTION_BACKFLIP:
        return "backflip";
    case SERVICE_V2_GAIT_ACTION_IDLE_STRETCH:
        return "idle_stretch";
    case SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH:
        return "idle_scratch";
    case SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME:
        return "background_home";
    case SERVICE_V2_GAIT_ACTION_STAND:
        return "stand";
    case SERVICE_V2_GAIT_ACTION_PRONE:
        return "prone";
    case SERVICE_V2_GAIT_ACTION_ASSEMBLY:
        return "assembly";
    case SERVICE_V2_GAIT_ACTION_STANDBY_FOLD:
        return "standby_fold";
    case SERVICE_V2_GAIT_ACTION_FLIP_RISE:
        return "flip_rise";
    case SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP:
        return "roll";
    case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK:
        return "right_front_kick";
    case SERVICE_V2_GAIT_ACTION_TENNIS_SERVE:
        return "tennis_serve";
    case SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND:
        return "tennis_forehand";
    case SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND:
        return "tennis_backhand";
    case SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND:
        return "tennis_double_backhand";
    case SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE:
        return "left_front_handshake";
    case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE:
        return "right_front_handshake";
    case SERVICE_V2_GAIT_ACTION_GREETING:
        return "greeting";
    case SERVICE_V2_GAIT_ACTION_SCRATCH:
        return "scratch";
    case SERVICE_V2_GAIT_ACTION_SPIN:
        return "spin";
    case SERVICE_V2_GAIT_ACTION_SIT:
        return "sit";
    case SERVICE_V2_GAIT_ACTION_KISS:
        return "kiss";
    case SERVICE_V2_GAIT_ACTION_DANCE:
        return "dance";
    case SERVICE_V2_GAIT_ACTION_JUMP:
        return "jump";
    case SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT:
        return "wiggle_butt";
    case SERVICE_V2_GAIT_ACTION_CODEX_SQUAT:
        return "codex_squat";
    case SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE:
        return "upside_wiggle";
    case SERVICE_V2_GAIT_ACTION_ALL_OFF:
        return "all_off";
    case SERVICE_V2_GAIT_ACTION_FORWARD:
        return "forward";
    case SERVICE_V2_GAIT_ACTION_BACKWARD:
        return "backward";
    case SERVICE_V2_GAIT_ACTION_RUN_FORWARD:
        return "run_forward";
    case SERVICE_V2_GAIT_ACTION_RUN_BACKWARD:
        return "run_backward";
    case SERVICE_V2_GAIT_ACTION_TURN_LEFT:
        return "turn_left";
    case SERVICE_V2_GAIT_ACTION_TURN_RIGHT:
        return "turn_right";
    case SERVICE_V2_GAIT_ACTION_STOP:
        return "stop";
    default:
        break;
    }
    return "unknown";
}
