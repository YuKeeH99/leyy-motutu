#ifndef SERVICE_VOICE_ACTION_H
#define SERVICE_VOICE_ACTION_H

#include <stdbool.h>
#include <stdint.h>

#include "service_v2_gait.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * eAgent 语音 -> 机械狗动作。
 *
 * 真实语音只走云端原生通路：ASR -> LLM -> action_list -> 设备动作。
 * eAgent 的 text_output 里 payload.type == "action_list" 时，result 是模型判定要执行的
 * 动作数组，元素形如 {"action":"DOG_RUN_FORWARD","parameters":{...}}。
 * lingxin_server 把每条元素发到 VOICE_MSG_CLOUD_ACTION_LIST，这里把控制台的下发码映射成
 * 步态动作。动作是否触发由大模型决定，端上不订阅 ASR 文本做本地关键词触发。
 *
 * service_voice_action_match() 只供 v2hw gait say 做离线词表自检，不接入真实语音链路。
 */

typedef struct {
    bool ready;
    uint32_t matched_count;              /* 云端命中并进入上层 FIFO 的次数 */
    uint32_t suppressed_count;           /* 被去抖丢掉的重复次数 */
    uint32_t cloud_action_count;         /* 云端 action_list 认出来的指令数 */
    uint32_t cloud_unknown_count;        /* 云端下发但表里没有的指令码数 */
    uint32_t queued_count;               /* 等待 gait 的动作数 */
    uint32_t queue_dropped_count;         /* 上层 FIFO 满而拒绝的动作数 */
    bool action_active;                  /* 当前有云端动作正在 gait 中执行 */
    service_v2_gait_action_t last_match; /* 最近一次命中的动作 */
} service_voice_action_status_t;

/**
 * 订阅云端 action_list。必须在 service_v2_gait_init() 之后调用。
 *
 * @return 0 成功，负的 errno 风格错误码表示失败。
 */
int service_voice_action_init(void);
/* Worker-only: invalidate cloud action/query generations before offline ownership. */
void service_voice_action_cancel_pending(void);
/* Read-only handoff barrier: false until a synchronous query start and any
 * stale-start cleanup finish. All new cloud/offline owners must wait for true. */
bool service_voice_action_query_idle(void);

/** 读取统计，用于 shell 侧确认语音链路有没有把词送到。 */
int service_voice_action_get_status(service_voice_action_status_t *status);

/**
 * 把云端指令码（控制台"具体下发指令"那一栏的值）解析成动作。
 *
 * 独立出来是为了让 shell 能不连云直接验证映射表（v2hw gait code <指令码>）。
 * 比较不区分大小写。
 *
 * @return true 认得，动作写入 action。
 */
bool service_voice_action_from_cloud_code(const char *code, service_v2_gait_action_t *action);

/**
 * 在一段中文里匹配动作关键词，仅供 shell 离线自检。
 *
 * @return true 命中，命中的动作写入 action。
 */
bool service_voice_action_match(const char *text, service_v2_gait_action_t *action);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_VOICE_ACTION_H */
