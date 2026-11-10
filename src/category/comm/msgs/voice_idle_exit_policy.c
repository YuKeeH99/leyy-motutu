#include "voice_idle_exit_policy.h"

#include <stddef.h>

void voice_idle_exit_policy_init(voice_idle_exit_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }
    *policy = (voice_idle_exit_policy_t){0};
}

void voice_idle_exit_policy_note_activity(voice_idle_exit_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->activity_generation++;
    policy->armed = false;
}

void voice_idle_exit_policy_arm(voice_idle_exit_policy_t *policy)
{
    if (policy == NULL) {
        return;
    }
    policy->armed_generation = policy->activity_generation;
    policy->armed = true;
}

bool voice_idle_exit_policy_timeout(voice_idle_exit_policy_t *policy)
{
    if (policy == NULL || !policy->armed || policy->stop_pending ||
        policy->armed_generation != policy->activity_generation) {
        return false;
    }
    policy->armed = false;
    policy->stop_pending = true;
    policy->worker_generation = policy->armed_generation;
    return true;
}

bool voice_idle_exit_policy_worker_may_stop(const voice_idle_exit_policy_t *policy,
                                            bool session_active,
                                            bool tts_active)
{
    return policy != NULL && policy->stop_pending && session_active && !tts_active &&
           policy->worker_generation == policy->activity_generation;
}

void voice_idle_exit_policy_worker_complete(voice_idle_exit_policy_t *policy)
{
    if (policy != NULL) {
        policy->stop_pending = false;
    }
}
