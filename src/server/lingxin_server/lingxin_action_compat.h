#ifndef LINGXIN_ACTION_COMPAT_H
#define LINGXIN_ACTION_COMPAT_H

#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINGXIN_ACTION_COMPAT_MAX_ACTIONS 32U
#define LINGXIN_ACTION_COMPAT_CODE_SIZE   48U

typedef struct {
    char seen_actions[LINGXIN_ACTION_COMPAT_MAX_ACTIONS][LINGXIN_ACTION_COMPAT_CODE_SIZE];
    uint8_t seen_count;
    uint8_t duplicate_count;
} lingxin_action_compat_state_t;

typedef int (*lingxin_action_compat_emit_fn)(const char *source,
                                            const char *action,
                                            const char *json,
                                            void *user_data);

void lingxin_action_compat_reset(lingxin_action_compat_state_t *state);

int lingxin_action_compat_publish_value(lingxin_action_compat_state_t *state,
                                       const cJSON *actions,
                                       const char *source,
                                       lingxin_action_compat_emit_fn emit,
                                       void *user_data);

int lingxin_action_compat_publish_json(lingxin_action_compat_state_t *state,
                                      const char *json,
                                      const char *source,
                                      lingxin_action_compat_emit_fn emit,
                                      void *user_data);

int lingxin_action_compat_publish_embedded(lingxin_action_compat_state_t *state,
                                          const char *text,
                                          lingxin_action_compat_emit_fn emit,
                                          void *user_data);

#ifdef __cplusplus
}
#endif

#endif
