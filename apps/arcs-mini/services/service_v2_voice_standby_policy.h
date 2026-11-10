#ifndef SERVICE_V2_VOICE_STANDBY_POLICY_H
#define SERVICE_V2_VOICE_STANDBY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SERVICE_V2_VOICE_STANDBY_ACTIVE = 0,
    SERVICE_V2_VOICE_STANDBY_ENTER_PENDING,
    SERVICE_V2_VOICE_STANDBY_ENTERING,
    SERVICE_V2_VOICE_STANDBY_ASLEEP,
    SERVICE_V2_VOICE_STANDBY_RESUMING,
} service_v2_voice_standby_state_t;

typedef enum {
    SERVICE_V2_VOICE_STANDBY_ACTION_NONE = 0,
    SERVICE_V2_VOICE_STANDBY_ACTION_ENTER,
    SERVICE_V2_VOICE_STANDBY_ACTION_RESUME,
} service_v2_voice_standby_action_t;

typedef enum {
    SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_DONE = 0,
    SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_RETRY,
    SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_FAILED,
} service_v2_voice_standby_servo_release_t;

typedef struct {
    service_v2_voice_standby_state_t state;
    uint32_t last_activity_ms;
    uint32_t timeout_ms;
    bool resume_after_enter;
} service_v2_voice_standby_policy_t;

/**
 * Return whether user-facing peripherals may enter standby.
 *
 * An active portal alone is not busy. The coordinator pauses provisioning
 * and waits for in-flight operations before it may switch the radio off.
 */
bool service_v2_voice_standby_policy_resources_idle(bool chat_active,
                                                     bool tts_active,
                                                     bool audio_active,
                                                     bool provisioning_active,
                                                     bool alarm_active,
                                                     bool ota_busy,
                                                     bool music_active,
                                                     bool gait_ready,
                                                     bool gait_busy);

/** Suppress asynchronous background prompts while peripherals are sleeping. */
bool service_v2_voice_standby_policy_suppress_background_audio(
    service_v2_voice_standby_state_t state);

void service_v2_voice_standby_policy_init(service_v2_voice_standby_policy_t *policy,
                                          uint32_t now_ms,
                                          uint32_t timeout_ms);
void service_v2_voice_standby_policy_request_manual(service_v2_voice_standby_policy_t *policy);
service_v2_voice_standby_action_t service_v2_voice_standby_policy_poll(
    service_v2_voice_standby_policy_t *policy, uint32_t now_ms, bool eligible,
    bool automatic_entry_allowed);
service_v2_voice_standby_action_t service_v2_voice_standby_policy_note_activity(
    service_v2_voice_standby_policy_t *policy, uint32_t now_ms);
service_v2_voice_standby_action_t service_v2_voice_standby_policy_mark_entered(
    service_v2_voice_standby_policy_t *policy);
void service_v2_voice_standby_policy_mark_entry_failed(service_v2_voice_standby_policy_t *policy,
                                                       uint32_t now_ms);
void service_v2_voice_standby_policy_mark_resumed(service_v2_voice_standby_policy_t *policy,
                                                  uint32_t now_ms);
service_v2_voice_standby_servo_release_t
service_v2_voice_standby_policy_servo_release(int disable_result,
                                              int status_result,
                                              uint8_t enabled_mask,
                                              uint32_t attempt,
                                              uint32_t max_attempts);

#endif
