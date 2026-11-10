#include <assert.h>
#include <math.h>

#include "service_voice_action_policy.h"

int main(void)
{
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_PUSHUP));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_BACKFLIP));
    assert(service_voice_action_resolve_pushup_cycles(NAN) == 4U);
    assert(service_voice_action_resolve_pushup_cycles(-INFINITY) == 3U);
    assert(service_voice_action_resolve_pushup_cycles(0.0) == 3U);
    assert(service_voice_action_resolve_pushup_cycles(4.0) == 4U);
    assert(service_voice_action_resolve_pushup_cycles(5.0) == 5U);
    assert(service_voice_action_resolve_pushup_cycles(INFINITY) == 5U);
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_GREETING));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_SCRATCH));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_SPIN));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_SIT));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_KISS));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_DANCE));
    assert(service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_JUMP));

    assert(!service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_FORWARD));
    assert(!service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_BACKWARD));
    assert(!service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_RUN_FORWARD));
    assert(!service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_RUN_BACKWARD));
    assert(!service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_TURN_LEFT));
    assert(!service_voice_action_has_fixed_timing(SERVICE_V2_GAIT_ACTION_TURN_RIGHT));

    assert(service_voice_action_resolve_turn_cycles(1U, false) ==
           SERVICE_V2_GAIT_DEFAULT_TURN_CYCLES);
    assert(service_voice_action_resolve_turn_cycles(12U, false) == 12U);
    assert(service_voice_action_resolve_turn_cycles(4U, true) == 4U);
    return 0;
}
