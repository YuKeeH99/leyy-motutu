#include "service_voice_action_queue.h"

#include <string.h>

void service_voice_action_queue_init(service_voice_action_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

void service_voice_action_queue_clear(service_voice_action_queue_t *queue)
{
    service_voice_action_queue_init(queue);
}

bool service_voice_action_queue_push(service_voice_action_queue_t *queue,
                                     const service_voice_action_queue_item_t *item)
{
    if (queue == NULL || item == NULL || queue->count >= SERVICE_VOICE_ACTION_QUEUE_CAPACITY) {
        return false;
    }

    uint8_t tail = (uint8_t)((queue->head + queue->count) % SERVICE_VOICE_ACTION_QUEUE_CAPACITY);
    queue->items[tail] = *item;
    queue->count++;
    return true;
}

bool service_voice_action_queue_peek(const service_voice_action_queue_t *queue,
                                     service_voice_action_queue_item_t *item)
{
    if (queue == NULL || item == NULL || queue->active || queue->count == 0U) {
        return false;
    }

    *item = queue->items[queue->head];
    return true;
}

bool service_voice_action_queue_mark_active(service_voice_action_queue_t *queue)
{
    if (queue == NULL || queue->active || queue->count == 0U) {
        return false;
    }

    queue->current = queue->items[queue->head];
    queue->head = (uint8_t)((queue->head + 1U) % SERVICE_VOICE_ACTION_QUEUE_CAPACITY);
    queue->count--;
    queue->active = true;
    return true;
}

bool service_voice_action_queue_complete(service_voice_action_queue_t *queue,
                                         service_v2_gait_action_t action)
{
    if (queue == NULL || !queue->active || queue->current.action != action) {
        return false;
    }

    queue->active = false;
    return true;
}

bool service_voice_action_queue_active(const service_voice_action_queue_t *queue)
{
    return queue != NULL && queue->active;
}

uint32_t service_voice_action_queue_count(const service_voice_action_queue_t *queue)
{
    return queue != NULL ? queue->count : 0U;
}
