#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "voice_idle_exit_policy.h"

int main(void)
{
    voice_idle_exit_policy_t policy;

    voice_idle_exit_policy_init(&policy);
    voice_idle_exit_policy_arm(&policy);
    voice_idle_exit_policy_note_activity(&policy);
    assert(!voice_idle_exit_policy_timeout(&policy));

    voice_idle_exit_policy_arm(&policy);
    assert(voice_idle_exit_policy_timeout(&policy));
    assert(voice_idle_exit_policy_worker_may_stop(&policy, true, false));
    voice_idle_exit_policy_worker_complete(&policy);

    voice_idle_exit_policy_arm(&policy);
    assert(voice_idle_exit_policy_timeout(&policy));
    voice_idle_exit_policy_note_activity(&policy);
    assert(!voice_idle_exit_policy_worker_may_stop(&policy, true, false));
    voice_idle_exit_policy_worker_complete(&policy);

    voice_idle_exit_policy_arm(&policy);
    assert(voice_idle_exit_policy_timeout(&policy));
    assert(!voice_idle_exit_policy_worker_may_stop(&policy, false, false));
    assert(!voice_idle_exit_policy_worker_may_stop(&policy, true, true));

    return 0;
}
