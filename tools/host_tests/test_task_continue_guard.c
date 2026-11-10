#include <assert.h>
#include <stdbool.h>

#include "task_continue_guard.h"

int main(void)
{
    task_continue_guard_t guard = {0};

    assert(!task_continue_guard_should_defer(&guard));
    task_continue_guard_acquire(&guard);
    task_continue_guard_acquire(&guard);
    assert(task_continue_guard_should_defer(&guard));
    assert(!task_continue_guard_release(&guard));
    assert(task_continue_guard_release(&guard));
    assert(!task_continue_guard_release(&guard));

    task_continue_guard_acquire(&guard);
    assert(task_continue_guard_should_defer(&guard));
    task_continue_guard_cancel_deferred(&guard);
    assert(task_continue_guard_release(&guard) == false);

    task_continue_guard_acquire(&guard);
    task_continue_guard_acquire(&guard);
    assert(task_continue_guard_should_defer(&guard));
    task_continue_guard_reset(&guard);
    assert(!task_continue_guard_should_defer(&guard));
    assert(!task_continue_guard_release(&guard));
    return 0;
}
