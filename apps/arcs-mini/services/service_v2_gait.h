#ifndef SERVICE_V2_GAIT_H
#define SERVICE_V2_GAIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 四足步态播放器。
 *
 * 关键帧数据来自 Motion Studio 上逐帧手调、整机实测通过的 V5 动作组
 * （tools/QUADRUPED_MOTION_STUDIO.md 里的调试流程）。原来这些帧存在主机侧的
 * 工程 JSON 里，靠一条 ADB shell 逐帧下发，单次往返 43.3 ms，一个步态周期被
 * 硬顶在 1200 ms。搬进固件之后由板上 20 ms 插值器直接跑，主机和 USB 都不再
 * 参与，语音链路也就能直接驱动它。
 *
 * V2.1 实际接线 CH0..CH3 = 左前、右前、右后、左后。
 * 历史帧/宏的 RL、RR 名称沿用旧标签，查物理腿位应以通道号为准。
 * 翻面只会改变四条腿在“当前世界坐标”里的前后职责，绝不能重排通道。
 */

typedef enum {
    SERVICE_V2_GAIT_ACTION_STOP = 0,     /* 停止移动并平滑回 Home，保持扭矩 */
    SERVICE_V2_GAIT_ACTION_STAND,        /* 起立：回到逐腿标定的 Home，站稳后保持 */
    SERVICE_V2_GAIT_ACTION_PRONE,        /* 趴下：四腿 52.5° 贴地，到位后卸力 */
    SERVICE_V2_GAIT_ACTION_FORWARD,      /* 慢速前进 */
    SERVICE_V2_GAIT_ACTION_BACKWARD,     /* 慢速后退 */
    SERVICE_V2_GAIT_ACTION_RUN_FORWARD,  /* 快速前进 */
    SERVICE_V2_GAIT_ACTION_RUN_BACKWARD, /* 快速后退 */
    SERVICE_V2_GAIT_ACTION_TURN_LEFT,    /* 原地左转 */
    SERVICE_V2_GAIT_ACTION_TURN_RIGHT,   /* 原地右转 */
    SERVICE_V2_GAIT_ACTION_FLIP_RISE,    /* 起翻：三拍起身到约 80°，末尾保持 */
    SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP, /* IMU 自动判面；1=倒立表演，2=翻跟头/前空翻；轨迹不变 */
    SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_KICK, /* 正常面右前腿前送并快速蹬六次，结束回 Home */
    SERVICE_V2_GAIT_ACTION_TENNIS_SERVE, /* 正常面网球发球：蓄力、抬臂、挥拍一次并回 Home */
    SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND,
    SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND,
    SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND,
    SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE,
    SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE,
    SERVICE_V2_GAIT_ACTION_GREETING,
    SERVICE_V2_GAIT_ACTION_SCRATCH,
    SERVICE_V2_GAIT_ACTION_SPIN,
    SERVICE_V2_GAIT_ACTION_SIT,
    SERVICE_V2_GAIT_ACTION_KISS,
    SERVICE_V2_GAIT_ACTION_DANCE,
    SERVICE_V2_GAIT_ACTION_JUMP,
    SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT,
    SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE,
    SERVICE_V2_GAIT_ACTION_ALL_OFF,      /* 急停：立即切断四路 PWM，不回姿态 */
    SERVICE_V2_GAIT_ACTION_ASSEMBLY,     /* 装配复位：四腿水平伸直并保持扭矩 */
    SERVICE_V2_GAIT_ACTION_STANDBY_FOLD, /* 休眠收腿：站立、后腿蹲下、前腿向前折叠 */
    SERVICE_V2_GAIT_ACTION_IDLE_STRETCH, /* 空闲伸懒腰：后腿轻弯、双前腿前伸摆动、回站姿 */
    SERVICE_V2_GAIT_ACTION_BACKGROUND_HOME, /* 后台动作打断后，仅在再次空闲时收腿 */
    SERVICE_V2_GAIT_ACTION_PUSHUP,       /* 俯卧撑：后腿后伸，前腿屈伸 3~5 次 */
    SERVICE_V2_GAIT_ACTION_BACKFLIP,     /* 从正常站姿向后完整一圈；无 IMU 起步/落地门，保留电源保护 */
    SERVICE_V2_GAIT_ACTION_MUSIC_BEAT,   /* 131_music.mp3 单次播放，前脚轻点拍 */
    SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH, /* 坐姿随机前爪：用户指定 CH0 800/1100 或 CH1 2200/1900 us 往返五次 */
    SERVICE_V2_GAIT_ACTION_CODEX_SQUAT,   /* Codex: small single squat */
} service_v2_gait_action_t;

/*
 * 三种移动周期都由 service_v2_gait.c 里的 _Static_assert 和关键帧数钉死。
 * 放在头里是给语音侧用的：云端的 TIME 参数要按具体动作换算周期数。
 */
#define SERVICE_V2_GAIT_WALK_CYCLE_MS 640U
#define SERVICE_V2_GAIT_RUN_CYCLE_MS  320U
#define SERVICE_V2_GAIT_TURN_CYCLE_MS 600U

