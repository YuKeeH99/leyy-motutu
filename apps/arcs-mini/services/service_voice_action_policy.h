#ifndef SERVICE_VOICE_ACTION_POLICY_H
#define SERVICE_VOICE_ACTION_POLICY_H

#include <stdbool.h>

#include "service_v2_gait.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Product presentation actions have a calibrated client-side timeline.
 * Cloud SPEED/TIME values must not stretch these motions into unsafe or
 * mechanically inconsistent sequences. Parameters that select semantics
 * (HAND and DIRECTION) are handled separately by the cloud action bridge.
 * COUNT is fixed except pushups, which explicitly permit 3~5 repetitions.
 */
bool service_voice_action_has_fixed_timing(service_v2_gait_action_t action);

/* Pushups permit 3~5 repetitions while retaining the fixed per-stroke timing. */
uint32_t service_voice_action_resolve_pushup_cycles(double requested_cycles);

/* Ordinary left/right commands must not collapse to one short model-supplied
 * COUNT/TIME cycle. Explicit ANGLE requests retain their calculated length. */
uint32_t service_voice_action_resolve_turn_cycles(uint32_t requested_cycles,
                                                  bool has_explicit_angle);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_VOICE_ACTION_POLICY_H */
