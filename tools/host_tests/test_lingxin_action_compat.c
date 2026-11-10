#include <assert.h>
#include <string.h>

#include "lingxin_action_compat.h"

typedef struct {
    unsigned count;
    char source[24];
    char action[32];
    char json[192];
} capture_t;

static int capture_action(const char *source, const char *action,
                          const char *json, void *user_data)
{
    capture_t *capture = (capture_t *)user_data;

    capture->count++;
    strncpy(capture->source, source, sizeof(capture->source) - 1U);
    strncpy(capture->action, action, sizeof(capture->action) - 1U);
    strncpy(capture->json, json, sizeof(capture->json) - 1U);
    return 0;
}

static void plain_or_malformed_text_is_not_executable(void)
{
    lingxin_action_compat_state_t state = {0};
    capture_t capture = {0};

    assert(lingxin_action_compat_publish_embedded(
               &state, "好的，收到。", capture_action, &capture) == 0);
    assert(lingxin_action_compat_publish_embedded(
               &state, "好的。json[not-json]", capture_action, &capture) == 0);
    assert(capture.count == 0U);
}

static void explicit_json_array_is_published(void)
{
    lingxin_action_compat_state_t state = {0};
    capture_t capture = {0};
    const char *json = "[{\"action\":\"FORWARD\",\"params\":{\"SPEED\":50}}]";
    const char *text = "好的，收到。json[{\"action\":\"FORWARD\",\"params\":{\"SPEED\":50}}]";

    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    assert(cJSON_IsArray(root));
    assert(cJSON_IsObject(cJSON_GetArrayItem(root, 0)));
    assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetArrayItem(root, 0), "action")));
    cJSON_Delete(root);
    assert(lingxin_action_compat_publish_json(
               &state, json, "direct_test", capture_action, &capture) == 1);
    lingxin_action_compat_reset(&state);
    memset(&capture, 0, sizeof(capture));
    assert(lingxin_action_compat_publish_embedded(
               &state, text, capture_action, &capture) == 1);
    assert(capture.count == 1U);
    assert(strcmp(capture.source, "agent_response_json") == 0);
    assert(strcmp(capture.action, "FORWARD") == 0);
    assert(strstr(capture.json, "\"SPEED\":50") != NULL);
}

static void fenced_json_array_is_published(void)
{
    lingxin_action_compat_state_t state = {0};
    capture_t capture = {0};

    assert(lingxin_action_compat_publish_embedded(
               &state,
               "好的。```json\n[{\"action\":\"TURN_LEFT\"}]\n```",
               capture_action, &capture) == 1);
    assert(strcmp(capture.action, "TURN_LEFT") == 0);
}

static void formal_and_fallback_routes_are_deduplicated_per_round(void)
{
    lingxin_action_compat_state_t state = {0};
    capture_t capture = {0};
    const char *formal = "[{\"action\":\"FORWARD\",\"params\":{\"SPEED\":50}}]";
    const char *fallback = "好的。json[{\"action\":\"FORWARD\",\"params\":{\"SPEED\":50}}]";

    assert(lingxin_action_compat_publish_json(
               &state, formal, "action_list", capture_action, &capture) == 1);
    assert(lingxin_action_compat_publish_embedded(
               &state, fallback, capture_action, &capture) == 0);
    assert(capture.count == 1U);
    assert(state.duplicate_count == 1U);

    lingxin_action_compat_reset(&state);
    assert(lingxin_action_compat_publish_embedded(
               &state, fallback, capture_action, &capture) == 1);
    assert(capture.count == 2U);
}

static void malformed_action_elements_are_skipped(void)
{
    lingxin_action_compat_state_t state = {0};
    capture_t capture = {0};

    assert(lingxin_action_compat_publish_json(
               &state,
               "[{\"params\":{}},{\"action\":17},{\"action\":\"STOP_MOVING\"}]",
               "action_list", capture_action, &capture) == 1);
    assert(capture.count == 1U);
    assert(strcmp(capture.action, "STOP_MOVING") == 0);
}

int main(void)
{
    plain_or_malformed_text_is_not_executable();
    explicit_json_array_is_published();
    fenced_json_array_is_published();
    formal_and_fallback_routes_are_deduplicated_per_round();
    malformed_action_elements_are_skipped();
    return 0;
}
