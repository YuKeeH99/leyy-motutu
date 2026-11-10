#ifndef SERVICE_V2_GAIT_PREFLIGHT_POLICY_H
#define SERVICE_V2_GAIT_PREFLIGHT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "service_v2_gait.h"

#ifdef __cplusplus
extern "C" {
#endif

/* User-visible motions need a normal-face preflight. Emergency, maintenance,
 * and the IMU's own upside-down feedback must remain immediately executable. */
bool service_v2_gait_action_requires_normal_face(service_v2_gait_action_t action);

/* Leave a wide dead band around a side/tilted pose.  A roll recovery is only
 * allowed after gravity-Z clearly identifies the upside-down face. */
#define SERVICE_V2_GAIT_UPSIDE_Z_MIN_LSB 8000
bool service_v2_gait_accel_z_is_upside_down(int32_t accel_z);

#ifdef __cplusplus
}
#endif

#endif
