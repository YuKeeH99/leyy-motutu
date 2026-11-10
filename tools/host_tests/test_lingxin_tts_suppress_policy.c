#include <assert.h>

#include "lingxin_tts_suppress_policy.h"

int main(void)
{
    lingxin_tts_suppress_policy_t policy = {0};

    lingxin_tts_suppress_new_response(&policy);
    assert(!lingxin_tts_suppress_is_active(&policy));
    assert(!lingxin_tts_suppress_should_drop_audio(&policy));

    lingxin_tts_suppress_request(&policy);
    assert(lingxin_tts_suppress_is_active(&policy));
    assert(lingxin_tts_suppress_should_drop_audio(&policy));

    /* End-of-audio must not reopen the same response. */
    assert(lingxin_tts_suppress_should_drop_audio(&policy));

    lingxin_tts_suppress_new_response(&policy);
    assert(!lingxin_tts_suppress_is_active(&policy));
    assert(!lingxin_tts_suppress_should_drop_audio(&policy));
    return 0;
}
