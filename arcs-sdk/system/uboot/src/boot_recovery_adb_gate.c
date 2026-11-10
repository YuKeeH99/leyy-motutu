#include "boot_recovery_adb_gate.h"

#include "boot_config.h"

static bool boot_recovery_adb_reason_allows_start(uint8_t recover_reason)
{
    switch (recover_reason) {
    case RECOVER_REASON_SOFT_REQ:
    case RECOVER_REASON_HARD_REQ:
    case RECOVER_REASON_APP_INVALID:
    case RECOVER_REASON_AP_WDT_TIMEOUT:
    case RECOVER_REASON_BOOT_WDT_TIMEOUT:
        return true;
    default:
        return false;
    }
}

bool boot_recovery_adb_should_start(uint32_t boot_info_raw)
{
    union {
        struct boot_info info;
        uint32_t raw;
    } bits = {
        .raw = boot_info_raw,
    };

    /* Only explicit recovery metadata opens ADB; OTA/pending entry bits alone do not. */
    return boot_recovery_adb_reason_allows_start(bits.info.recover_reason);
}

bool boot_recovery_adb_should_escape(uint32_t boot_info_raw, bool host_connected,
                                     uint32_t elapsed_ms, uint32_t timeout_ms)
{
    union {
        struct boot_info info;
        uint32_t raw;
    } bits = {.raw = boot_info_raw};

    return bits.info.recover_reason == RECOVER_REASON_SOFT_REQ && !host_connected &&
           timeout_ms != 0u && elapsed_ms >= timeout_ms;
}

void boot_recovery_adb_prepare_escape(volatile uint32_t *boot_info_raw)
{
    union {
        struct boot_info info;
        uint32_t raw;
    } bits = {0};

    if (boot_info_raw == NULL) {
        return;
    }

    bits.raw = *boot_info_raw;
    bits.info.req = 0u;
    bits.info.recover_reason = RECOVER_REASON_NONE;
    bits.info.handshake_timeout = 0u;
    bits.info.boot_wdt = 0u;
    bits.info.resume_normal_boot = 1u;
    *boot_info_raw = bits.raw;
}
