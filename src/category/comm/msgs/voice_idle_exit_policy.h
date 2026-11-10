#ifndef VOICE_IDLE_EXIT_POLICY_H
#define VOICE_IDLE_EXIT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t activity_generation;
    uint32_t armed_generation;
    uint32_t worker_generation;
    bool armed;
    bool stop_pending;
} voice_idle_exit_policy_t;

void voice_idle_exit_policy_init(voice_idle_exit_policy_t *policy);
void voice_idle_exit_policy_note_activity(voice_idle_exit_policy_t *policy);
void voice_idle_exit_policy_arm(voice_idle_exit_policy_t *policy);
bool voice_idle_exit_policy_timeout(voice_idle_exit_policy_t *policy);
bool voice_idle_exit_policy_worker_may_stop(const voice_idle_exit_policy_t *policy,
                                            bool session_active,
                                            bool tts_active);
void voice_idle_exit_policy_worker_complete(voice_idle_exit_policy_t *policy);

#endif
