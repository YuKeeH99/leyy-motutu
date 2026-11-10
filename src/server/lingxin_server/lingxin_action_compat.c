#include "lingxin_action_compat.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool action_code_seen(const lingxin_action_compat_state_t *state, const char *action)
{
    for (uint8_t i = 0U; i < state->seen_count; ++i) {
        if (strcmp(state->seen_actions[i], action) == 0) {
            return true;
        }
    }
    return false;
}

static void action_code_remember(lingxin_action_compat_state_t *state, const char *action)
{
    size_t length = strlen(action);

    if (state->seen_count >= LINGXIN_ACTION_COMPAT_MAX_ACTIONS ||
        length >= LINGXIN_ACTION_COMPAT_CODE_SIZE) {
        return;
    }
    memcpy(state->seen_actions[state->seen_count], action, length + 1U);
    state->seen_count++;
}

void lingxin_action_compat_reset(lingxin_action_compat_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int lingxin_action_compat_publish_value(lingxin_action_compat_state_t *state,
                                       const cJSON *actions,
                                       const char *source,
                                       lingxin_action_compat_emit_fn emit,
                                       void *user_data)
{
    if (state == NULL || actions == NULL || source == NULL || emit == NULL) {
        return 0;
    }

    int count = cJSON_IsArray(actions) ? cJSON_GetArraySize(actions) : 1;
    if ((!cJSON_IsArray(actions) && !cJSON_IsObject(actions)) ||
        count < 0 || count > (int)LINGXIN_ACTION_COMPAT_MAX_ACTIONS) {
        return 0;
    }

    int published = 0;
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_IsArray(actions) ? cJSON_GetArrayItem(actions, i) : actions;
        const char *action = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(item, "action"));

        if (!cJSON_IsObject(item) || action == NULL || action[0] == '\0' ||
            strlen(action) >= LINGXIN_ACTION_COMPAT_CODE_SIZE) {
            continue;
        }
        if (action_code_seen(state, action)) {
            if (state->duplicate_count < UINT8_MAX) {
                state->duplicate_count++;
            }
            continue;
        }

        char *serialized = cJSON_PrintUnformatted(item);
        if (serialized == NULL) {
            continue;
        }
        if (emit(source, action, serialized, user_data) == 0) {
            action_code_remember(state, action);
            published++;
        }
        cJSON_free(serialized);
    }
    return published;
}

int lingxin_action_compat_publish_json(lingxin_action_compat_state_t *state,
                                      const char *json,
                                      const char *source,
                                      lingxin_action_compat_emit_fn emit,
                                      void *user_data)
{
    if (json == NULL || json[0] == '\0') {
        return 0;
    }

    cJSON *actions = cJSON_Parse(json);
    if (actions == NULL) {
        return 0;
    }
    int published = lingxin_action_compat_publish_value(state, actions, source, emit, user_data);
    cJSON_Delete(actions);
    return published;
}

static bool ascii_equal_ci(char lhs, char rhs)
{
    return tolower((unsigned char)lhs) == tolower((unsigned char)rhs);
}

static const char *find_explicit_json_array(const char *text)
{
    if (text == NULL) {
        return NULL;
    }

    for (const char *cursor = text; cursor[0] != '\0'; ++cursor) {
        if (cursor[1] == '\0' || cursor[2] == '\0' || cursor[3] == '\0' ||
            !ascii_equal_ci(cursor[0], 'j') ||
            !ascii_equal_ci(cursor[1], 's') ||
            !ascii_equal_ci(cursor[2], 'o') ||
            !ascii_equal_ci(cursor[3], 'n')) {
            continue;
        }
        if (cursor != text && isalnum((unsigned char)cursor[-1])) {
            continue;
        }

        const char *array = cursor + 4;
        while (*array == ' ' || *array == '\t' || *array == '\r' || *array == '\n' ||
               *array == ':' || *array == '`') {
            array++;
        }
        if (*array == '[') {
            return array;
        }
    }
    return NULL;
}

int lingxin_action_compat_publish_embedded(lingxin_action_compat_state_t *state,
                                          const char *text,
                                          lingxin_action_compat_emit_fn emit,
                                          void *user_data)
{
    const char *array = find_explicit_json_array(text);
    return array == NULL ? 0 : lingxin_action_compat_publish_json(
                                  state, array, "agent_response_json", emit, user_data);
}
