#include "service_v2_touch_action_policy.h"

bool service_v2_touch_pose_allows_action(service_v2_imu_pose_t pose)
{
    return pose == SERVICE_V2_IMU_POSE_NORMAL;
}
