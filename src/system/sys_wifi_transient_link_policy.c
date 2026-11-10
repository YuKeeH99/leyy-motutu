#include "sys_wifi_transient_link_policy.h"

#include <stddef.h>

void sys_wifi_transient_link_policy_init(sys_wifi_transient_link_policy_t *policy)
{
    if (policy != NULL) {
        policy->grace_pending = false;
    }
}

sys_wifi_transient_link_decision_t sys_wifi_transient_link_on_disconnect(
    sys_wifi_transient_link_policy_t *policy,
    bool link_was_connected,
    bool transient_error)
{
    if (policy == NULL) {
        return SYS_WIFI_TRANSIENT_LINK_PASS_THROUGH;
    }
    if (policy->grace_pending) {
        return SYS_WIFI_TRANSIENT_LINK_IGNORE_DURING_GRACE;
    }
    if (!link_was_connected || !transient_error) {
        return SYS_WIFI_TRANSIENT_LINK_PASS_THROUGH;
    }
    policy->grace_pending = true;
    return SYS_WIFI_TRANSIENT_LINK_START_GRACE;
}

sys_wifi_transient_link_decision_t sys_wifi_transient_link_on_connect(
    sys_wifi_transient_link_policy_t *policy)
{
    if (policy == NULL || !policy->grace_pending) {
        return SYS_WIFI_TRANSIENT_LINK_PASS_THROUGH;
    }
    policy->grace_pending = false;
    return SYS_WIFI_TRANSIENT_LINK_RECOVERED;
}

bool sys_wifi_transient_link_on_timeout(sys_wifi_transient_link_policy_t *policy)
{
    if (policy == NULL || !policy->grace_pending) {
        return false;
    }
    policy->grace_pending = false;
    return true;
}

void sys_wifi_transient_link_cancel(sys_wifi_transient_link_policy_t *policy)
{
    if (policy != NULL) {
        policy->grace_pending = false;
    }
}
