#include "lingxin_trace.h"

#if defined(CONFIG_LINGXIN_TRACE_HISTORY) && CONFIG_LINGXIN_TRACE_HISTORY
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lisa_log.h"
#include "lisa_mem.h"

#define TAG "lingxin.trace"
#define LINGXIN_TRACE_MAX_LEN 1024

static char s_trace_history[LINGXIN_TRACE_MAX_LEN];

static void lingxin_trace_append(char *dst, size_t dst_len, const char *step)
{
    size_t used = 0;
    size_t step_len = 0;

    if (dst == NULL || dst_len == 0 || step == NULL || step[0] == '\0') {
        return;
    }

    step_len = strlen(step);
    if (step_len >= dst_len) {
        step += step_len - (dst_len - 1U);
        step_len = dst_len - 1U;
    }

    used = strlen(dst);
    while (used > 0U && used + 1U + step_len >= dst_len) {
        char *separator = strchr(dst, '|');
        if (separator == NULL) {
            dst[0] = '\0';
            used = 0U;
            break;
        }

        size_t remove_len = (size_t)(separator - dst) + 1U;
        memmove(dst, dst + remove_len, used - remove_len + 1U);
        used -= remove_len;
    }

    if (used > 0 && used + 1 < dst_len) {
        dst[used++] = '|';
        dst[used] = '\0';
    }

    if (used < dst_len - 1) {
        snprintf(dst + used, dst_len - used, "%s", step);
    }
}

int lingxin_trace_set(const char *step)
{
    if (!step) {
        return -1;
    }

    LOGI("[Lingxin][trace] %s", step);
    taskENTER_CRITICAL();
    lingxin_trace_append(s_trace_history, sizeof(s_trace_history), step);
    taskEXIT_CRITICAL();
    return 0;
}

int lingxin_trace_clear(void)
{
    taskENTER_CRITICAL();
    s_trace_history[0] = '\0';
    taskEXIT_CRITICAL();
    return 0;
}

int lingxin_trace_get(char **out)
{
    size_t len = 0;
    char *copy = NULL;
    char snapshot[LINGXIN_TRACE_MAX_LEN];

    if (out == NULL) {
        return -1;
    }

    taskENTER_CRITICAL();
    memcpy(snapshot, s_trace_history, sizeof(snapshot));
    taskEXIT_CRITICAL();
    snapshot[sizeof(snapshot) - 1U] = '\0';

    if (snapshot[0] == '\0') {
        return -1;
    }

    len = strlen(snapshot) + 1U;
    copy = (char *)lisa_mem_alloc((uint32_t)len);
    if (copy == NULL) {
        return -1;
    }

    memcpy(copy, snapshot, len);
    *out = copy;
    return 0;
}

int lingxin_trace_try_get(char **out)
{
    int ret = lingxin_trace_get(out);

    if (ret != 0 || out == NULL || *out == NULL || (*out)[0] == '\0') {
        return -1;
    }

    return 0;
}
#else
int lingxin_trace_set(const char *step)
{
    return step ? 0 : -1;
}

int lingxin_trace_clear(void)
{
    return 0;
}

int lingxin_trace_get(char **out)
{
    if (out) {
        *out = 0;
    }
    return -1;
}

int lingxin_trace_try_get(char **out)
{
    return lingxin_trace_get(out);
}
#endif
