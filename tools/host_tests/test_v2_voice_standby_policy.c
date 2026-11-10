#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "service_v2_voice_standby_policy.h"

static void test_provisioning_does_not_block_peripheral_standby(void)
{
    assert(service_v2_voice_standby_policy_resources_idle(
        false, false, false, true, false, false, false, true, false));
    assert(!service_v2_voice_standby_policy_resources_idle(
        true, false, false, true, false, false, false, true, false));
    assert(!service_v2_voice_standby_policy_resources_idle(
        false, false, false, true, false, false, false, true, true));
}

static void test_auto_timeout_waits_for_true_idle(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 1000U, 300000U);
    assert(service_v2_voice_standby_policy_poll(&policy, 300999U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(service_v2_voice_standby_policy_poll(&policy, 301000U, false, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(service_v2_voice_standby_policy_poll(&policy, 301000U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_ENTERING);
}

static void test_activity_restarts_auto_timeout(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 100U, 300000U);
    assert(service_v2_voice_standby_policy_note_activity(&policy, 250000U) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(service_v2_voice_standby_policy_poll(&policy, 300100U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(service_v2_voice_standby_policy_poll(&policy, 550000U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
}

static void test_usb_blocks_only_automatic_entry(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 0U, 300000U);
    assert(service_v2_voice_standby_policy_poll(&policy, 300000U, true, false) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE);

    service_v2_voice_standby_policy_request_manual(&policy);
    assert(service_v2_voice_standby_policy_poll(&policy, 300001U, true, false) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
}

static void test_manual_request_is_deferred_until_idle(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 0U, 300000U);
    service_v2_voice_standby_policy_request_manual(&policy);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_ENTER_PENDING);
    assert(service_v2_voice_standby_policy_poll(&policy, 100U, false, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(service_v2_voice_standby_policy_poll(&policy, 200U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
}

static void test_manual_request_survives_current_work_finishing(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 0U, 300000U);
    service_v2_voice_standby_policy_request_manual(&policy);
    assert(service_v2_voice_standby_policy_poll(&policy, 100U, false, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);

    /* Lifecycle notifications such as TTS_STOPPED and GAIT_FINISHED refresh
     * the idle timestamp but must not consume the user's five-click request. */
    assert(service_v2_voice_standby_policy_note_activity(&policy, 200U) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_ENTER_PENDING);
    assert(service_v2_voice_standby_policy_poll(&policy, 201U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
}

static void test_wakeup_during_entry_resumes_after_entry_finishes(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 0U, 300000U);
    service_v2_voice_standby_policy_request_manual(&policy);
    assert(service_v2_voice_standby_policy_poll(&policy, 1U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
    assert(service_v2_voice_standby_policy_note_activity(&policy, 2U) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(service_v2_voice_standby_policy_mark_entered(&policy) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_RESUME);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_RESUMING);
}

static void test_standby_wakeup_is_single_shot(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 0U, 300000U);
    service_v2_voice_standby_policy_request_manual(&policy);
    assert(service_v2_voice_standby_policy_poll(&policy, 1U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
    assert(service_v2_voice_standby_policy_mark_entered(&policy) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_ASLEEP);
    assert(service_v2_voice_standby_policy_note_activity(&policy, 2U) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_RESUME);
    assert(service_v2_voice_standby_policy_note_activity(&policy, 3U) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    service_v2_voice_standby_policy_mark_resumed(&policy, 4U);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE);
}

static void test_entry_failure_returns_active_and_restarts_timer(void)
{
    service_v2_voice_standby_policy_t policy;

    service_v2_voice_standby_policy_init(&policy, 0U, 300000U);
    service_v2_voice_standby_policy_request_manual(&policy);
    assert(service_v2_voice_standby_policy_poll(&policy, 1U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
    service_v2_voice_standby_policy_mark_entry_failed(&policy, 10U);
    assert(policy.state == SERVICE_V2_VOICE_STANDBY_ACTIVE);
    assert(service_v2_voice_standby_policy_poll(&policy, 300009U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_NONE);
    assert(service_v2_voice_standby_policy_poll(&policy, 300010U, true, true) ==
           SERVICE_V2_VOICE_STANDBY_ACTION_ENTER);
}

static void test_servo_release_requires_verified_zero_enabled_mask(void)
{
    assert(service_v2_voice_standby_policy_servo_release(
               0, 0, 0U, 1U, 3U) ==
           SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_DONE);
    assert(service_v2_voice_standby_policy_servo_release(
               -1, 0, 0U, 1U, 3U) ==
           SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_RETRY);
    assert(service_v2_voice_standby_policy_servo_release(
               0, -1, 0U, 1U, 3U) ==
           SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_RETRY);
    assert(service_v2_voice_standby_policy_servo_release(
               0, 0, 0x0fU, 2U, 3U) ==
           SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_RETRY);
    assert(service_v2_voice_standby_policy_servo_release(
               0, 0, 0x01U, 3U, 3U) ==
           SERVICE_V2_VOICE_STANDBY_SERVO_RELEASE_FAILED);
}

static void test_background_audio_is_suppressed_only_during_standby_transition(void)
{
    assert(!service_v2_voice_standby_policy_suppress_background_audio(
        SERVICE_V2_VOICE_STANDBY_ACTIVE));
    assert(!service_v2_voice_standby_policy_suppress_background_audio(
        SERVICE_V2_VOICE_STANDBY_ENTER_PENDING));
    assert(service_v2_voice_standby_policy_suppress_background_audio(
        SERVICE_V2_VOICE_STANDBY_ENTERING));
    assert(service_v2_voice_standby_policy_suppress_background_audio(
        SERVICE_V2_VOICE_STANDBY_ASLEEP));
    assert(service_v2_voice_standby_policy_suppress_background_audio(
        SERVICE_V2_VOICE_STANDBY_RESUMING));
}

int main(void)
{
    test_provisioning_does_not_block_peripheral_standby();
    test_auto_timeout_waits_for_true_idle();
    test_activity_restarts_auto_timeout();
    test_usb_blocks_only_automatic_entry();
    test_manual_request_is_deferred_until_idle();
    test_manual_request_survives_current_work_finishing();
    test_wakeup_during_entry_resumes_after_entry_finishes();
    test_standby_wakeup_is_single_shot();
    test_entry_failure_returns_active_and_restarts_timer();
    test_servo_release_requires_verified_zero_enabled_mask();
    test_background_audio_is_suppressed_only_during_standby_transition();
    return 0;
}
