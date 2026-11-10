#include "service_v2_low_battery_policy.h"

#include <stddef.h>
#include <string.h>

static void low_battery_policy_clear(service_v2_low_battery_policy_t *policy)
{
    memset(policy, 0, sizeof(*policy));
}

void service_v2_low_battery_policy_init(service_v2_low_battery_policy_t *policy)
{
    if (policy != NULL) {
        low_battery_policy_clear(policy);
    }
}

service_v2_low_battery_action_t service_v2_low_battery_policy_poll(
    service_v2_low_battery_policy_t *policy,
    uint32_t now_ms,
    uint8_t level,
    bool has_battery,
    bool external_power,
    bool reminder_allowed)
{
    if (policy == NULL) {
        return SERVICE_V2_LOW_BATTERY_ACTION_NONE;
    }

    if (!has_battery || external_power) {
        low_battery_policy_clear(policy);
        return SERVICE_V2_LOW_BATTERY_ACTION_NONE;
    }

    if (level < SERVICE_V2_LOW_BATTERY_CRITICAL_PERCENT) {
        if (!policy->critical_confirming) {
            policy->critical_confirming = true;
            policy->critical_since_ms = now_ms;
        } else if (!policy->shutdown_requested &&
                   (uint32_t)(now_ms - policy->critical_since_ms) >=
                       SERVICE_V2_LOW_BATTERY_CONFIRM_MS) {
            return SERVICE_V2_LOW_BATTERY_ACTION_SHUTDOWN;
        }
        /* Do not insert the ordinary 15% reminder immediately before the
         * dedicated critical shutdown prompt. */
        return SERVICE_V2_LOW_BATTERY_ACTION_NONE;
    } else {
        policy->critical_confirming = false;
        policy->critical_since_ms = 0U;
    }

    if (!policy->warning_active) {
        if (level < SERVICE_V2_LOW_BATTERY_WARNING_PERCENT) {
            if (!policy->warning_confirming) {
                policy->warning_confirming = true;
                policy->warning_since_ms = now_ms;
            } else if ((uint32_t)(now_ms - policy->warning_since_ms) >=
                       SERVICE_V2_LOW_BATTERY_CONFIRM_MS) {
                policy->warning_active = true;
                policy->warning_confirming = false;
            }
        } else {
            policy->warning_confirming = false;
            policy->warning_since_ms = 0U;
        }
    } else if (level >= SERVICE_V2_LOW_BATTERY_WARNING_EXIT_PERCENT) {
        policy->warning_active = false;
        policy->warning_confirming = false;
        policy->reminder_played = false;
        policy->last_reminder_ms = 0U;
    }

    if (!policy->warning_active || !reminder_allowed) {
        return SERVICE_V2_LOW_BATTERY_ACTION_NONE;
    }

    if (!policy->reminder_played ||
        (uint32_t)(now_ms - policy->last_reminder_ms) >=
            SERVICE_V2_LOW_BATTERY_REMINDER_INTERVAL_MS) {
        return SERVICE_V2_LOW_BATTERY_ACTION_REMIND;
    }

    return SERVICE_V2_LOW_BATTERY_ACTION_NONE;
}

void service_v2_low_battery_policy_mark_reminded(
    service_v2_low_battery_policy_t *policy, uint32_t now_ms)
{
    if (policy == NULL) {
        return;
    }

    policy->reminder_played = true;
    policy->last_reminder_ms = now_ms;
}

void service_v2_low_battery_policy_mark_shutdown_requested(
    service_v2_low_battery_policy_t *policy)
{
    if (policy != NULL) {
        policy->shutdown_requested = true;
    }
}
