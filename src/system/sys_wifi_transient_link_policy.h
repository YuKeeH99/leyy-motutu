#ifndef SYS_WIFI_TRANSIENT_LINK_POLICY_H
#define SYS_WIFI_TRANSIENT_LINK_POLICY_H

#include <stdbool.h>

/* Same-AP reassociation on the field unit has taken about six seconds.
 * Preserve DHCP, netif and the WebSocket long enough for that recovery. */
#define SYS_WIFI_TRANSIENT_LINK_GRACE_MS 15000U

typedef enum {
    SYS_WIFI_TRANSIENT_LINK_PASS_THROUGH = 0,
    SYS_WIFI_TRANSIENT_LINK_START_GRACE,
    SYS_WIFI_TRANSIENT_LINK_IGNORE_DURING_GRACE,
    SYS_WIFI_TRANSIENT_LINK_RECOVERED,
} sys_wifi_transient_link_decision_t;

typedef struct {
    bool grace_pending;
} sys_wifi_transient_link_policy_t;

void sys_wifi_transient_link_policy_init(sys_wifi_transient_link_policy_t *policy);
sys_wifi_transient_link_decision_t sys_wifi_transient_link_on_disconnect(
    sys_wifi_transient_link_policy_t *policy,
    bool link_was_connected,
    bool transient_error);
sys_wifi_transient_link_decision_t sys_wifi_transient_link_on_connect(
    sys_wifi_transient_link_policy_t *policy);
bool sys_wifi_transient_link_on_timeout(sys_wifi_transient_link_policy_t *policy);
void sys_wifi_transient_link_cancel(sys_wifi_transient_link_policy_t *policy);

#endif
