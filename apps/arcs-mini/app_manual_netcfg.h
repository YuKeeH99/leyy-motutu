#ifndef APP_MANUAL_NETCFG_H
#define APP_MANUAL_NETCFG_H
/* Task context only, after the caller drains motion and dialogue.
 * Shared with triple click; never deletes saved Wi-Fi credentials. */
int app_enter_manual_netcfg(void);
#endif
