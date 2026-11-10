#pragma once
#include <stdbool.h>
/* Shared read-only ownership gate; USB (including full battery) rejects
 * autonomous movement. Ordinary foreground motions do not use this gate. */
bool service_v2_background_motion_allowed(void);
/* Called only by the low-priority touch worker, including disabled/sleep polls.
 * USB + business idle releases unowned PWM without moving to a different pose. */
void service_v2_idle_action_poll(bool enabled);
