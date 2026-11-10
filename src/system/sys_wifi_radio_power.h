#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Zero initialization is the normal boot state. Only the standby worker owns
 * transitions; readers may query the gate from event/connection workers. */
typedef struct {
    uint32_t busy;
    uint32_t blocked;
    uint32_t off;
} sys_wifi_radio_power_t;

typedef struct {
    int (*pause_io)(void *arg);
    int (*off)(void *arg);
    int (*on)(void *arg);
    int (*restore)(void *arg);
    void (*release_io)(void *arg);
} sys_wifi_radio_power_ops_t;

bool sys_wifi_radio_power_blocked(const sys_wifi_radio_power_t *power);
bool sys_wifi_radio_power_is_off(const sys_wifi_radio_power_t *power);
/* Failures retain the closed gate. The caller must resume to roll back, and
 * must not publish successful standby until suspend returns zero. */
int sys_wifi_radio_power_suspend(sys_wifi_radio_power_t *power,
                                const sys_wifi_radio_power_ops_t *ops, void *arg);
int sys_wifi_radio_power_resume(sys_wifi_radio_power_t *power,
                               const sys_wifi_radio_power_ops_t *ops, void *arg);
