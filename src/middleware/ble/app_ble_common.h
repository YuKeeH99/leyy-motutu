#ifndef __APP_BLE_COMMON_H__
#define __APP_BLE_COMMON_H__
#include <stdbool.h>

void app_ble_netcfg_init(void);
void app_ble_netcfg_prepare(void);
void app_ble_netcfg_adv_start_delayed(void);
void app_ble_netcfg_complete(void);
/* Freeze intake/advertising without erasing credentials or bond information.
 * -EBUSY means a credential/advertisement/notification-scan worker is finishing;
 * cancel sleep. Queued jobs check the pause flag before touching the radio. */
int app_ble_netcfg_set_paused(bool paused);
bool app_ble_netcfg_offline_blocked(void);
/* Compatibility gate/release; never pause advertising or disconnect phones. */
int app_ble_netcfg_offline_acquire(void);
void app_ble_netcfg_offline_release(void);

#endif

