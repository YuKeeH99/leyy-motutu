#ifndef SERVICE_VOICE_ACTION_QUEUE_H
#define SERVICE_VOICE_ACTION_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "service_v2_gait.h"

#define SERVICE_VOICE_ACTION_QUEUE_CAPACITY 16U

typedef struct {
    service_v2_gait_action_t action;
    uint32_t cycles;
    uint8_t speed_percent;
} service_voice_action_queue_item_t;

typedef struct {
    service_voice_action_queue_item_t items[SERVICE_VOICE_ACTION_QUEUE_CAPACITY];
    service_voice_action_queue_item_t current;
    uint8_t head;
    uint8_t count;
    bool active;
} service_voice_action_queue_t;

void service_voice_action_queue_init(service_voice_action_queue_t *queue);
void service_voice_action_queue_clear(service_voice_action_queue_t *queue);
bool service_voice_action_queue_push(service_voice_action_queue_t *queue,
                                     const service_voice_action_queue_item_t *item);
bool service_voice_action_queue_peek(const service_voice_action_queue_t *queue,
                                     service_voice_action_queue_item_t *item);
bool service_voice_action_queue_mark_active(service_voice_action_queue_t *queue);
bool service_voice_action_queue_complete(service_voice_action_queue_t *queue,
                                         service_v2_gait_action_t action);
bool service_voice_action_queue_active(const service_voice_action_queue_t *queue);
uint32_t service_voice_action_queue_count(const service_voice_action_queue_t *queue);

#endif /* SERVICE_VOICE_ACTION_QUEUE_H */
