#include <assert.h>

#include "service_v2_touch_action_policy.h"

int main(void)
{
    assert(service_v2_touch_pose_allows_action(SERVICE_V2_IMU_POSE_NORMAL));
    assert(!service_v2_touch_pose_allows_action(SERVICE_V2_IMU_POSE_UNKNOWN));
    assert(!service_v2_touch_pose_allows_action(SERVICE_V2_IMU_POSE_UPSIDE_DOWN));
    assert(!service_v2_touch_pose_allows_action(SERVICE_V2_IMU_POSE_OTHER));
    return 0;
}
