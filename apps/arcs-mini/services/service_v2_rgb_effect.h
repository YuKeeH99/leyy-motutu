#ifndef SERVICE_V2_RGB_EFFECT_H
#define SERVICE_V2_RGB_EFFECT_H

#include "service_v2_rgb_effect_policy.h"
#include "service_v2_rgb_show.h"

/** Start the low-rate dialogue-state RGB breathing indicator. */
int service_v2_rgb_effect_init(void);
int service_v2_rgb_effect_set_enabled(int enabled);
void service_v2_rgb_effect_set_provisioning(
    service_v2_rgb_provisioning_state_t state);
void service_v2_rgb_effect_notify_dizzy(void);
/** Non-blocking lifecycle hooks, called at actual playback/motion boundaries. */
void service_v2_rgb_effect_show_start(service_v2_rgb_show_kind_t kind);
void service_v2_rgb_effect_show_stop(service_v2_rgb_show_kind_t kind);

#endif
