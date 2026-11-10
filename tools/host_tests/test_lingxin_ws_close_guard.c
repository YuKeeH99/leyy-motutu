#include <assert.h>
#include <stddef.h>
#include <stdbool.h>

#include "lingxin_ws_close_guard.h"

int main(void)
{
    bool closing = false;
    assert(lingxin_ws_close_guard_begin(&closing));
    assert(closing);
    assert(!lingxin_ws_close_guard_begin(&closing));
    assert(!lingxin_ws_close_guard_begin(NULL));
    return 0;
}
