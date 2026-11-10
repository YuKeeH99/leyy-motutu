#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SYS_WIFI_CONNECT_RESULT_SUCCESS       = 0x00,
    SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP    = 0x01,
    SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD = 0x02,
    SYS_WIFI_CONNECT_RESULT_FAIL_IP       = 0x03,
    SYS_WIFI_CONNECT_RESULT_FAIL_OTHER    = 0xFF,
} sys_wifi_connect_result_t;

#ifdef __cplusplus
extern "C" {
#endif

int sys_wifi_init(void);
int sys_wifi_start(bool autoconnect);
int sys_wifi_stop(void);
/* Only the standby worker owns radio transitions. Resume restores an idle
 * STA; the wakeup worker owns the bounded saved-AP connection attempt. */
int sys_wifi_suspend_radio(void);
int sys_wifi_resume_radio(void);
bool sys_wifi_radio_suspended(void);
bool sys_wifi_radio_is_off(void);
uint32_t sys_wifi_radio_epoch(void);
int sys_wifi_connect(const char *ssid, const char *pwd, const char *bssid);
/* Restart the saved-AP scanner for one caller-bounded recovery attempt.
 * Returns 0 when started, 1 when the Wi-Fi link is already up, or <0 when
 * recovery cannot start. The caller must stop it after its timeout. */
int sys_wifi_reconnect_saved_once_start(void);
void sys_wifi_reconnect_saved_once_stop(void);
/* Evidence from a complete saved-AP scan/attempt cycle; false while suspended,
 * unavailable or associated. A DHCP/cloud/DNS failure alone never qualifies. */
bool sys_wifi_reconnect_saved_exhausted(void);
/* Successful scan saw no matching saved AP; errors/association failures excluded. */
bool sys_wifi_reconnect_saved_no_signal(void);
/* Associated but DHCP failed or has exceeded its bounded wait. */
bool sys_wifi_ip_recovery_needed(void);
sys_wifi_connect_result_t sys_wifi_get_connect_result(void);
void sys_wifi_reset_connect_result(void);
int sys_wifi_save_ap(const char *ssid, const char *pwd, const char *bssid);
int sys_wifi_clear_saved_aps(void);
bool sys_wifi_get_signal_quality(int *rssi);
bool sys_wifi_get_ip_addr(uint32_t *ip_addr);
bool sys_wifi_get_ip_string(char *buf, size_t len);
bool sys_wifi_is_ready(void);
bool sys_wifi_is_started(void);
bool sys_wifi_is_connected(void);
bool sys_wifi_has_ap(void);

/* Real-time cloud audio continuously transmits small upstream frames. DTIM
 * power save can lose the AP link under that traffic pattern on ARCS-MINI.
 * This RAM-only switch is idempotent; false restores the normal DTIM policy. */
int sys_wifi_set_realtime_mode(bool enabled);

#ifdef __cplusplus
}
#endif
