#include "lingxin_timer.h"

#define TAG "lingxin.timer"

#include <stdbool.h>
#include <stdint.h>

#include "lisa_log.h"
#include "lisa_timer.h"

#define LINGXIN_TIMER_MAX 16

typedef struct {
    bool used;
    bool periodic;
    lisa_timer_t *timer;
    void *priv;
    void (*func)(void *priv);
} lingxin_timer_slot_t;

static lingxin_timer_slot_t s_timer_slots[LINGXIN_TIMER_MAX];

static int lingxin_timer_slot_alloc(void)
{
    for (int i = 0; i < LINGXIN_TIMER_MAX; i++) {
        if (!s_timer_slots[i].used) {
            s_timer_slots[i].used = true;
            return i;
        }
    }
    return INVALID_TIMER_ID;
}

static lingxin_timer_slot_t *lingxin_timer_slot_get(int timer_id)
{
    if (timer_id < 0 || timer_id >= LINGXIN_TIMER_MAX || !s_timer_slots[timer_id].used) {
        return NULL;
    }
    return &s_timer_slots[timer_id];
}

static void lingxin_timer_callback(struct lisa_timer *timer)
{
    for (int i = 0; i < LINGXIN_TIMER_MAX; i++) {
        lingxin_timer_slot_t *slot = &s_timer_slots[i];
        if (slot->used && slot->timer == timer) {
            if (slot->func) {
                slot->func(slot->priv);
            }
            if (slot->periodic && slot->used && slot->timer) {
                lisa_timer_start(slot->timer);
            }
            return;
        }
    }
}

static int lingxin_timer_create_common(void *priv, void (*func)(void *priv), long msec, bool periodic)
{
    if (!func || msec < 0) {
        return INVALID_TIMER_ID;
    }

    int id = lingxin_timer_slot_alloc();
    if (id == INVALID_TIMER_ID) {
        LOGE("no timer slot");
        return INVALID_TIMER_ID;
    }

    lingxin_timer_slot_t *slot = &s_timer_slots[id];
    slot->periodic = periodic;
    slot->priv = priv;
    slot->func = func;
    slot->timer = lisa_timer_create((uint32_t)msec, lingxin_timer_callback, NULL);
    if (!slot->timer) {
        slot->used = false;
        return INVALID_TIMER_ID;
    }

    if (lisa_timer_start(slot->timer) != LISA_OK) {
        lisa_timer_delete(slot->timer);
        *slot = (lingxin_timer_slot_t){0};
        return INVALID_TIMER_ID;
    }

    return id;
}

int lingxin_sys_timer_add(void *priv, void (*func)(void *priv), long msec)
{
    return lingxin_timer_create_common(priv, func, msec, true);
}

int lingxin_sys_timer_del(int timer_id)
{
    lingxin_timer_slot_t *slot = lingxin_timer_slot_get(timer_id);
    if (!slot) {
        return -1;
    }
    lisa_timer_delete(slot->timer);
    *slot = (lingxin_timer_slot_t){0};
    return 0;
}

int lingxin_sys_timer_re_run(int timer_id)
{
    lingxin_timer_slot_t *slot = lingxin_timer_slot_get(timer_id);
    if (!slot || !slot->timer) {
        return -1;
    }
    lisa_timer_stop(slot->timer);
    return lisa_timer_start(slot->timer) == LISA_OK ? 0 : -1;
}

int lingxin_one_shot_timer_create(void *priv, void (*func)(void *priv), long countdown)
{
    return lingxin_timer_create_common(priv, func, countdown, false);
}

int lingxin_one_shot_timer_delete(int timerId)
{
    return lingxin_sys_timer_del(timerId);
}
