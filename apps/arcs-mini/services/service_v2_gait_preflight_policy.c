#include "service_v2_gait_preflight_policy.h"

bool service_v2_gait_action_requires_normal_face(service_v2_gait_action_t action)
{
    switch (action) {
    /* Backflip uses a fixed standing-start sequence; never auto-flip first. */
    case SERVICE_V2_GAIT_ACTION_BACKFLIP:
    case SERVICE_V2_GAIT_ACTION_STOP:
    case SERVICE_V2_GAIT_ACTION_STAND:
    case SERVICE_V2_GAIT_ACTION_ALL_OFF:
    case SERVICE_V2_GAIT_ACTION_ASSEMBLY:
    case SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE:
        return false;
    default:
        return true;
    }
}

bool service_v2_gait_accel_z_is_upside_down(int32_t accel_z)
{
    return accel_z >= SERVICE_V2_GAIT_UPSIDE_Z_MIN_LSB;
}
