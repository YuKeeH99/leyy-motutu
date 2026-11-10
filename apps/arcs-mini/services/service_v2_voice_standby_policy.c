#include "service_v2_voice_standby_policy.h"

#include <stddef.h>

bool service_v2_voice_standby_policy_resources_idle(bool chat_active,
                                                     bool tts_active,
                                                     bool audio_active,
                                                     bool provisioning_active,
                                                     bool alarm_active,
                                                     bool ota_busy,
                                                     bool music_active,
                                                     bool gait_ready,
                                                     bool gait_busy)
{
    /* Wi-Fi/BLE provisioning stays operational while only the user-facing
     * peripherals sleep. */
    (void)provisioning_active;

    return !chat_active && !tts_active && !audio_active && !alarm_active &&
           !ota_busy && !music_active && gait_ready && !gait_busy;
}

bool service_v2_voice_standby_policy_suppress_background_audio(
    service_v2_voice_standby_state_t state)
{
    return state == SERVICE_V2_VOICE_STANDBY_ENTERING ||
           state == SERVICE_V2_VOICE_STANDBY_ASLEEP ||
           state == SERVICE_V2_VOICE_STANDBY_RESUMING;
}

void service_v2_voice_standby_policy_init(service_v2_voice_standby_policy_t *policy,
                                          uint32_t now_ms,
                                          uint32_t timeout_ms)
{
    if (policy == NULL) {
        return;
    }

    policy->state = SERVICE_V2_VOICE_STANDBY_ACTIVE;
    policy->last_activity_ms = now_ms;
    policy->timeout_ms = timeout_ms;
    policy->resume_after_enter = false;
}

void service_v2_voice_standby_policy_request_manual(service_v2_voice_standby_policy_t *policy)
{
    if (policy == NULL || policy->state != SERVICE_V2_VOICE_STANDBY_ACTIVE) {
        return;
    }

    policy->state = SERVICE_V2_VOICE_STANDBY_ENTER_PENDING;
}

service_v2_voice_standby_action_t service_v2_voice_standby_policy_poll(
    service_v2_voice_standby_policy_t *policy, uint32_t now_ms, bool eligible,
    bool automatic_entry_allowed)
{
    if (policy == NULL) {
        return SERVICE_V2_VOICE_STANDBY_ACTION_NONE;
    }

    if (policy->state == SERVICE_V2_VOICE_STANDBY_ACTIVE && eligible && automatic_entry_allowed &&
        (uint32_t)(now_ms - policy->last_activity_ms) >= policy->timeout_ms) {
        policy->state = SERVICE_V2_VOICE_STANDBY_ENTERING;
        return SERVICE_V2_VOICE_STANDBY_ACTION_ENTER;
    }

    if (policy->state == SERVICE_V2_VOICE_STANDBY_ENTER_PENDING && eligible) {
        policy->state = SERVICE_V2_VOICE_STANDBY_ENTERING;
        return SERVICE_V2_VOICE_STANDBY_ACTION_ENTER;
    }

    return SERVICE_V2_VOICE_STANDBY_ACTION_NONE;
}

service_v2_voice_standby_action_t service_v2_voice_standby_policy_note_activity(
    service_v2_voice_standby_policy_t *policy, uint32_t now_ms)
{
    if (policy == NULL) {
        return SERVICE_V2_VOICE_STANDBY_ACTION_NONE;
    }

    policy->last_activity_ms = now_ms;
    if (policy->state == SERVICE_V2_VOICE_STANDBY_ASLEEP) {
        policy->state = SERVICE_V2_VOICE_STANDBY_RESUMING;
        return SERVICE_V2_VOICE_STANDBY_ACTION_RESUME;
    }
    if (policy->state == SERVICE_V2_VOICE_STANDBY_ENTERING) {
        policy->resume_after_enter = true;
    }

    return SERVICE_V2_VOICE_STANDBY_ACTION_NONE;
}

service_v2_voice_standby_action_t service_v2_voice_standby_policy_mark_entered(
    service_v2_voice_standby_policy_t *policy)
{
    if (policy == NULL || policy->state != SERVICE_V2_VOICE_STANDBY_ENTERING) {
        return SERVICE_V2_VOICE_STANDBY_ACTION_NONE;
    }

    if (policy->resume_after_enter) {
        policy->resume_after_enter = false;
        policy->state = SERVICE_V2_VOICE_STANDBY_RESUMING;
        return SERVICE_V2_VOICE_STANDBY_ACTION_RESUME;
    }

    policy->state = SERVICE_V2_VOICE_STANDBY_ASLEEP;
    return SERVICE_V2_VOICE_STANDBY_ACTION_NONE;
}

void service_v2_voice_standby_policy_mark_entry_failed(service_v2_voice_standby_policy_t *policy,
                                                       uint32_t now_ms)
{
    if (policy == NULL) {
        return;
    }

    policy->state = SERVICE_V2_VOICE_STANDBY_ACTIVE;
    policy->last_activity_ms = now_ms;
    policy->resume_after_enter = false;
}

void service_v2_voice_standby_policy_mark_resumed(service_v2_voice_standby_policy_t *policy,
                                                  uint32_t now_ms)
{
    if (policy == NULL) {
        return;
    }

    policy->state = SERVICE_V2_VOICE_STANDBY_ACTIVE;
    policy->last_activity_ms = now_ms;
    policy->resume_after_enter = false;
}

service_v2_voice_standby_servo_release_t
service_v2_voice_standby_policy_servo_release(int disable_result,
                                              int status_result,
                                              uint8_t enabled_mask,
                                              uint32_t attempt,
                                              uint32_t max_attempts)
{
    if (disable_result == 0 && status_result == 0 && enabled_mask == 0U) {
        return SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_DONE;
    }
    if (attempt < max_attempts) {
        return SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_RETRY;
    }
    return SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_FAILED;
}
