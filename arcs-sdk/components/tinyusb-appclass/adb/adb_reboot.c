#define LOG_TAG "adb.reboot"

#include "adb_services.h"
#include "adb.h"
#include "adb_utils.h"

#include <stdint.h>
#include <string.h>

#include "sys/reboot.h"
#include "uboot_recovery_api.h"

static void do_reboot(void)
{
	extern void sys_platform_sw_full_reset(void);
	sys_platform_sw_full_reset();
}

static int do_recovery(void)
{
#if defined(CONFIG_BOOT_RECOVERY_API) || defined(CONFIG_BOOT)
	int ret = uboot_recovery_request(UBOOT_RECOVERY_MODE_SOFT);
	if (ret != 0) {
		ADB_LOGE("request recovery failed: %d\n", ret);
		return ret;
	}
	sys_reboot(SYS_REBOOT_SOFT);
	return 0;
#else
	ADB_LOGE("recovery API is disabled\n");
	return -1;
#endif
}

static int adb_reboot_open(struct adb_service *s, const uint8_t *args)
{
	if (args == NULL || args[0] == '\0') {
		ADB_LOGI("adb reboot\n");
		do_reboot();
	} else if (strcmp((const char *)args, "recovery") == 0) {
		ADB_LOGI("adb reboot recovery\n");
		if (do_recovery() != 0) {
			return -1;
		}
	} else {
		ADB_LOGE("adb reboot: unknown arg: %s\n", args);
		return -1;
	}

	/* should not reach here */
	return 0;
}

static int adb_reboot_close(struct adb_service *s)
{
	return 0;
}

static int adb_reboot_write(struct adb_service *s, adb_packet_t *p)
{
	adb_packet_free(p);
	return 0;
}

static const struct adb_service_handle adb_reboot_handle = {
	.name = "reboot",
	.open = adb_reboot_open,
	.close = adb_reboot_close,
	.write = adb_reboot_write,
};

void adb_reboot_init(void)
{
	adb_service_hd_register(&adb_reboot_handle);
}
