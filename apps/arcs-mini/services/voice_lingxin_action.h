#ifndef VOICE_LINGXIN_ACTION_H
#define VOICE_LINGXIN_ACTION_H

#include <stdbool.h>
bool voice_lingxin_action_is_supported(const char *code);

void voice_lingxin_action_init(void);
int voice_lingxin_action_handle_json(const char *json);

#endif
