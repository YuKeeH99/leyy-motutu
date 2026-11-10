#include "lingxin_tts_suppress_policy.h"

#include <stddef.h>

void lingxin_tts_suppress_new_response(lingxin_tts_suppress_policy_t *policy)
{
    if (policy != NULL) {
        policy->active = false;
    }
}

void lingxin_tts_suppress_request(lingxin_tts_suppress_policy_t *policy)
{
    if (policy != NULL) {
        policy->active = true;
    }
}

bool lingxin_tts_suppress_is_active(const lingxin_tts_suppress_policy_t *policy)
{
    return policy != NULL && policy->active;
}

bool lingxin_tts_suppress_should_drop_audio(const lingxin_tts_suppress_policy_t *policy)
{
    return lingxin_tts_suppress_is_active(policy);
}
