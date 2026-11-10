#include "lingxin_capture_buffer.h"
#include <string.h>

uint32_t lingxin_capture_begin(lingxin_capture_buffer_t *b)
{
    if (++b->generation == 0U) ++b->generation;
    b->read = b->length = 0U;
    b->limit = LINGXIN_CAPTURE_CAPACITY;
    b->active = true;
    b->overflow = false;
    return b->generation;
}

void lingxin_capture_end(lingxin_capture_buffer_t *b, uint32_t generation)
{
    if (b->generation != generation) return;
    b->active = false;
    b->read = b->length = 0U;
}

bool lingxin_capture_put(lingxin_capture_buffer_t *b, const void *data, uint32_t len)
{
    if (!b->active || !data || !len) return false;
    if (len > b->limit - b->length) {
        /* Invalidate queued downstream frames too. Do not silently replace
         * the sentence prefix or concatenate audio across a missing region. */
        lingxin_capture_begin(b);
        lingxin_capture_end(b, b->generation);
        b->overflow = true;
        return false;
    }
    uint32_t write = (b->read + b->length) % LINGXIN_CAPTURE_CAPACITY;
    uint32_t first = LINGXIN_CAPTURE_CAPACITY - write;
    if (first > len) first = len;
    memcpy(b->data + write, data, first);
    memcpy(b->data, (const uint8_t *)data + first, len - first);
    b->length += len;
    return true;
}

bool lingxin_capture_take(lingxin_capture_buffer_t *b, void *data, uint32_t len)
{
    if (!b->active || !data || !len || len > b->length) return false;
    uint32_t first = LINGXIN_CAPTURE_CAPACITY - b->read;
    if (first > len) first = len;
    memcpy(data, b->data + b->read, first);
    memcpy((uint8_t *)data + first, b->data, len - first);
    b->read = (b->read + len) % LINGXIN_CAPTURE_CAPACITY;
    b->length -= len;
    if (b->length <= LINGXIN_CAPTURE_LIVE_LIMIT)
        b->limit = LINGXIN_CAPTURE_LIVE_LIMIT;
    return true;
}
