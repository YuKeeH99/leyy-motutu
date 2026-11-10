#ifndef SERVICE_V2_TOUCH_ACTION_POLICY_H
#define SERVICE_V2_TOUCH_ACTION_POLICY_H

#include <stdbool.h>

#include "service_v2_imu_interaction.h"

#ifdef __cplusplus
extern "C" {
#endif

bool service_v2_touch_pose_allows_action(service_v2_imu_pose_t pose);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_V2_TOUCH_ACTION_POLICY_H */
