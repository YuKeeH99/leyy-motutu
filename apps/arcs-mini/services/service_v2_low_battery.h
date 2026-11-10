#ifndef SERVICE_V2_LOW_BATTERY_H
#define SERVICE_V2_LOW_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVICE_V2_LOW_BATTERY_SIM_IDLE = 0,
    SERVICE_V2_LOW_BATTERY_SIM_ACTIVE,
    SERVICE_V2_LOW_BATTERY_SIM_SLEEP,
} service_v2_low_battery_sim_state_t;

typedef struct {
    bool ready;
    bool simulated;
    bool external_power;
    bool reminder_allowed;
    bool warning_active;
    bool critical_confirming;
    bool shutdown_requested;
    uint8_t level;
    uint32_t reminder_count;
    uint32_t shutdown_count;
} service_v2_low_battery_status_t;

int service_v2_low_battery_init(void);
int service_v2_low_battery_get_status(service_v2_low_battery_status_t *status);
int service_v2_low_battery_simulate(uint8_t level,
                                    service_v2_low_battery_sim_state_t state);
void service_v2_low_battery_clear_simulation(void);

/** One-shot query used by the normal shutdown audio callback. */
bool service_v2_low_battery_consume_shutdown_prompt_request(void);

#ifdef __cplusplus
}
#endif

#endif
