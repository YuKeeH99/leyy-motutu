#include <assert.h>
#include <stdbool.h>

#include "sys_wifi_transient_link_policy.h"

int main(void)
{
    sys_wifi_transient_link_policy_t policy;

    /* Field logs show same-AP reassociation commonly needs about six seconds.
     * The grace period must not tear down DHCP/WebSocket before that. */
    assert(SYS_WIFI_TRANSIENT_LINK_GRACE_MS >= 10000U);
    assert(SYS_WIFI_TRANSIENT_LINK_GRACE_MS <= 30000U);

    sys_wifi_transient_link_policy_init(&policy);
    assert(sys_wifi_transient_link_on_disconnect(&policy, true, false) ==
           SYS_WIFI_TRANSIENT_LINK_PASS_THROUGH);
    assert(!policy.grace_pending);

    assert(sys_wifi_transient_link_on_disconnect(&policy, true, true) ==
           SYS_WIFI_TRANSIENT_LINK_START_GRACE);
    assert(policy.grace_pending);
    assert(sys_wifi_transient_link_on_disconnect(&policy, true, true) ==
           SYS_WIFI_TRANSIENT_LINK_IGNORE_DURING_GRACE);
    assert(sys_wifi_transient_link_on_connect(&policy) ==
           SYS_WIFI_TRANSIENT_LINK_RECOVERED);
    assert(!policy.grace_pending);
    assert(!sys_wifi_transient_link_on_timeout(&policy));

    assert(sys_wifi_transient_link_on_disconnect(&policy, true, true) ==
           SYS_WIFI_TRANSIENT_LINK_START_GRACE);
    assert(sys_wifi_transient_link_on_timeout(&policy));
    assert(!policy.grace_pending);

    assert(sys_wifi_transient_link_on_disconnect(&policy, false, true) ==
           SYS_WIFI_TRANSIENT_LINK_PASS_THROUGH);
    sys_wifi_transient_link_cancel(&policy);
    assert(!policy.grace_pending);

    return 0;
}
