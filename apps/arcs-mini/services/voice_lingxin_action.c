#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TAG "lingxin-action"
#include "lisa_log.h"

#include "cJSON.h"
#include "service_volume.h"
#include "voice_lingxin_action.h"
#include "voice_msg.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_voice_action.h"
#endif

#define DEFAULT_VOLUME_STEP 10

bool voice_lingxin_action_is_supported(const char *code)
{
    static const char *const codes[] = {
        "INCREASE_VOLUME_BY", "INCREASE_VOLUM_BY", "DECREASE_VOLUME_BY",
        "DECREASE_VOLUM_BY", "CHANGE_VOLUME_TO", "MAX_VOLUME", "MUTE", "UNMUTE"
    };
    if (code == NULL) return false;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        if (strcmp(code, codes[i]) == 0) return true;
    }
    return false;
}

static int parse_volume(const cJSON *action, int default_value, int *volume)
{
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(action, "params");
    if (params == NULL) params = cJSON_GetObjectItem(action, "parameters");
    const cJSON *value = params ? cJSON_GetObjectItemCaseSensitive(params, "VOLUME") : NULL;

    if (value == NULL) {
        *volume = default_value;
        return default_value >= 0 ? 0 : -1;
    }
    if (cJSON_IsNumber(value)) {
        *volume = value->valueint;
    } else if (cJSON_IsString(value) && value->valuestring != NULL) {
        char *end = NULL;
        errno = 0;
        long parsed = strtol(value->valuestring, &end, 10);
        if (errno != 0 || end == value->valuestring || *end != '\0') {
            return -1;
        }
        *volume = (int)parsed;
    } else {
        return -1;
    }

    return (*volume >= 0 && *volume <= 100) ? 0 : -1;
}

static int handle_action(const cJSON *item)
{
    const cJSON *action_json = cJSON_GetObjectItemCaseSensitive(item, "action");
    const char *action = cJSON_GetStringValue(action_json);
    int volume = 0;

    if (action == NULL) {
        return -1;
    }

    if (strcmp(action, "INCREASE_VOLUME_BY") == 0 || strcmp(action, "INCREASE_VOLUM_BY") == 0) {
        if (parse_volume(item, DEFAULT_VOLUME_STEP, &volume) != 0) {
            return -1;
        }
        service_volume_adjust(volume);
    } else if (strcmp(action, "DECREASE_VOLUME_BY") == 0 || strcmp(action, "DECREASE_VOLUM_BY") == 0) {
        if (parse_volume(item, DEFAULT_VOLUME_STEP, &volume) != 0) {
            return -1;
        }
        service_volume_adjust(-volume);
    } else if (strcmp(action, "CHANGE_VOLUME_TO") == 0) {
        if (parse_volume(item, -1, &volume) != 0) {
            return -1;
        }
        service_volume_set(volume);
    } else if (strcmp(action, "MAX_VOLUME") == 0) {
        service_volume_set(100);
    } else if (strcmp(action, "MUTE") == 0) {
        service_volume_mute(true);
    } else if (strcmp(action, "UNMUTE") == 0) {
        service_volume_mute(false);
    } else {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_v2_gait_action_t motion;
        if (service_voice_action_from_cloud_code(action, &motion)) return 0;
#endif
        LOGW("unsupported action: %s", action);
        return -1;
    }

    LOGI("handled action=%s volume=%d muted=%s", action,
         service_volume_get(), service_volume_is_muted() ? "yes" : "no");
    return 0;
}

int voice_lingxin_action_handle_json(const char *json)
{
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    int handled = 0;
    int failed = 0;

    if (root == NULL) {
        LOGE("invalid action_list JSON");
        return -1;
    }

    if (cJSON_IsArray(root)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, root) {
            if (cJSON_IsObject(item) && handle_action(item) == 0) {
                handled++;
            } else {
                failed++;
            }
        }
    } else if (cJSON_IsObject(root)) {
        if (handle_action(root) == 0) {
            handled++;
        } else {
            failed++;
        }
    } else {
        failed++;
    }

    cJSON_Delete(root);
    return handled > 0 && failed == 0 ? 0 : -1;
}

static void action_list_cb(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    (void)unused;
    (void)msg_id;
    (void)len;
    (void)user_data;
    if (data != NULL) {
        (void)voice_lingxin_action_handle_json((const char *)data);
    }
}

void voice_lingxin_action_init(void)
{
    voice_msg_sub(VOICE_MSG_CLOUD_ACTION_LIST, action_list_cb, NULL);
    LOGI("Lingxin volume action handler ready");
}
