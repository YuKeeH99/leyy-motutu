#include "task_continue_guard.h"

void task_continue_guard_acquire(task_continue_guard_t *guard)
{
    if (guard == 0) {
        return;
    }
    guard->hold_count++;
}

bool task_continue_guard_should_defer(task_continue_guard_t *guard)
{
    if (guard == 0 || guard->hold_count == 0U) {
        return false;
    }

    guard->deferred = true;
    if (guard->hold_count == 0U) {
        /* The final action may have completed between the first load and the
         * deferred store. In that case continuing now is safe. */
        guard->deferred = false;
        return false;
    }
    return true;
}

void task_continue_guard_cancel_deferred(task_continue_guard_t *guard)
{
    if (guard != 0) {
        guard->deferred = false;
    }
}

bool task_continue_guard_release(task_continue_guard_t *guard)
{
    if (guard == 0) {
        return false;
    }

    if (guard->hold_count == 0U) {
        return false;
    }
    guard->hold_count--;
    if (guard->hold_count != 0U) {
        return false;
    }

    bool deferred = guard->deferred;
    guard->deferred = false;
    return deferred;
}

void task_continue_guard_reset(task_continue_guard_t *guard)
{
    if (guard != 0) {
        guard->deferred = false;
        guard->hold_count = 0U;
    }
}
