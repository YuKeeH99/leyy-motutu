#include "lingxin_thread.h"

#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lisa_mem.h"
#include "lisa_log.h"
#include "lisa_thread.h"
#include "lisa_typedef.h"

#include "lingxin_trace.h"

#define TAG "lingxin.thread"

typedef struct {
    void *(*entry)(void *);
    void *arg;
} lingxin_thread_ctx_t;

static void lingxin_thread_trampoline(void *arg)
{
    lingxin_thread_ctx_t *ctx = (lingxin_thread_ctx_t *)arg;
    if (!ctx || !ctx->entry) {
        if (ctx) {
            lisa_mem_free(ctx);
        }
        vTaskDelete(NULL);
        return;
    }
    ctx->entry(ctx->arg);
    lisa_mem_free(ctx);
    /* FreeRTOS task entry functions must not return into the scheduler port. */
    vTaskDelete(NULL);
}

static uint32_t lingxin_thread_map_priority(int priority)
{
    if (priority >= LISA_OS_PRIORITY_IDLE && priority <= LISA_OS_PRIORITY_REAL_TIME) {
        return (uint32_t)priority;
    }

    return LISA_OS_PRIORITY_LOW;
}

int lingxin_thread_create(lingxin_tid_t *thread, const lingxin_thread_param_t *param, void *(*start_routine)(void *), void *args)
{
    if (!thread || !start_routine) {
        LOGE("invalid create args");
        return -1;
    }

    lisa_thread_attr_t attr = {0};
    attr.name = (uint8_t *)((param && param->name) ? param->name : "lingxin");
    int requested_priority = (param && param->priority > 0) ? param->priority : LISA_OS_PRIORITY_LOW;
    attr.priority = lingxin_thread_map_priority(requested_priority);
    attr.stack_size = (param && param->stack_size > 0) ? (uint32_t)param->stack_size : 2048;

    if ((uint32_t)requested_priority != attr.priority) {
        char trace_step[64] = {0};
        snprintf(trace_step, sizeof(trace_step), "thread:prio_%d_to_%u", requested_priority, (unsigned)attr.priority);
        lingxin_trace_set(trace_step);
        LOGW("thread priority mapped, name=%s requested=%d mapped=%u",
             attr.name ? (char *)attr.name : "lingxin",
             requested_priority,
             (unsigned)attr.priority);
    }

    lingxin_thread_ctx_t *ctx = (lingxin_thread_ctx_t *)lisa_mem_alloc(sizeof(lingxin_thread_ctx_t));
    if (!ctx) {
        LOGE("ctx alloc failed, name=%s", attr.name ? (char *)attr.name : "lingxin");
        return -1;
    }
    ctx->entry = start_routine;
    ctx->arg = args;

    lisa_thread_t *handle = lisa_thread_create(&attr, lingxin_thread_trampoline, ctx);
    if (!handle) {
        LOGE("thread create failed, name=%s stack=%u prio=%u",
             attr.name ? (char *)attr.name : "lingxin",
             (unsigned)attr.stack_size,
             (unsigned)attr.priority);
        lisa_mem_free(ctx);
        return -1;
    }

    *thread = (lingxin_tid_t)(uintptr_t)handle;
    LOGI("thread create ok, name=%s stack=%u prio=%u handle=%p",
         attr.name ? (char *)attr.name : "lingxin",
         (unsigned)attr.stack_size,
         (unsigned)attr.priority,
         handle);
    return 0;
}

void lingxin_thread_destroy(lingxin_tid_t thread, lingxin_thread_destroy_mode_t mode)
{
    (void)mode;
    lisa_thread_t *handle = (lisa_thread_t *)(uintptr_t)thread;
    if (handle) {
        lisa_thread_delete(handle);
    }
}

char *lingxin_get_current_thread_name()
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();

    if (task == NULL) {
        return "unknown";
    }

    return pcTaskGetName(task);
}

void lingxin_thread_sleep(int time)
{
    lisa_thread_mdelay((uint32_t)time);
}
