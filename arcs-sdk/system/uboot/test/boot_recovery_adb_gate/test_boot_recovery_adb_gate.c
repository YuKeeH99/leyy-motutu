#include "unity.h"

#ifdef RUN_TEST
#undef RUN_TEST
#endif
#define RUN_TEST(TestFunc, TestLineNum) UnityDefaultTestRun(TestFunc, #TestFunc, TestLineNum)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot_config.h"
#include "boot_recovery_adb_gate.h"

static uint32_t make_boot_info_raw(bool req, bool pending, uint8_t recover_reason, bool handshake_timeout,
                                   bool boot_wdt)
{
    union {
        struct boot_info info;
        uint32_t raw;
    } bits = {0};

    bits.info.req = req ? 1u : 0u;
    bits.info.ota_pending = pending ? 1u : 0u;
    bits.info.recover_reason = recover_reason;
    bits.info.handshake_timeout = handshake_timeout ? 1u : 0u;
    bits.info.boot_wdt = boot_wdt ? 1u : 0u;
    return bits.raw;
}

void setUp(void)
{
}

void tearDown(void)
{
}

void test_allows_explicit_recovery_reasons_only(void)
{
    const uint8_t allow_reasons[] = {
        RECOVER_REASON_SOFT_REQ,
        RECOVER_REASON_HARD_REQ,
        RECOVER_REASON_APP_INVALID,
        RECOVER_REASON_AP_WDT_TIMEOUT,
        RECOVER_REASON_BOOT_WDT_TIMEOUT,
    };

    for (size_t i = 0; i < sizeof(allow_reasons) / sizeof(allow_reasons[0]); ++i) {
        TEST_ASSERT_TRUE(boot_recovery_adb_should_start(
            make_boot_info_raw(false, false, allow_reasons[i], false, false)));
    }
}

void test_denies_zero_boot_info(void)
{
    TEST_ASSERT_FALSE(boot_recovery_adb_should_start(0));
}

void test_denies_pending_only_second_stage_entry(void)
{
    TEST_ASSERT_FALSE(boot_recovery_adb_should_start(
        make_boot_info_raw(false, true, RECOVER_REASON_NONE, false, false)));
}

void test_denies_unknown_recovery_reason(void)
{
    TEST_ASSERT_FALSE(boot_recovery_adb_should_start(
        make_boot_info_raw(false, false, RECOVER_REASON_MAX, false, false)));
}

void test_denies_reason_above_recovery_max(void)
{
    TEST_ASSERT_FALSE(boot_recovery_adb_should_start(
        make_boot_info_raw(false, false, UINT8_MAX, false, false)));
}

void test_denies_request_and_pending_bits_without_explicit_reason(void)
{
    TEST_ASSERT_FALSE(boot_recovery_adb_should_start(
        make_boot_info_raw(true, true, RECOVER_REASON_NONE, true, true)));
}

void test_soft_recovery_escapes_only_after_timeout_without_usb_host(void)
{
    uint32_t raw = make_boot_info_raw(false, false, RECOVER_REASON_SOFT_REQ, false, false);

    TEST_ASSERT_FALSE(boot_recovery_adb_should_escape(raw, false, 29999u, 30000u));
    TEST_ASSERT_TRUE(boot_recovery_adb_should_escape(raw, false, 30000u, 30000u));
    TEST_ASSERT_FALSE(boot_recovery_adb_should_escape(raw, true, 60000u, 30000u));
}

void test_non_soft_recovery_never_uses_the_usb_timeout_escape(void)
{
    const uint8_t reasons[] = {
        RECOVER_REASON_NONE,
        RECOVER_REASON_HARD_REQ,
        RECOVER_REASON_APP_INVALID,
        RECOVER_REASON_AP_WDT_TIMEOUT,
        RECOVER_REASON_BOOT_WDT_TIMEOUT,
    };

    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        uint32_t raw = make_boot_info_raw(false, false, reasons[i], false, false);
        TEST_ASSERT_FALSE(boot_recovery_adb_should_escape(raw, false, 60000u, 30000u));
    }
}

void test_soft_recovery_escape_clears_transient_reason_and_marks_normal_resume(void)
{
    union {
        struct boot_info info;
        uint32_t raw;
    } bits = {0};

    bits.info.req = 1u;
    bits.info.recover_reason = RECOVER_REASON_SOFT_REQ;
    bits.info.handshake_timeout = 1u;
    bits.info.boot_wdt = 1u;

    boot_recovery_adb_prepare_escape(&bits.raw);

    TEST_ASSERT_EQUAL_UINT32(0u, bits.info.req);
    TEST_ASSERT_EQUAL_UINT32(RECOVER_REASON_NONE, bits.info.recover_reason);
    TEST_ASSERT_EQUAL_UINT32(0u, bits.info.handshake_timeout);
    TEST_ASSERT_EQUAL_UINT32(0u, bits.info.boot_wdt);
    TEST_ASSERT_EQUAL_UINT32(1u, bits.info.resume_normal_boot);
}

int main(void)
{
    UnityBegin("system/uboot/test/boot_recovery_adb_gate/test_boot_recovery_adb_gate.c");

    RUN_TEST(test_allows_explicit_recovery_reasons_only, __LINE__);
    RUN_TEST(test_denies_zero_boot_info, __LINE__);
    RUN_TEST(test_denies_pending_only_second_stage_entry, __LINE__);
    RUN_TEST(test_denies_unknown_recovery_reason, __LINE__);
    RUN_TEST(test_denies_reason_above_recovery_max, __LINE__);
    RUN_TEST(test_denies_request_and_pending_bits_without_explicit_reason, __LINE__);
    RUN_TEST(test_soft_recovery_escapes_only_after_timeout_without_usb_host, __LINE__);
    RUN_TEST(test_non_soft_recovery_never_uses_the_usb_timeout_escape, __LINE__);
    RUN_TEST(test_soft_recovery_escape_clears_transient_reason_and_marks_normal_resume, __LINE__);

    return UnityEnd();
}
