#ifndef __BOOT_RECOVERY_ADB_GATE_H__
#define __BOOT_RECOVERY_ADB_GATE_H__

#include <stdbool.h>
#include <stdint.h>

bool boot_recovery_adb_should_start(uint32_t boot_info_raw);
bool boot_recovery_adb_should_escape(uint32_t boot_info_raw, bool host_connected,
                                     uint32_t elapsed_ms, uint32_t timeout_ms);
void boot_recovery_adb_prepare_escape(volatile uint32_t *boot_info_raw);

#endif /* __BOOT_RECOVERY_ADB_GATE_H__ */
