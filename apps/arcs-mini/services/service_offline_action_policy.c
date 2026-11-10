#include "service_offline_action_policy.h"
#include <string.h>

/* Canonical intent/intentStr values from the 2026-09-23 ListenAI export, not
 * guessed pronunciations or numeric resource IDs. Generalization words are
 * resolved to these semantics by ESR; unknown semantics fail closed. */
static const struct { const char *intent; int command; } s_commands[] = {
    {"qian2 jin4", SERVICE_V2_GAIT_ACTION_FORWARD},
    {"hou4 tui4", SERVICE_V2_GAIT_ACTION_BACKWARD},
    {"zuo3 zhuan3", SERVICE_V2_GAIT_ACTION_TURN_LEFT},
    {"you4 zhuan3", SERVICE_V2_GAIT_ACTION_TURN_RIGHT},
    {"ting2 zhi3 yi2 dong4", SERVICE_V2_GAIT_ACTION_STOP},
    {"wang3 qian2 pao3", SERVICE_V2_GAIT_ACTION_RUN_FORWARD},
    {"wang3 hou4 pao3", SERVICE_V2_GAIT_ACTION_RUN_BACKWARD},
    {"zhan4 qi3 lai2", SERVICE_V2_GAIT_ACTION_STAND},
    {"pa1 xia4 lai2", SERVICE_V2_GAIT_ACTION_PRONE},
    {"dao4 li4", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"fan1 gen1 tou4", OFFLINE_CMD_FLIP_CONTINUOUS},
    {"hou4 kong1 fan1", SERVICE_V2_GAIT_ACTION_BACKFLIP},
    {"chang4 ge1", SERVICE_V2_GAIT_ACTION_MUSIC_BEAT},
    {"wang3 qiu2 fa1 qiu2", SERVICE_V2_GAIT_ACTION_TENNIS_SERVE},
    {"wang3 qiu2 zheng4 shou3", SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND},
    {"wang3 qiu2 dan1 fan3", SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND},
    {"wang3 qiu2 shuang1 fan3", SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND},
    {"wo4 shou3", SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE},
    {"da3 zhao1 hu1", SERVICE_V2_GAIT_ACTION_GREETING},
    /* Application-ready; absent from this particular recognition export. */
    {"nao2 yang3 yang3", SERVICE_V2_GAIT_ACTION_SCRATCH},
    {"zhuan4 quan1 quan1", SERVICE_V2_GAIT_ACTION_SPIN},
    {"zuo4 xia4 lai2", SERVICE_V2_GAIT_ACTION_SIT},
    {"qin1 qin1 wo3", SERVICE_V2_GAIT_ACTION_KISS},
    {"tiao4 wu3", SERVICE_V2_GAIT_ACTION_DANCE},
    {"tiao4 yue4", SERVICE_V2_GAIT_ACTION_JUMP},
    {"niu3 pi4 gu3", SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT},
    {"xiu1 xi1", OFFLINE_CMD_GOODBYE},
    {"jin4 ru4 pei4 wang3 mo2 shi4", OFFLINE_CMD_NETCFG},
    {"zeng1 da4 yin1 liang4", OFFLINE_CMD_VOLUME_UP},
    {"jian3 xiao3 yin1 liang4", OFFLINE_CMD_VOLUME_DOWN},
    {"jin4 ru4 li2 xian4 mo2 shi4", OFFLINE_CMD_ENTER_OFFLINE},
    {"jin4 ru4 lian2 wang3 mo2 shi4", OFFLINE_CMD_ENTER_ONLINE},
    {"cha2 xun2 xi4 tong3 ban3 ben3", OFFLINE_CMD_VERSION},
};

int offline_map(const char *intent)
{
    if (!intent) return -1;
    for (unsigned i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i)
        if (!strcmp(intent, s_commands[i].intent)) return s_commands[i].command;
    return -1;
}

void offline_policy_init(offline_policy_t *p)
{
    memset(p, 0, sizeof(*p));
}

static bool offline_motion_allowed(int command)
{
    if (command == OFFLINE_CMD_FLIP_CONTINUOUS) return true;
    if (command <= SERVICE_V2_GAIT_ACTION_STOP || command >= OFFLINE_CMD_NETCFG) return false;
    for (unsigned i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i)
        if (command == s_commands[i].command) return true;
    return false;
}

bool offline_eligible(bool online, bool provisioning, bool sleeping, bool blocked)
{
    return !online && !provisioning && !sleeping && !blocked;
}

