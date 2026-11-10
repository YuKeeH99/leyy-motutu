#include "sys_wifi_reconnect_policy.h"

sys_wifi_reconnect_decision_t sys_wifi_reconnect_decide(bool manager_ready,
                                                        bool station_started,
                                                        bool has_saved_ap,
                                                        bool link_connected)
{
    if (!manager_ready || !station_started || !has_saved_ap) {
        return SYS_WIFI_RECONNECT_UNAVAILABLE;
    }
    return link_connected ? SYS_WIFI_RECONNECT_FORCE_REASSOCIATE
                          : SYS_WIFI_RECONNECT_START_AUTOCONNECT;
}
