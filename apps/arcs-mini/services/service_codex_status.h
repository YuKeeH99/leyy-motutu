#pragma once

#include "service_v2_rgb_effect_policy.h"

int service_codex_status_init(void);
void service_codex_status_toggle_mode(void);
void service_codex_status_apply_color(service_v2_rgb_color_t *color);
bool service_codex_status_feedback_active(void);
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
bool service_codex_status_office_mode(void);
#endif
#ifdef CONFIG_APP_CODEX_STATUS_AGENT_VOICE
bool service_codex_status_notice_active(void);
#endif
