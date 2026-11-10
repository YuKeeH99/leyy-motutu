#ifndef SERVICE_V2_LOW_BATTERY_POLICY_H
#define SERVICE_V2_LOW_BATTERY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERVICE_V2_LOW_BATTERY_WARNING_PERCENT       15U
#define SERVICE_V2_LOW_BATTERY_WARNING_EXIT_PERCENT  18U
#define SERVICE_V2_LOW_BATTERY_CRITICAL_PERCENT       8U
#define SERVICE_V2_LOW_BATTERY_CONFIRM_MS          5000U
#define SERVICE_V2_LOW_BATTERY_REMINDER_INTERVAL_MS 120000U

typedef enum {
    SERVICE_V2_LOW_BATTERY_ACTION_NONE = 0,
    SERVICE_V2_LOW_BATTERY_ACTION_REMIND,
    SERVICE_V2_LOW_BATTERY_ACTION_SHUTDOWN,
} service_v2_low_battery_action_t;

typedef struct {
    bool warning_confirming;
    bool warning_active;
    bool critical_confirming;
    bool shutdown_requested;
    bool reminder_played;
    uint32_t warning_since_ms;
    uint32_t critical_since_ms;
    uint32_t last_reminder_ms;
} service_v2_low_battery_policy_t;

void service_v2_low_battery_policy_init(service_v2_low_battery_policy_t *policy);

/**
 * Evaluate one filtered battery sample.
 *
 * Warning reminders require reminder_allowed; critical shutdown deliberately
 * does not, so it remains effective while voice standby is active.
 */
service_v2_low_battery_action_t service_v2_low_battery_policy_poll(
    service_v2_low_battery_policy_t *policy,
    uint32_t now_ms,
    uint8_t level,
    bool has_battery,
    bool external_power,
    bool reminder_allowed);

void service_v2_low_battery_policy_mark_reminded(
    service_v2_low_battery_policy_t *policy, uint32_t now_ms);
void service_v2_low_battery_policy_mark_shutdown_requested(
    service_v2_low_battery_policy_t *policy);

#ifdef __cplusplus
}
#endif

#endif
