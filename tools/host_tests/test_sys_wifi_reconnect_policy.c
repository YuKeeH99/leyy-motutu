#include <assert.h>

#include "sys_wifi_reconnect_policy.h"

int main(void)
{
    assert(sys_wifi_reconnect_decide(false, true, true, true) ==
           SYS_WIFI_RECONNECT_UNAVAILABLE);
    assert(sys_wifi_reconnect_decide(true, false, true, true) ==
           SYS_WIFI_RECONNECT_UNAVAILABLE);
    assert(sys_wifi_reconnect_decide(true, true, false, true) ==
           SYS_WIFI_RECONNECT_UNAVAILABLE);

    /* A stale link flag must not suppress recovery after the internet layer
     * has already declared the transport unusable. */
    assert(sys_wifi_reconnect_decide(true, true, true, true) ==
           SYS_WIFI_RECONNECT_FORCE_REASSOCIATE);
    assert(sys_wifi_reconnect_decide(true, true, true, false) ==
           SYS_WIFI_RECONNECT_START_AUTOCONNECT);
    return 0;
}
