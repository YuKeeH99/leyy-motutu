#include <assert.h>
#include <stdbool.h>

#include "service_voice_action_queue.h"

static service_voice_action_queue_item_t item(service_v2_gait_action_t action)
{
    service_voice_action_queue_item_t value = {
        .action = action,
        .cycles = (uint32_t)action + 1U,
        .speed_percent = 50U,
    };
    return value;
}

int main(void)
{
    service_voice_action_queue_t queue;
    service_voice_action_queue_item_t next;

    service_voice_action_queue_init(&queue);
    assert(service_voice_action_queue_count(&queue) == 0U);
    assert(!service_voice_action_queue_active(&queue));

    service_voice_action_queue_item_t forward = item(SERVICE_V2_GAIT_ACTION_FORWARD);
    service_voice_action_queue_item_t dance = item(SERVICE_V2_GAIT_ACTION_DANCE);
    service_voice_action_queue_item_t roll = item(SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP);
    assert(service_voice_action_queue_push(&queue, &forward));
    assert(service_voice_action_queue_push(&queue, &dance));
    assert(service_voice_action_queue_push(&queue, &roll));
    assert(service_voice_action_queue_count(&queue) == 3U);

    assert(service_voice_action_queue_peek(&queue, &next));
    assert(next.action == SERVICE_V2_GAIT_ACTION_FORWARD);
    assert(service_voice_action_queue_mark_active(&queue));
    assert(service_voice_action_queue_active(&queue));
    assert(service_voice_action_queue_count(&queue) == 2U);
    assert(!service_voice_action_queue_peek(&queue, &next));
    assert(!service_voice_action_queue_complete(&queue, SERVICE_V2_GAIT_ACTION_DANCE));
    assert(service_voice_action_queue_complete(&queue, SERVICE_V2_GAIT_ACTION_FORWARD));

    assert(service_voice_action_queue_peek(&queue, &next));
    assert(next.action == SERVICE_V2_GAIT_ACTION_DANCE);
    assert(service_voice_action_queue_mark_active(&queue));
    assert(service_voice_action_queue_complete(&queue, SERVICE_V2_GAIT_ACTION_DANCE));
    assert(service_voice_action_queue_peek(&queue, &next));
    assert(next.action == SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP);

    service_voice_action_queue_clear(&queue);
    assert(service_voice_action_queue_count(&queue) == 0U);
    assert(!service_voice_action_queue_active(&queue));
    assert(!service_voice_action_queue_peek(&queue, &next));

    for (uint32_t i = 0U; i < SERVICE_VOICE_ACTION_QUEUE_CAPACITY; ++i) {
        assert(service_voice_action_queue_push(&queue, &forward));
    }
    assert(!service_voice_action_queue_push(&queue, &forward));
    return 0;
}
