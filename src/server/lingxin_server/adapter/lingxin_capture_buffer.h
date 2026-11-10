#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Preserve the beginning of speech during a cold TLS/start_task handshake.
 * Once sending catches up, limit live backlog to four seconds again. */
#define LINGXIN_CAPTURE_CAPACITY (16000U * 2U * 16U)
#define LINGXIN_CAPTURE_LIVE_LIMIT (16000U * 2U * 4U)
typedef struct {
    uint32_t generation, read, length, limit;
    bool active, overflow;
    uint8_t data[LINGXIN_CAPTURE_CAPACITY];
} lingxin_capture_buffer_t;

uint32_t lingxin_capture_begin(lingxin_capture_buffer_t *b);
void lingxin_capture_end(lingxin_capture_buffer_t *b, uint32_t generation);
bool lingxin_capture_put(lingxin_capture_buffer_t *b, const void *data, uint32_t len);
bool lingxin_capture_take(lingxin_capture_buffer_t *b, void *data, uint32_t len);
