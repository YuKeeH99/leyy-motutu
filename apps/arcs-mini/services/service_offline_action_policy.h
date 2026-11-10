#ifndef SERVICE_OFFLINE_ACTION_POLICY_H
#define SERVICE_OFFLINE_ACTION_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#include "service_v2_gait.h"

typedef enum { OFFLINE_IDLE, OFFLINE_ONLINE, OFFLINE_PROMPT, OFFLINE_TAIL, OFFLINE_ACTIVE } offline_state_t;
/* Local utilities are not gait actions. */
enum {
    OFFLINE_CMD_NETCFG = 256, OFFLINE_CMD_VOLUME_UP, OFFLINE_CMD_VOLUME_DOWN,
    OFFLINE_CMD_GOODBYE, OFFLINE_CMD_FLIP_CONTINUOUS,
    OFFLINE_CMD_ENTER_OFFLINE, OFFLINE_CMD_ENTER_ONLINE, OFFLINE_CMD_VERSION
};
typedef struct {
    offline_state_t state;
    uint32_t generation, last_activity_ms, prompt_done_ms;
    int queue[3];
    unsigned head, count;
    int controls[3];
    unsigned control_head, control_count;
    bool netcfg_requested;
    bool was_busy;
    bool forced_offline;
    int local_request;
    int last_mode_command;
    uint32_t last_mode_ms;
} offline_policy_t;

int offline_map(const char *intent);
void offline_policy_init(offline_policy_t *p);
bool offline_eligible(bool online, bool provisioning, bool sleeping, bool blocked);
void offline_cancel(offline_policy_t *p);
void offline_begin(offline_policy_t *p, bool offline, uint32_t now);
void offline_prompt_finished(offline_policy_t *p, uint32_t generation, bool ok, uint32_t now);
bool offline_enqueue(offline_policy_t *p, uint32_t generation, int action, uint32_t now);
int offline_take(offline_policy_t *p, uint32_t generation);
int offline_take_control(offline_policy_t *p, uint32_t generation);
void offline_poll(offline_policy_t *p, uint32_t now, bool busy);
#endif
