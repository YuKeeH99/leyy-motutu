#include <assert.h>

#include "service_v2_gait_preflight_policy.h"

static void user_actions_require_normal_face(void)
{
    assert(service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_FORWARD));
    assert(service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_PUSHUP));
    assert(service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_PRONE));
    assert(service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP));
    assert(service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_DANCE));
    assert(service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_TENNIS_SERVE));
}

static void emergency_and_internal_actions_skip_recovery(void)
{
    assert(!service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_STOP));
    /* Backflip checks its own start face, and must never run a forward flip first. */
    assert(!service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_BACKFLIP));
    assert(!service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_ALL_OFF));
    assert(!service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_ASSEMBLY));
    assert(!service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_STAND));
    assert(!service_v2_gait_action_requires_normal_face(SERVICE_V2_GAIT_ACTION_UPSIDE_WIGGLE));
}

static void only_a_clear_positive_z_face_is_upside_down(void)
{
    assert(!service_v2_gait_accel_z_is_upside_down(-16000));
    assert(!service_v2_gait_accel_z_is_upside_down(0));
    assert(!service_v2_gait_accel_z_is_upside_down(7999));
    assert(service_v2_gait_accel_z_is_upside_down(8000));
    assert(service_v2_gait_accel_z_is_upside_down(16000));
}

int main(void)
{
    user_actions_require_normal_face();
    emergency_and_internal_actions_skip_recovery();
    only_a_clear_positive_z_face_is_upside_down();
    return 0;
}
