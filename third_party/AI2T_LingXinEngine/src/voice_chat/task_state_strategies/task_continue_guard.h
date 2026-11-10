#ifndef TASK_CONTINUE_GUARD_H
#define TASK_CONTINUE_GUARD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    volatile uint32_t hold_count;
    volatile bool deferred;
} task_continue_guard_t;

void task_continue_guard_acquire(task_continue_guard_t *guard);
bool task_continue_guard_should_defer(task_continue_guard_t *guard);
void task_continue_guard_cancel_deferred(task_continue_guard_t *guard);
bool task_continue_guard_release(task_continue_guard_t *guard);
void task_continue_guard_reset(task_continue_guard_t *guard);

#endif
