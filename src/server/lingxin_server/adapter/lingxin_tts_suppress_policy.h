#pragma once

#include <stdbool.h>

typedef struct {
    volatile bool active;
} lingxin_tts_suppress_policy_t;

void lingxin_tts_suppress_new_response(lingxin_tts_suppress_policy_t *policy);
void lingxin_tts_suppress_request(lingxin_tts_suppress_policy_t *policy);
bool lingxin_tts_suppress_is_active(const lingxin_tts_suppress_policy_t *policy);
bool lingxin_tts_suppress_should_drop_audio(const lingxin_tts_suppress_policy_t *policy);