/* 起翻固定时间轴从 HOME 帧开始到最后一帧到位，末尾不回 HOME。 */
#define SERVICE_V2_GAIT_FLIP_RISE_DURATION_MS 2430U

/* 各类语音命令默认均执行约 5~6 s。 */
#define SERVICE_V2_GAIT_DEFAULT_WALK_CYCLES 9U
#define SERVICE_V2_GAIT_DEFAULT_RUN_CYCLES  18U
#define SERVICE_V2_GAIT_DEFAULT_TURN_CYCLES 8U
#define SERVICE_V2_GAIT_DEFAULT_ROLL_CYCLES 1U
#define SERVICE_V2_GAIT_MAX_ROLL_CYCLES     2U
#define SERVICE_V2_GAIT_RIGHT_FRONT_KICK_CYCLES 6U
#define SERVICE_V2_GAIT_TENNIS_SERVE_CYCLES     1U
#define SERVICE_V2_GAIT_HANDSHAKE_CYCLES        3U
#define SERVICE_V2_GAIT_WIGGLE_BUTT_CYCLES      3U
#define SERVICE_V2_GAIT_UPSIDE_WIGGLE_CYCLES    3U
#define SERVICE_V2_GAIT_DEFAULT_SPIN_CYCLES    20U

/* 走行周期上限，防止一条命令让它无限跑下去。0 表示持续到下一条命令。 */
#define SERVICE_V2_GAIT_MAX_CYCLES 200U

typedef struct {
    bool ready;
    bool busy;                        /* 关键帧正在运行，或动作已入队/预留 */
    service_v2_gait_action_t current; /* 正在执行/最后一次执行的动作 */
    uint32_t cycles_done;             /* 本次走行已完成的周期数 */
    int last_error;                   /* 最近一次帧下发的返回值 */
    bool background; /* Running worker owns a background action, without a pending takeover. */
} service_v2_gait_status_t;

typedef struct {
    service_v2_gait_action_t action;
    int result;
    bool background; /* Autonomous/touch movement must not postpone standby. */
} service_v2_gait_action_finished_t;

/**
 * 创建播放线程。必须在 service_v2_actuators_init() 之后调用。
 *
 * @return 0 成功，负的 errno 风格错误码表示失败。
 */
int service_v2_gait_init(void);
/** Accepted local input only; drain old actions then stand/reset. Worker context. */
int service_v2_gait_wake_pose(service_v2_gait_action_t pose, uint32_t timeout_ms);
bool service_v2_gait_has_local_pose(void);

/**
 * 投递一个动作。
 *
 * 非阻塞：命令进队列后立即返回，实际关键帧由播放线程执行。新命令会抢占正在
 * 播放的动作（当前帧插值到一半也会被下一帧无缝接管，因为固件每次起帧都以当前
 * 脉宽为起点）。
 *
 * @param action 目标动作。
 * @param cycles 对前后走、前后跑、左右转和翻滚有意义；翻滚按完成的 180°
 *               半圈计数，只接受 1（单翻）或 2（连续翻后回到起始面并停止）。
 *               姿态动作和旧起翻调试动作忽略该参数。
 * @return 0 成功入队，-ENODEV 未初始化，-EINVAL 参数非法，-EBUSY 队列满。
 */
int service_v2_gait_submit(service_v2_gait_action_t action, uint32_t cycles);
int service_v2_gait_submit_background(service_v2_gait_action_t action, uint32_t cycles);
int service_v2_gait_submit_speed(service_v2_gait_action_t action, uint32_t cycles,
                                 uint8_t speed_percent);

/**
 * 仅在播放器完全空闲时原子投递一个动作。
 *
 * 空闲判断和入队受同一把内部锁保护，并覆盖 worker 已取走命令但尚未
 * 更新 busy 的窗口。适合触摸、按键等可能重复上报的边沿触发源；它不会
 * 抢占已有动作。
 *
 * @return 0 成功入队，-ENODEV 未初始化，-EINVAL 参数非法，-EBUSY 非空闲。
 */
int service_v2_gait_submit_if_idle(service_v2_gait_action_t action, uint32_t cycles);

/** USB power saving after the caller's business-idle check. Pass the offline
 * generation captured before that check. Atomically rejects newer wakes,
 * queued/active gait work and raw servo owners; never moves or cancels a frame.
 * Returns 0 if PWM is off, -EBUSY if owned/USB removed, or a driver error. */
int service_v2_gait_release_idle_pwm(uint32_t expected_generation);

/** 读取播放器状态。 */
int service_v2_gait_get_status(service_v2_gait_status_t *status);

/** 返回动作的默认周期数；姿态/STOP 返回 0。 */
uint32_t service_v2_gait_default_cycles(service_v2_gait_action_t action);

/** 返回移动动作的单周期时长；姿态/STOP 返回 0。 */
uint32_t service_v2_gait_cycle_ms(service_v2_gait_action_t action);

/** 把动作名解析成枚举，供 shell 和语音关键词共用。 */
bool service_v2_gait_action_from_name(const char *name, service_v2_gait_action_t *action);

/** 动作枚举的可读名，用于日志。 */
const char *service_v2_gait_action_name(service_v2_gait_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_V2_GAIT_H */
