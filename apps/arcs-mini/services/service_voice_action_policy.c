#include "service_voice_action_policy.h"

uint32_t service_voice_action_resolve_pushup_cycles(double requested_cycles)
{
    if (requested_cycles != requested_cycles) return 4U;
    if (requested_cycles < 3.0) return 3U;
    if (requested_cycles > 5.0) return 5U;
    return (uint32_t)requested_cycles;
}

bool service_voice_action_has_fixed_timing(service_v2_gait_action_t action)
{
    switch (action) {
    case SERVICE_V2_GAIT_ACTION_MUSIC_BEAT:
    case SERVICE_V2_GAIT_ACTION_PUSHUP:
    case SERVICE_V2_GAIT_ACTION_BACKFLIP:
    case SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE:
    case SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE:
    case SERVICE_V2_GAIT_ACTION_GREETING:
    case SERVICE_V2_GAIT_ACTION_SCRATCH:
    case SERVICE_V2_GAIT_ACTION_SPIN:
    case SERVICE_V2_GAIT_ACTION_SIT:
    case SERVICE_V2_GAIT_ACTION_KISS:
    case SERVICE_V2_GAIT_ACTION_DANCE:
    case SERVICE_V2_GAIT_ACTION_JUMP:
    case SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND:
    case SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND:
    case SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND:
        return true;
    default:
        return false;
    }
}

uint32_t service_voice_action_resolve_turn_cycles(uint32_t requested_cycles,
                                                  bool has_explicit_angle)
{
    if (!has_explicit_angle && requested_cycles < SERVICE_V2_GAIT_DEFAULT_TURN_CYCLES) {
        return SERVICE_V2_GAIT_DEFAULT_TURN_CYCLES;
    }
    return requested_cycles;
}
