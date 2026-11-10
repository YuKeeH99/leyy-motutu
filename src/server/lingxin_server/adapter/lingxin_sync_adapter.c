#include "lingxin_mutex.h"
#include "lingxin_semaphore.h"

#include "lisa_mutex.h"
#include "lisa_semaphore.h"
#include "lisa_typedef.h"

lingxin_mutex_t lingxin_mutex_create()
{
    return lisa_mutex_create();
}

void lingxin_mutex_lock(lingxin_mutex_t mutex)
{
    if (mutex) {
        lisa_mutex_lock((lisa_mutex_t *)mutex, -1);
    }
}

void lingxin_mutex_unlock(lingxin_mutex_t mutex)
{
    if (mutex) {
        lisa_mutex_unlock((lisa_mutex_t *)mutex);
    }
}

void lingxin_mutex_destroy(lingxin_mutex_t mutex)
{
    if (mutex) {
        lisa_mutex_delete((lisa_mutex_t *)mutex);
    }
}

lingxin_semaphore_t lingxin_semaphore_create(uint32_t cnt)
{
    lisa_semaphore_t *sem = lisa_semaphore_create(cnt > 0 ? cnt : 1);
    if (!sem) {
        return NULL;
    }

    for (uint32_t i = 0; i < cnt; i++) {
        lisa_semaphore_give(sem);
    }
    return sem;
}

void lingxin_semaphore_pend(lingxin_semaphore_t sem, uint32_t timeout_ms)
{
    if (sem) {
        lisa_semaphore_take((lisa_semaphore_t *)sem, timeout_ms == 0 ? LISA_OS_WAIT_FOREVER : (int32_t)timeout_ms);
    }
}

void lingxin_semaphore_post(lingxin_semaphore_t sem)
{
    if (sem) {
        lisa_semaphore_give((lisa_semaphore_t *)sem);
    }
}

void lingxin_semaphore_set_value(lingxin_semaphore_t sem, uint32_t cnt)
{
    if (sem) {
        if (cnt == 0) {
            lisa_semaphore_clear((lisa_semaphore_t *)sem);
        }
    }
}

void lingxin_semaphore_destroy(lingxin_semaphore_t sem)
{
    if (sem) {
        lisa_semaphore_delete((lisa_semaphore_t *)sem);
    }
}
