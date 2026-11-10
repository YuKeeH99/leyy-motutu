#include <string.h>

#include "ota_manager.h"
#include "shell.h"

static int cmd_ota(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();

    if (argc <= 1 || strcmp(argv[1], "status") == 0) {
        ota_state_t state;
        if (ota_manager_get_state_snapshot(&state) != 0) return -1;
        shellPrint(shell, "state=%d target=%d progress=%u/%u version=%s->%s\r\n",
                   state.state, state.target, state.bytes_processed, state.bytes_total,
                   state.current_version, state.target_version);
        return 0;
    }
    if (strcmp(argv[1], "check") == 0) {
        return ota_manager_check_all();
    }
    if (strcmp(argv[1], "confirm") == 0) {
        return ota_manager_confirm_app_update();
    }
    if (strcmp(argv[1], "defer") == 0) {
        return ota_manager_skip_app_update();
    }

    shellPrint(shell, "Usage: ota [status|check|confirm|defer]\r\n");
    return -1;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 ota, cmd_ota, OTA check/status/confirm/defer);
