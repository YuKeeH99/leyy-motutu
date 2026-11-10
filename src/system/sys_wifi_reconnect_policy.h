#ifndef SYS_WIFI_RECONNECT_POLICY_H
#define SYS_WIFI_RECONNECT_POLICY_H

#include <stdbool.h>

typedef enum {
    SYS_WIFI_RECONNECT_UNAVAILABLE = 0,
    SYS_WIFI_RECONNECT_START_AUTOCONNECT,
    SYS_WIFI_RECONNECT_FORCE_REASSOCIATE,
} sys_wifi_reconnect_decision_t;

sys_wifi_reconnect_decision_t sys_wifi_reconnect_decide(bool manager_ready,
                                                        bool station_started,
                                                        bool has_saved_ap,
                                                        bool link_connected);

#endif
