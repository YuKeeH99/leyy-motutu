#include "stdint.h"
#include "stdio.h"
#include "string.h"
#include "stddef.h"

#include "cmd.h"

#include "shell.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lisa_mem.h"
#include "chip.h"
#include "power_manager.h"
#include "uboot_recovery_api.h"

static int reboot_cmd_handler(int argc, char **argv)
{
    power_reboot_soft();

    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, reboot,
                 reboot_cmd_handler, reboot);

static int recovery_cmd_handler(int argc, char **argv)
{
    int ret = uboot_recovery_request(UBOOT_RECOVERY_MODE_SOFT);
    if (ret != 0) {
        printf("request recovery failed: %d\n", ret);
        return ret;
    }

    power_reboot_soft();

    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, recovery,
                 recovery_cmd_handler, recovery);

static int threads_cmd(int argc, char **argv)
{
    uint32_t tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *item = lisa_mem_alloc(tasks * sizeof(TaskStatus_t));
    if (item) {
        uint32_t total = 0;
        tasks = uxTaskGetSystemState(item, tasks, &total);
        /* Keep stack/state diagnostics when runtime CPU counters are off. */
        if (tasks > 0) {
            printf("%s", "\n---------------------------------------------------------------------------------------------\n");
            printf("%s", "Name                      State  Prio  Stack  MinFree    MaxUsed    Tid    Call100US      PCT\n");
            printf("%s", "---------------------------------------------------------------------------------------------\n");
            for (uint32_t i = 0, pct = 0; i < tasks; i++) {
                uint32_t stack_size = (item[i].pxEndOfStack - item[i].pxStackBase + 2) * sizeof(StackType_t);
                uint32_t min_free = item[i].usStackHighWaterMark * sizeof(StackType_t);
                float max_used_pct = 100.0f - (float)min_free / (float)stack_size * 100.0f;
                
                if ((pct = total ? (uint32_t)(100.0f * item[i].ulRunTimeCounter / total) : 0U)) {
                    printf("%-25s %-6c %-6u %-6u %-10u %-10.1f %-6u %-12u %5u%%\n", item[i].pcTaskName,
                           "XRBSD"[item[i].eCurrentState], item[i].uxCurrentPriority,
                           stack_size, min_free, max_used_pct, item[i].xTaskNumber,
                           item[i].ulRunTimeCounter, pct);
                } else {
                    printf("%-25s %-6c %-6u %-6u %-10u %-10.1f %-6u %-12u %5s%%\n", item[i].pcTaskName,
                           "XRBSD"[item[i].eCurrentState], item[i].uxCurrentPriority,
                           stack_size, min_free, max_used_pct, item[i].xTaskNumber,
                           item[i].ulRunTimeCounter, total ? "<1" : "n/a");
                }
            }
            printf("%s", "---------------------------------------------------------------------------------------------\n\n");
        }
        lisa_mem_free(item);
    }

    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, threads,
                 threads_cmd, show threads info);
