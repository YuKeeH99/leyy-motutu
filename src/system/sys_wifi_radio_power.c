#include "sys_wifi_radio_power.h"
#include <errno.h>

bool sys_wifi_radio_power_blocked(const sys_wifi_radio_power_t *power)
{
    return __atomic_load_n(&power->blocked, __ATOMIC_ACQUIRE) != 0U;
}

bool sys_wifi_radio_power_is_off(const sys_wifi_radio_power_t *power)
{
    return __atomic_load_n(&power->off, __ATOMIC_ACQUIRE) != 0U;
}

int sys_wifi_radio_power_suspend(sys_wifi_radio_power_t *power,
                                const sys_wifi_radio_power_ops_t *ops, void *arg)
{
    /* Only the standby coordinator calls transitions (never ISR/event bus).
     * This guard rejects reentrant callbacks; other tasks only read the gate. */
    if (power->busy != 0U) {
        return -EBUSY;
    }
    power->busy = 1U;
    int ret = 0;
    if (!sys_wifi_radio_power_is_off(power)) {
        __atomic_store_n(&power->blocked, 1U, __ATOMIC_RELEASE);
        ret = ops->pause_io(arg);
        if (ret == 0) {
            ret = ops->off(arg);
            if (ret == 0) {
                __atomic_store_n(&power->off, 1U, __ATOMIC_RELEASE);
            }
        }
    }
    power->busy = 0U;
    return ret;
}

int sys_wifi_radio_power_resume(sys_wifi_radio_power_t *power,
                               const sys_wifi_radio_power_ops_t *ops, void *arg)
{
    if (power->busy != 0U) {
        return -EBUSY;
    }
    power->busy = 1U;
    int ret = 0;
    if (sys_wifi_radio_power_blocked(power)) {
        if (sys_wifi_radio_power_is_off(power)) {
            ret = ops->on(arg);
            if (ret == 0) {
                __atomic_store_n(&power->off, 0U, __ATOMIC_RELEASE);
            }
        }
        if (ret == 0) {
            ret = ops->restore(arg);
        }
        if (ret == 0) {
            ops->release_io(arg);
            __atomic_store_n(&power->blocked, 0U, __ATOMIC_RELEASE);
        }
    }
    power->busy = 0U;
    return ret;
}
