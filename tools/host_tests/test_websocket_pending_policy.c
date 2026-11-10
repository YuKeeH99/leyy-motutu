#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "lisa_websocket_pending_policy.h"

int main(void)
{
    lisa_ws_pending_tracker_t tracker = {0};

    assert(lisa_ws_classify_send_result(-2, 512) == LISA_WS_SEND_PENDING);
    assert(lisa_ws_classify_send_result(-2, 0) == LISA_WS_SEND_FAILED);
    assert(lisa_ws_classify_send_result(320, 0) == LISA_WS_SEND_COMPLETE);
    assert(lisa_ws_classify_send_result(320, 64) == LISA_WS_SEND_PENDING);
    assert(lisa_ws_classify_send_result(-1, 0) == LISA_WS_SEND_FAILED);

    assert(lisa_ws_pending_tracker_update(&tracker, 512, 100U, 3000U) == LISA_WS_PENDING_WAIT);
    assert(tracker.active && tracker.since_ms == 100U);
    assert(lisa_ws_pending_tracker_update(&tracker, 128, 3099U, 3000U) == LISA_WS_PENDING_WAIT);
    assert(lisa_ws_pending_tracker_update(&tracker, 128, 3100U, 3000U) == LISA_WS_PENDING_STALLED);
    assert(lisa_ws_pending_tracker_update(&tracker, 0, 3101U, 3000U) == LISA_WS_PENDING_READY);
    assert(!tracker.active);

    return 0;
}
