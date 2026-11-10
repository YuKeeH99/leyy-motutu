#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "service_v2_low_battery_policy.h"

static service_v2_low_battery_action_t poll_at(
    service_v2_low_battery_policy_t *policy,
    uint32_t now_ms,
    uint8_t level,
    bool external_power,
    bool reminder_allowed)
{
    return service_v2_low_battery_policy_poll(policy, now_ms, level, true,
                                              external_power,
                                              reminder_allowed);
}

static void warning_requires_five_stable_seconds_below_fifteen(void)
{
    service_v2_low_battery_policy_t policy;
    service_v2_low_battery_policy_init(&policy);

    assert(poll_at(&policy, 0U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 4999U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 5000U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_REMIND);
}

static void warning_waits_until_idle_and_repeats_after_two_minutes(void)
{
    service_v2_low_battery_policy_t policy;
    service_v2_low_battery_policy_init(&policy);

    assert(poll_at(&policy, 0U, 14U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 5000U, 14U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 6000U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_REMIND);
    service_v2_low_battery_policy_mark_reminded(&policy, 6000U);

    assert(poll_at(&policy, 125999U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 126000U, 14U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 127000U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_REMIND);
}

static void warning_hysteresis_prevents_threshold_chatter(void)
{
    service_v2_low_battery_policy_t policy;
    service_v2_low_battery_policy_init(&policy);

    assert(poll_at(&policy, 0U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 5000U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_REMIND);
    service_v2_low_battery_policy_mark_reminded(&policy, 5000U);

    assert(poll_at(&policy, 6000U, 16U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(policy.warning_active);
    assert(poll_at(&policy, 7000U, 18U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(!policy.warning_active);

    service_v2_low_battery_policy_init(&policy);
    assert(poll_at(&policy, 0U, 17U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(!policy.warning_active);
}

static void sleeping_suppresses_warning_but_not_critical_shutdown(void)
{
    service_v2_low_battery_policy_t policy;
    service_v2_low_battery_policy_init(&policy);

    assert(poll_at(&policy, 0U, 14U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 6000U, 14U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);

    assert(poll_at(&policy, 7000U, 7U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 12000U, 7U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_SHUTDOWN);
    service_v2_low_battery_policy_mark_shutdown_requested(&policy);
    assert(poll_at(&policy, 13000U, 7U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
}

static void exactly_eight_percent_does_not_shutdown(void)
{
    service_v2_low_battery_policy_t policy;
    service_v2_low_battery_policy_init(&policy);

    assert(poll_at(&policy, 0U, 8U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 10000U, 8U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_REMIND);
}

static void critical_confirmation_suppresses_ordinary_reminder(void)
{
    service_v2_low_battery_policy_t policy;
    service_v2_low_battery_policy_init(&policy);

    assert(poll_at(&policy, 0U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 5000U, 14U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_REMIND);
    service_v2_low_battery_policy_mark_reminded(&policy, 5000U);

    assert(poll_at(&policy, 125000U, 7U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 130000U, 7U, false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_SHUTDOWN);
}

static void charging_or_missing_battery_clears_all_low_power_state(void)
{
    service_v2_low_battery_policy_t policy;
    service_v2_low_battery_policy_init(&policy);

    assert(poll_at(&policy, 0U, 7U, false, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(poll_at(&policy, 4000U, 7U, true, false) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(!policy.warning_active);
    assert(!policy.critical_confirming);

    assert(service_v2_low_battery_policy_poll(&policy, 5000U, 1U, false,
                                              false, true) ==
           SERVICE_V2_LOW_BATTERY_ACTION_NONE);
    assert(!policy.warning_active);
}

int main(void)
{
    warning_requires_five_stable_seconds_below_fifteen();
    warning_waits_until_idle_and_repeats_after_two_minutes();
    warning_hysteresis_prevents_threshold_chatter();
    sleeping_suppresses_warning_but_not_critical_shutdown();
    exactly_eight_percent_does_not_shutdown();
    critical_confirmation_suppresses_ordinary_reminder();
    charging_or_missing_battery_clears_all_low_power_state();
    return 0;
}