void offline_cancel(offline_policy_t *p)
{
    if (++p->generation == 0) ++p->generation;
    p->state = OFFLINE_IDLE;
    p->head = p->count = 0;
    p->control_head = p->control_count = 0;
    p->netcfg_requested = false;
    p->was_busy = false;
    p->local_request = 0;
}

void offline_begin(offline_policy_t *p, bool offline, uint32_t now)
{
    offline_cancel(p);
    p->state = offline ? OFFLINE_PROMPT : OFFLINE_ONLINE;
    p->last_activity_ms = now;
}

void offline_prompt_finished(offline_policy_t *p, uint32_t generation, bool ok, uint32_t now)
{
    if (p->generation != generation || p->state != OFFLINE_PROMPT) return;
    if (!ok) { offline_cancel(p); return; }
    p->prompt_done_ms = now;
    p->state = OFFLINE_TAIL;
}

bool offline_enqueue(offline_policy_t *p, uint32_t generation, int action, uint32_t now)
{
    if (p->generation != generation) return false;
    if (action == OFFLINE_CMD_NETCFG &&
        (p->state != OFFLINE_IDLE || p->local_request || p->netcfg_requested)) {
        if (p->netcfg_requested) return true;
        offline_cancel(p);
        p->netcfg_requested = true;
        p->last_activity_ms = now;
        return true;
    }
    if (action >= OFFLINE_CMD_ENTER_OFFLINE && action <= OFFLINE_CMD_VERSION &&
        (p->state == OFFLINE_ONLINE || p->state == OFFLINE_ACTIVE || p->local_request || p->netcfg_requested ||
         (action != OFFLINE_CMD_VERSION && p->state >= OFFLINE_PROMPT))) {
        if (action == OFFLINE_CMD_ENTER_OFFLINE && p->forced_offline && !p->local_request) return true;
        if (action != OFFLINE_CMD_VERSION && p->last_mode_command == action && !p->local_request &&
            now - p->last_mode_ms < 1000U) return true;
        if (p->local_request == action) return true;
        offline_cancel(p);
        p->local_request = action;
        if (action != OFFLINE_CMD_VERSION) {
            p->last_mode_command = action;
            p->last_mode_ms = now;
        }
        p->last_activity_ms = now;
        return true;
    }
    if (p->state != OFFLINE_ACTIVE || p->generation != generation)
        return false;
    if (action == SERVICE_V2_GAIT_ACTION_STOP || action == OFFLINE_CMD_GOODBYE) {
        offline_cancel(p); /* Priority controls cannot sit behind a dance/roll. */
        if (action == SERVICE_V2_GAIT_ACTION_STOP) p->state = OFFLINE_ACTIVE;
    } else if (action == OFFLINE_CMD_NETCFG) {
        offline_cancel(p); /* Priority exit invalidates every older queued request. */
        p->netcfg_requested = true;
    } else if (action == OFFLINE_CMD_VOLUME_UP || action == OFFLINE_CMD_VOLUME_DOWN) {
        if (p->control_count == 3) return false;
        p->controls[(p->control_head + p->control_count++) % 3] = action;
    } else {
        if (p->count == 3 || !offline_motion_allowed(action)) return false;
        p->queue[(p->head + p->count++) % 3] = action;
    }
    p->last_activity_ms = now;
    return true;
}

int offline_take(offline_policy_t *p, uint32_t generation)
{
    if (p->state != OFFLINE_ACTIVE || p->generation != generation || !p->count) return -1;
    int action = p->queue[p->head];
    p->head = (p->head + 1) % 3;
    --p->count;
    return action;
}

int offline_take_control(offline_policy_t *p, uint32_t generation)
{
    if (p->state != OFFLINE_ACTIVE || p->generation != generation || !p->control_count) return -1;
    int command = p->controls[p->control_head];
    p->control_head = (p->control_head + 1) % 3;
    --p->control_count;
    return command;
}

void offline_poll(offline_policy_t *p, uint32_t now, bool busy)
{
    if (p->state == OFFLINE_PROMPT && now - p->last_activity_ms >= 30000) {
        offline_cancel(p); /* Lost/error completion never opens the command gate. */
        return;
    }
    if (p->state == OFFLINE_TAIL && now - p->prompt_done_ms >= 200) {
        p->state = OFFLINE_ACTIVE;
        p->last_activity_ms = now;
    }
    if (p->state != OFFLINE_ACTIVE) return;
    if (p->was_busy && !busy) p->last_activity_ms = now;
    p->was_busy = busy;
    if (!busy && !p->count && !p->control_count && now - p->last_activity_ms >= 30000) offline_cancel(p);
}
