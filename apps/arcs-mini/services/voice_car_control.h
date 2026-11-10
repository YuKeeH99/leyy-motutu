#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 语音控制小车(简单关键字方案):
 * 订阅云端 ASR 识别文本(VOICE_MSG_CLOUD_IAT_START/UPDATE/END)，缓存整句，
 * 在 IAT_END 时统一匹配关键字并调用 L2 car_motion 动作。
 * 支持基础动作关键词，也支持"前进一米 / 后退半米 / 左转九十度 / 向左平移二十厘米"。
 *
 * 不走 MCP/大模型意图, 纯字符串匹配, 简单直接。
 */
void voice_car_control_init(void);
bool voice_car_control_consume_cloud_tts_suppress(void);

#ifdef __cplusplus
}
#endif
