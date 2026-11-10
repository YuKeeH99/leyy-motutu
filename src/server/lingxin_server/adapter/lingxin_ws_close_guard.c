#include "lingxin_ws_close_guard.h"

#include <stddef.h>

bool lingxin_ws_close_guard_begin(bool *closing)
{
    if (closing == NULL) {
        return false;
    }
#if defined(LINGXIN_WS_CLOSE_GUARD_HOST_TEST)
    bool was_closing = *closing;
    *closing = true;
    return !was_closing;
#else
    return !__atomic_exchange_n(closing, true, __ATOMIC_ACQ_REL);
#endif
}
