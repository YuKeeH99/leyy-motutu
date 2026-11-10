#include "lisa_websocket_pending_policy.h"

#include <stddef.h>

lisa_ws_send_outcome_t lisa_ws_classify_send_result(int send_result, int pending_bytes)
{
    if (send_result == -2) {
        return pending_bytes > 0 ? LISA_WS_SEND_PENDING : LISA_WS_SEND_FAILED;
    }
    if (send_result < 0) {
        return LISA_WS_SEND_FAILED;
    }
    return pending_bytes > 0 ? LISA_WS_SEND_PENDING : LISA_WS_SEND_COMPLETE;
}

lisa_ws_pending_decision_t lisa_ws_pending_tracker_update(lisa_ws_pending_tracker_t *tracker,
                                                          int pending_bytes,
                                                          uint32_t now_ms,
                                                          uint32_t timeout_ms)
{
    if (tracker == NULL) {
        return LISA_WS_PENDING_STALLED;
    }
    if (pending_bytes <= 0) {
        tracker->active = false;
        tracker->since_ms = 0U;
        return LISA_WS_PENDING_READY;
    }
    if (!tracker->active) {
        tracker->active = true;
        tracker->since_ms = now_ms;
    }
    if ((uint32_t)(now_ms - tracker->since_ms) >= timeout_ms) {
        return LISA_WS_PENDING_STALLED;
    }
    return LISA_WS_PENDING_WAIT;
}
