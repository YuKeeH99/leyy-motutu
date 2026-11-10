#include "lingxin_printf.h"
#include "lingxin_log.h"

#define TAG "lingxin"

#include "lisa_log.h"

void lingxin_printf(char *message)
{
    if (message) {
        LOGI("%s", message);
    }
}

void lingxin_log_printf(int level, char *message)
{
    if (!message) {
        return;
    }
    if (level == LINGXIN_ERROR) {
        LOGE("%s", message);
    } else if (level == LINGXIN_WARN) {
        LOGW("%s", message);
    } else {
        LOGI("%s", message);
    }
}
