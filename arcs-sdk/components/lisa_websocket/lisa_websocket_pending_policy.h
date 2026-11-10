#ifndef LISA_WEBSOCKET_PENDING_POLICY_H
#define LISA_WEBSOCKET_PENDING_POLICY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LISA_WS_SEND_FAILED = -1,
    LISA_WS_SEND_COMPLETE = 0,
    LISA_WS_SEND_PENDING = 1,
} lisa_ws_send_outcome_t;

typedef enum {
    LISA_WS_PENDING_STALLED = -1,
    LISA_WS_PENDING_READY = 0,
    LISA_WS_PENDING_WAIT = 1,
} lisa_ws_pending_decision_t;

typedef struct {
    bool active;
    uint32_t since_ms;
} lisa_ws_pending_tracker_t;

lisa_ws_send_outcome_t lisa_ws_classify_send_result(int send_result, int pending_bytes);
lisa_ws_pending_decision_t lisa_ws_pending_tracker_update(lisa_ws_pending_tracker_t *tracker,
                                                          int pending_bytes,
                                                          uint32_t now_ms,
                                                          uint32_t timeout_ms);

#endif
