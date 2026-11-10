#include "lingxin_server.h"
#include "lingxin_server_config.h"

#define TAG "lingxin.server"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
#include "chat_api.h"
#include "chat_runtime_context.h"
#include "chat_state_machine.h"
#include "lingxin_protocol_manager.h"
#endif
#include "cJSON.h"
#include "lisa_kv.h"
#include "lisa_mem.h"
#include "lisa_log.h"
#include "lisa_time.h"
#include "lisa_thread.h"
#include "lisa_websocket.h"
#include "lingxin_common.h"
#include "lingxin_memory.h"
#include "lingxin_tls_utils.h"
#include "lingxin_trace.h"
#include "lingxin_adapter.h"
#include "sys_wifi.h"
#include "sys_network_manager.h"
#include "voice_msg.h"
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
#include "service_codex_status.h"
#include "sysutils.h"

static lingxin_asr_probe_t s_asr_probe __psram_bss__;
static TickType_t s_asr_probe_vad;

bool lingxin_server_asr_probe_active(void)
{
    bool active;
    taskENTER_CRITICAL();
    active = s_asr_probe.active;
    taskEXIT_CRITICAL();
    return active;
}

void lingxin_server_asr_probe_read(lingxin_asr_probe_t *result)
{
    taskENTER_CRITICAL();
    *result = s_asr_probe;
    taskEXIT_CRITICAL();
}
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
static uint32_t s_lingxin_offline_owner_generation;
static uint32_t s_lingxin_requested_owner_generation;
static char s_lingxin_requested_task_id[160];
#endif

void lingxin_server_task_started(const char *task_id)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    taskENTER_CRITICAL();
    if (task_id && s_lingxin_requested_task_id[0] &&
        strcmp(task_id, s_lingxin_requested_task_id) == 0) {
        s_lingxin_offline_owner_generation = s_lingxin_requested_owner_generation;
    }
    taskEXIT_CRITICAL();
#else
    (void)task_id;
#endif
}

#define LINGXIN_DIAG_BUILD_ID "lingxin_cloud_vad_v2_task_isolation_20260911"
#define LINGXIN_CHAT_SESSION_ID_KEY "lingxin.chat.session_id"
#define LINGXIN_AUDIO_SEND_FRAME_BYTES 1280
#define LINGXIN_GOODBYE_EXIT_TIMEOUT_MS 8000U
#define LINGXIN_PLAYBACK_READY_WAIT_MS 5000U
#define LINGXIN_PLAYBACK_READY_POLL_MS 20U

typedef struct {
    lisa_ws_t *ws;
} lingxin_probe_ctx_t;

static lingxin_probe_ctx_t s_probe_ctx;

static void lingxin_probe_ws_event(lisa_ws_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->what) {
    case LISA_WS_ON_CONNECTING:
        lingxin_trace_set("probe:connecting");
        LOGI("[Lingxin][probe] websocket connecting");
        break;
    case LISA_WS_ON_CONNECTED:
        lingxin_trace_set("probe:connected");
        LOGI("[Lingxin][probe] websocket connected");
        break;
    case LISA_WS_ON_DISCONNECTED:
        lingxin_trace_set("probe:disconnected");
        LOGI("[Lingxin][probe] websocket disconnected");
        break;
    case LISA_WS_ON_ERROR:
        lingxin_trace_set("probe:error");
        LOGE("[Lingxin][probe] websocket error");
        break;
    case LISA_WS_ON_HEADER:
    default:
        break;
    }
}

static void lingxin_probe_ws_data(lisa_ws_data_t *data)
{
    if (data == NULL || data->buf == NULL || data->len == 0) {
        return;
    }

    if (data->type == LISA_WS_TEXT) {
        lingxin_trace_set("probe:text_received");
        LOGI("[Lingxin][probe] rx text len=%u", (unsigned)data->len);
    } else {
        lingxin_trace_set("probe:bin_received");
        LOGI("[Lingxin][probe] rx bin len=%u", (unsigned)data->len);
    }
}

static int s_lingxin_inited;

static char *lingxin_get_app_id(void)
{
    return (char *)lingxin_server_config_app_id();
}

static char *lingxin_get_license(void)
{
    return (char *)lingxin_server_config_license();
}

static char *lingxin_get_sn(void)
{
    return (char *)lingxin_server_config_sn();
}

static char *lingxin_get_app_code(void)
{
    return (char *)lingxin_server_config_app_code();
}

static char *lingxin_get_device_code(void)
{
    return (char *)lingxin_server_config_device_code();
}

#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
static bool s_lingxin_chat_running;
static bool s_lingxin_chat_finished_for_round;
static uint32_t s_lingxin_exit_sequence;
static bool s_lingxin_exit_callback_complete = true;
static uint32_t s_lingxin_chat_generation;
static uint32_t s_lingxin_goodbye_generation;
static volatile bool s_lingxin_goodbye_pending;
static TimerHandle_t s_lingxin_goodbye_timer;
static lingxin_goodbye_stats_t s_lingxin_goodbye_stats;
static bool s_lingxin_publish_ui_session_events = true;
static uint32_t s_lingxin_timing_round_seq;
static uint32_t s_lingxin_timing_round_start_ms;
static uint32_t s_lingxin_timing_last_ms;
static uint32_t s_lingxin_timing_step_seq;
static bool s_lingxin_timing_round_active;

static void lingxin_goodbye_reset(const char *reason)
{
    taskENTER_CRITICAL();
    bool was_pending = s_lingxin_goodbye_pending;

    s_lingxin_goodbye_pending = false;
    s_lingxin_goodbye_generation = 0U;
    taskEXIT_CRITICAL();
    if (s_lingxin_goodbye_timer != NULL &&
        xTimerIsTimerActive(s_lingxin_goodbye_timer) != pdFALSE) {
        (void)xTimerStop(s_lingxin_goodbye_timer, 0);
    }
    if (was_pending) {
        LOGI("[Lingxin] SAY_GOODBYE cleared: %s", reason != NULL ? reason : "unknown");
    }
}

static void lingxin_goodbye_dispatch_exit(const char *reason)
{
    uint32_t generation = s_lingxin_goodbye_generation;

    if (!s_lingxin_goodbye_pending || generation == 0U ||
        generation != s_lingxin_chat_generation) {
        return;
    }
    lingxin_goodbye_reset(reason);
    taskENTER_CRITICAL();
    s_lingxin_goodbye_stats.dispatched++;
    s_lingxin_goodbye_stats.last_dispatched_generation = generation;
    taskEXIT_CRITICAL();
    lingxin_trace_set("life:goodbye_exit");
    LOGI("[Lingxin] SAY_GOODBYE exit generation=%u reason=%s",
         (unsigned)generation, reason != NULL ? reason : "unknown");
    if (voice_msg_pub_timeout(VOICE_MSG_CLOUD_MCP_CHAT_EXIT, NULL, 0, 0) != 0) {
        taskENTER_CRITICAL();
        s_lingxin_goodbye_stats.publish_failures++;
        taskEXIT_CRITICAL();
        LOGE("[Lingxin] SAY_GOODBYE exit event publish failed");
    }
}

static void lingxin_goodbye_timeout_cb(TimerHandle_t timer)
{
    (void)timer;
    taskENTER_CRITICAL();
    s_lingxin_goodbye_stats.timeouts++;
    taskEXIT_CRITICAL();
    LOGW("[Lingxin] SAY_GOODBYE playback completion timeout");
    lingxin_goodbye_dispatch_exit("playback_timeout");
}

static void lingxin_goodbye_arm(void)
{
    uint32_t epoch = voice_chat_current_epoch();
    taskENTER_CRITICAL();
    s_lingxin_goodbye_stats.received++;
    s_lingxin_goodbye_stats.last_received_generation = s_lingxin_chat_generation;
    s_lingxin_goodbye_stats.last_received_epoch = epoch;
    taskEXIT_CRITICAL();
    if (s_lingxin_chat_generation == 0U) {
        LOGW("[Lingxin] ignore SAY_GOODBYE without active generation");
        return;
    }

    taskENTER_CRITICAL();
    s_lingxin_goodbye_generation = s_lingxin_chat_generation;
    s_lingxin_goodbye_pending = true;
    taskEXIT_CRITICAL();
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* The goodbye audio still belongs to this generation. Retire only ESR
     * controls; a late goodbye must not close a newer keyword's window. */
    service_offline_action_close_online_window(s_lingxin_offline_owner_generation);
#endif
    if (s_lingxin_goodbye_timer == NULL) {
        s_lingxin_goodbye_timer = xTimerCreate(
            "lx.goodbye", pdMS_TO_TICKS(LINGXIN_GOODBYE_EXIT_TIMEOUT_MS),
            pdFALSE, NULL, lingxin_goodbye_timeout_cb);
    }
    if (s_lingxin_goodbye_timer == NULL ||
        xTimerChangePeriod(s_lingxin_goodbye_timer,
                           pdMS_TO_TICKS(LINGXIN_GOODBYE_EXIT_TIMEOUT_MS), 0) != pdPASS) {
        LOGE("[Lingxin] SAY_GOODBYE fallback timer unavailable");
    }
    lingxin_trace_set("life:goodbye_armed");
    LOGI("[Lingxin] SAY_GOODBYE armed generation=%u; wait for PLAY_END",
         (unsigned)s_lingxin_goodbye_generation);
}

static void lingxin_goodbye_complete_after_playback(void)
{
    if (!s_lingxin_goodbye_pending ||
        s_lingxin_goodbye_generation != s_lingxin_chat_generation) {
        return;
    }
    lingxin_goodbye_dispatch_exit("playback_complete");
}

static void lingxin_trace_int(const char *key, int value)
{
    char trace_line[64];

    snprintf(trace_line, sizeof(trace_line), "%s=%d", key, value);
    lingxin_trace_set(trace_line);
}

static void lingxin_trace_str_prefix(const char *key, const char *value)
{
    char trace_line[128];

    if (value == NULL) {
        value = "";
    }

    snprintf(trace_line, sizeof(trace_line), "%s=%.96s", key, value);
    lingxin_trace_set(trace_line);
}

static const char *lingxin_diag_state_name(int state)
{
    switch ((ChatState)state) {
    case State_Idle: return "IDLE";
    case State_Welcome: return "WELCOME";
    case State_Upload_Init: return "LISTENING_INIT";
    case State_Upload_Transfer: return "LISTENING";
    case State_Download_Init: return "SPEAKING_INIT";
    case State_Download_Play: return "SPEAKING";
    case State_Download_End: return "SPEAKING_END";
    case State_Task_Complete: return "TASK_COMPLETE";
    case State_Terminate: return "TERMINATE";
    case State_Exit: return "EXIT";
    case State_NoVoice_Start: return "TEXT_START";
    case State_NoVoice_Terminate: return "TEXT_TERMINATE";
    default: return "UNKNOWN";
    }
}

static void lingxin_server_diag_timing_log(const char *stage, const char *reason,
                                           int value, const char *detail)
{
    uint32_t now_ms = lisa_os_get_tick_ms();
    uint32_t elapsed_ms = s_lingxin_timing_round_active ? (now_ms - s_lingxin_timing_round_start_ms) : 0U;
    uint32_t delta_ms = s_lingxin_timing_step_seq > 0U ? (now_ms - s_lingxin_timing_last_ms) : 0U;
    int state = (int)get_current_chat_state();

    if (!s_lingxin_timing_round_active) {
        return;
    }

    s_lingxin_timing_step_seq++;
    s_lingxin_timing_last_ms = now_ms;

    LOGI("[LX-HD] dialog=%u step=%u t=%ums delta=+%ums stage=%s value=%d state=%s(%d) detail=%s reason=%s running=%d responding=%d",
         (unsigned)s_lingxin_timing_round_seq,
         (unsigned)s_lingxin_timing_step_seq,
         (unsigned)elapsed_ms,
         (unsigned)delta_ms,
         stage ? stage : "",
         value,
         lingxin_diag_state_name(state),
         state,
         detail ? detail : "-",
         reason ? reason : "",
         s_lingxin_chat_running ? 1 : 0,
         isVoiceChatResponding() ? 1 : 0);
}

int lingxin_next_task_id(char *task_id, size_t task_id_size)
{
    static unsigned int allocating;
    static int next_id, reserved_until;
    const char *sn = lingxin_server_config_sn();
    int session_id = 0;

    if (task_id == NULL || task_id_size == 0U || sn == NULL || sn[0] == '\0') {
        return -1;
    }

    unsigned int expected = 0U;
    if (!__atomic_compare_exchange_n(&allocating, &expected, 1U, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -1;
    if (next_id >= reserved_until) {
        if (lisa_kv_get_int(LINGXIN_CHAT_SESSION_ID_KEY, &session_id) != 0 || session_id < 0)
            session_id = 0;
        /* Persist the reservation before issuing any ID. A reboot skips the
         * unused suffix, and continuous rounds don't write flash each time. */
        if (session_id > INT_MAX - 32 ||
            lisa_kv_set_int(LINGXIN_CHAT_SESSION_ID_KEY, session_id + 32) != 0) {
            __atomic_store_n(&allocating, 0U, __ATOMIC_RELEASE);
            LOGE("[Lingxin] failed to reserve chat task ids");
            return -1;
        }
        next_id = session_id;
        reserved_until = session_id + 32;
    }
    session_id = ++next_id;
    __atomic_store_n(&allocating, 0U, __ATOMIC_RELEASE);

    int written = snprintf(task_id, task_id_size, "%s^%d^1", sn, session_id);
    if (written < 0 || (size_t)written >= task_id_size) {
        LOGE("[Lingxin] generated task id is too long");
        return -1;
    }
    return 0;
}

static void lingxin_publish_chat_started(void)
{
    int ret;

    if (s_lingxin_chat_running) {
        return;
    }
    if (s_lingxin_chat_finished_for_round) {
        lingxin_trace_set("life:session_start_ignored_after_finish");
        LOGW("[Lingxin] ignore late session start after round finished");
        return;
    }

    s_lingxin_chat_running = true;
    __atomic_store_n(&s_lingxin_exit_callback_complete, false, __ATOMIC_RELEASE);
    lingxin_trace_set("life:session_start");
    if (s_lingxin_publish_ui_session_events) {
        ret = voice_msg_pub_timeout(VOICE_MSG_CLOUD_SESSION_STARTING, NULL, 0, 0);
        if (ret != 0) {
            lingxin_trace_set("life:session_start_msg_drop");
            LOGW("[Lingxin] drop UI session start event");
        }
    } else {
        lingxin_trace_set("life:ui_session_start_suppressed");
        LOGW("[Lingxin] UI session start event suppressed for freeze bisection");
    }
}

static void lingxin_publish_chat_finished(void)
{
    int ret;

    if (!s_lingxin_chat_running) {
        return;
    }

    s_lingxin_chat_running = false;
    s_lingxin_chat_finished_for_round = true;
    lingxin_goodbye_reset("session_finished");
    /* CHAT EXIT/ERROR is the whole-session boundary. task_ended alone is only
     * a round boundary in continuous half-duplex mode, so keep the real-time
     * Wi-Fi policy across automatic start_task rounds and restore it here. */
    (void)sys_wifi_set_realtime_mode(false);
    lingxin_trace_set("life:session_finish");
    if (s_lingxin_publish_ui_session_events) {
        ret = voice_msg_pub_timeout(VOICE_MSG_CLOUD_SESSION_FINISHED, NULL, 0, 0);
        if (ret != 0) {
            lingxin_trace_set("life:session_finish_msg_drop");
            LOGW("[Lingxin] drop UI session finish event");
        }
    } else {
        lingxin_trace_set("life:ui_session_finish_suppressed");
        LOGW("[Lingxin] UI session finish event suppressed for freeze bisection");
    }
}

static void lingxin_publish_text_triplet(uint32_t start_msg, uint32_t update_msg, uint32_t end_msg, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    if (voice_msg_pub_timeout(start_msg, NULL, 0, 0) != 0) {
        lingxin_trace_set("life:text_start_msg_drop");
    }
    if (voice_msg_pub_timeout(update_msg, (void *)text, strlen(text) + 1, 0) != 0) {
        lingxin_trace_set("life:text_update_msg_drop");
    }
    if (voice_msg_pub_timeout(end_msg, NULL, 0, 0) != 0) {
        lingxin_trace_set("life:text_end_msg_drop");
    }
}

static bool lingxin_emotion_code_valid(const char *code)
{
    static const char *const codes[] = {
        "happy", "sad", "angry", "fearful", "surprised", "disgusted", "neutral",
    };
    if (code == NULL) return false;
    for (size_t i = 0U; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        if (strcmp(code, codes[i]) == 0) return true;
    }
    return false;
}

static int lingxin_publish_emotion_value(const cJSON *value)
{
    int published = 0;
    if (cJSON_IsString(value)) {
        const char *text = cJSON_GetStringValue(value);
        if (text != NULL && (text[0] == '[' || text[0] == '{')) {
            cJSON *decoded = cJSON_Parse(text);
            if (decoded != NULL) {
                published = lingxin_publish_emotion_value(decoded);
                cJSON_Delete(decoded);
            }
        } else if (lingxin_emotion_code_valid(text)) {
            voice_msg_pub(VOICE_MSG_CLOUD_MCP_EMOJI, (void *)text, strlen(text) + 1U);
            published = 1;
        }
    } else if (cJSON_IsArray(value)) {
        int count = cJSON_GetArraySize(value);
        for (int i = 0; i < count && i < 8; ++i) {
            published += lingxin_publish_emotion_value(cJSON_GetArrayItem(value, i));
        }
    } else if (cJSON_IsObject(value)) {
        const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, "type"));
        if (lingxin_emotion_code_valid(code)) {
            voice_msg_pub(VOICE_MSG_CLOUD_MCP_EMOJI, (void *)code, strlen(code) + 1U);
            published = 1;
        }
    }
    return published;
}

static void lingxin_handle_text_output(const char *payload)
{
    cJSON *root;
    cJSON *type;
    cJSON *result;
    const char *type_str;
    const char *result_str;

    if (payload == NULL || payload[0] == '\0') {
        lingxin_trace_set("life:text_empty");
        return;
    }

    root = cJSON_Parse(payload);
    if (root == NULL) {
        lingxin_trace_set("life:text_parse_fail");
        lingxin_trace_str_prefix("life:text_raw", payload);
        LOGW("[Lingxin] text output parse failed: %.96s", payload);
        return;
    }

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    type_str = cJSON_GetStringValue(type);
    result_str = cJSON_GetStringValue(result);

    lingxin_trace_str_prefix("life:text_type", type_str);
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
    if (lingxin_server_asr_probe_active()) {
        TickType_t now = xTaskGetTickCount();
        taskENTER_CRITICAL();
        if (type_str && strcmp(type_str, "asr_final_text") == 0 && result_str &&
            !s_asr_probe.final_received && strlen(result_str) < sizeof(s_asr_probe.text)) {
            memcpy(s_asr_probe.text, result_str, strlen(result_str) + 1U);
            s_asr_probe.final_received = true;
            if (s_asr_probe_vad != 0)
                s_asr_probe.vad_to_final_ms = (now - s_asr_probe_vad) * portTICK_PERIOD_MS;
        } else if (type_str && strcmp(type_str, "agent_response_text") == 0) {
            s_asr_probe.agent_text = true;
        } else if (type_str && strcmp(type_str, "action_list") == 0) {
            s_asr_probe.actions = true;
        }
        taskEXIT_CRITICAL();
        cJSON_Delete(root);
        return;
    }
#endif

    if (type_str == NULL) {
        LOGW("[Lingxin] text output missing type: %.96s", payload);
    } else if (strcmp(type_str, "asr_final_text") == 0) {
        lingxin_trace_str_prefix("life:asr", result_str);
        lingxin_publish_text_triplet(VOICE_MSG_CLOUD_IAT_START,
                                     VOICE_MSG_CLOUD_IAT_UPDATE,
                                     VOICE_MSG_CLOUD_IAT_END,
                                     result_str);
    } else if (strcmp(type_str, "agent_response_text") == 0) {
        lingxin_trace_str_prefix("life:agent", result_str);
        lingxin_publish_text_triplet(VOICE_MSG_CLOUD_TTS_TEXT_START,
                                     VOICE_MSG_CLOUD_TTS_TEXT_UPDATE,
                                     VOICE_MSG_CLOUD_TTS_TEXT_END,
                                     result_str);
    } else if (strcmp(type_str, "hit_custom_fallback") == 0) {
        /* A no-input fallback is allowed to play once, but it must not enter
         * another automatic listening round or the service will repeat the
         * same prompt forever. TaskComplete will take the single-round exit. */
        update_current_context(CTX_FIELD_SINGLE_ROUND, (ContextValue){.boolean = true});
        lingxin_trace_set("life:fallback_exit_after_playback");
        LOGI("[Lingxin] fallback response will exit after playback");
    } else if (strcmp(type_str, "emotion") == 0 || strcmp(type_str, "emotion_list") == 0 ||
               strcmp(type_str, "emotion_output") == 0) {
        cJSON *decoded = result_str ? cJSON_Parse(result_str) : NULL;
        const cJSON *emotions = decoded != NULL ? decoded : result;
        int published = lingxin_publish_emotion_value(emotions);
        LOGI("[Lingxin] emotion output published=%d", published);
        cJSON_Delete(decoded);
    } else if (strcmp(type_str, "action_list") == 0) {
        lingxin_trace_set("life:action_list");
        /* Both providers encode result as an array or as a JSON string.
         * Publish one complete element per event so volume and gait consumers
         * share the same wire contract without dropping params/parameters. */
        cJSON *decoded = result_str ? cJSON_Parse(result_str) : NULL;
        const cJSON *actions = result_str ? decoded : result;
        int count = cJSON_IsArray(actions) ? cJSON_GetArraySize(actions) : 1;
        if (count > 32) {
            LOGW("[Lingxin] action_list too large, rejected");
        } else {
            for (int i = 0; i < count; ++i) {
                const cJSON *item = cJSON_IsArray(actions) ? cJSON_GetArrayItem(actions, i) : actions;
                if (cJSON_IsObject(item) &&
                    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item, "action")) &&
                    lingxin_publish_emotion_value(item) > 0) {
                    continue;
                }
                if (!cJSON_IsObject(item) ||
                    !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item, "action"))) {
                    LOGW("[Lingxin] malformed action element");
                    continue;
                }
                const char *action_code = cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(item, "action"));
                if (action_code != NULL && strcmp(action_code, "SAY_GOODBYE") == 0) {
                    lingxin_goodbye_arm();
                    continue;
                }
                char *serialized = cJSON_PrintUnformatted(item);
                if (serialized != NULL) {
                    if (voice_msg_pub(VOICE_MSG_CLOUD_ACTION_LIST, serialized, strlen(serialized) + 1U) != 0) {
                        LOGW("[Lingxin] action element publish failed, index=%d", i);
                    }
                    cJSON_free(serialized);
                }
            }
        }
        cJSON_Delete(decoded);
    } else {
        LOGW("[Lingxin] unknown text output type=%s", type_str);
    }

    cJSON_Delete(root);
}

static void lingxin_report_transport_exit(int exit_code)
{
    if (!s_lingxin_chat_running ||
        (exit_code != EXIT_REASON_WEBSOCKET_DISCONNECT &&
         exit_code != EXIT_REASON_WEBSOCKET_CONNECTION_FAILED)) return;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    taskENTER_CRITICAL();
    uint32_t owner = s_lingxin_offline_owner_generation;
    if (owner == 0U && s_lingxin_requested_task_id[0] != '\0') {
        /* A connected socket can fail before the start_task acknowledgment.
         * Start clears the request ID before retiring an older session. */
        owner = s_lingxin_requested_owner_generation;
    }
    taskEXIT_CRITICAL();
    if (!service_offline_action_online_current(owner)) return;
#endif
    /* This SDK exit also covers business ACK/TTS timeouts. Request a network
     * recheck without confusing a failed conversation with a lost bearer. */
    LOGW("[Lingxin] conversation connection/response failed, exit=%d; recheck network", exit_code);
    sys_network_report_transport_failure();
}

static void lingxin_chat_life_cycle_listener(ChatLifeCycleEvent event, void *payload)
{
    LOGI("[Lingxin] chat lifecycle event=%d", event);
    lingxin_trace_int("life:event", event);

    switch (event) {
    case CHAT_LIFE_CYCLE_EVENT_EXIT: {
        uint32_t exit_generation = s_lingxin_chat_generation;
        ExitPayload *exit_payload = (ExitPayload *)payload;
        int exit_code = exit_payload != NULL ? exit_payload->exit_code : -1;
        if (exit_payload != NULL) {
            lingxin_trace_int("life:exit_code", exit_payload->exit_code);
            lingxin_trace_str_prefix("life:exit_reason", exit_payload->reason);
            LOGI("[Lingxin] chat exit code=%d reason=%s",
                 exit_payload->exit_code,
                 exit_payload->reason ? exit_payload->reason : "");
        }
        lingxin_server_diag_timing_end("session_exit", exit_code);
        lingxin_report_transport_exit(exit_code);
        lingxin_publish_chat_finished();
        taskENTER_CRITICAL();
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
        s_asr_probe.active = false;
#endif
        s_lingxin_goodbye_stats.session_exits++;
        s_lingxin_goodbye_stats.last_exit_generation = exit_generation;
        s_lingxin_goodbye_stats.last_exit_code = exit_code;
        taskEXIT_CRITICAL();
        __atomic_store_n(&s_lingxin_exit_callback_complete, true, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_lingxin_exit_sequence, 1U, __ATOMIC_ACQ_REL);
        break;
    }
    case CHAT_LIFE_CYCLE_EVENT_TEXT_OUT:
        lingxin_handle_text_output((const char *)payload);
        break;
    case CHAT_LIFE_CYCLE_EVENT_PLAY_END:
        /* PLAY_END only closes the current TTS output. In continuous mode the
         * SDK immediately enters the next upload round on the same websocket. */
        lingxin_server_diag_timing_mark("tts_playback_end", 0);
        lingxin_goodbye_complete_after_playback();
        break;
    case CHAT_LIFE_CYCLE_EVENT_CHAT_PHASE_CHANGE: {
        ChatPhaseChangePayload *phase_payload = (ChatPhaseChangePayload *)payload;
        int phase = phase_payload ? (int)phase_payload->phase_code : -1;
        lingxin_trace_int("life:phase", phase);
        LOGI("[Lingxin] chat phase=%d", phase);
        if (phase == CHAT_PHASE_STARTING || phase == CHAT_PHASE_INPUTING ||
            phase == CHAT_PHASE_THINKING || phase == CHAT_PHASE_OUTPUTING) {
            lingxin_publish_chat_started();
        } else if (phase == CHAT_PHASE_STANDBY) {
            /* Download_End is also mapped to STANDBY by the SDK before it
             * decides whether to continue or exit. Treating that transient
             * phase as a whole-session finish races the continuous-mode
             * restart and can make the next round look inactive/no-reply.
             * EXIT/ERROR are the authoritative session terminal events. */
            lingxin_server_diag_timing_mark("round_standby", 0);
        }
        break;
    }
    case CHAT_LIFE_CYCLE_EVENT_ERROR:
        lingxin_trace_str_prefix("life:error", (const char *)payload);
        LOGE("[Lingxin] chat lifecycle error: %s", payload ? (const char *)payload : "");
        lingxin_server_diag_timing_end("session_error", -1);
        lingxin_publish_chat_finished();
        break;
    case CHAT_LIFE_CYCLE_EVENT_SCHEDULE_EMIT:
    default:
        break;
    }
}
#endif

void lingxin_server_tts_playback_complete(void)
{
#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    lingxin_goodbye_complete_after_playback();
#endif
}

void lingxin_server_get_goodbye_stats(lingxin_goodbye_stats_t *stats)
{
    if (stats == NULL) return;
    memset(stats, 0, sizeof(*stats));
#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    taskENTER_CRITICAL();
    *stats = s_lingxin_goodbye_stats;
    stats->pending = s_lingxin_goodbye_pending;
    stats->pending_generation = s_lingxin_goodbye_generation;
    taskEXIT_CRITICAL();
#endif
}

void lingxin_server_note_chat_exit_handled(int result)
{
#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    taskENTER_CRITICAL();
    s_lingxin_goodbye_stats.handled++;
    s_lingxin_goodbye_stats.last_stop_result = result;
    taskEXIT_CRITICAL();
#else
    (void)result;
#endif
}

int lingxin_server_available(void)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    return 1;
#else
    return 0;
#endif
}

int lingxin_server_init(void)
{
#ifndef CONFIG_VOICE_LINK_LINGXIN
    LOGI("Lingxin link inactive, skip init");
    return 0;
#elif !defined(CONFIG_LINGXIN_ENGINE_ENABLE)
    LOGE("[Lingxin] SDK engine disabled, skip init");
    return -1;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    LOGE("[Lingxin] SDK engine basic profile, chat API disabled");
    return -1;
#else
    LOGI("[Lingxin] init begin");
    lingxin_trace_set("init:begin");
    lingxin_server_config_dump();
    if (!lingxin_server_config_ready()) {
        LOGE("[Lingxin] init blocked: auth config is incomplete");
        return -1;
    }

    if (s_lingxin_inited) {
        LOGI("[Lingxin] init skipped: already initialized");
        return 0;
    }

    LOGI("[Lingxin] get default voice chat props");
    lingxin_trace_set("init:get_default_props");
    VoiceChatInitProps props = get_voice_chat_init_default_props();
    props.auth_app_id_get_func = lingxin_get_app_id;
    props.auth_license_get_func = lingxin_get_license;
    props.auth_sn_get_func = lingxin_get_sn;
    props.auth_app_code_get_func = lingxin_get_app_code;
    props.device_code_get_func = lingxin_get_device_code;
    props.chat_life_cycle_event_listener = lingxin_chat_life_cycle_listener;
    /* lisa_websocket services one queued frame per loop.  A 40 ms PCM frame
     * keeps the producer below that loop's sustainable message rate. */
    props.send_uni_size = LINGXIN_AUDIO_SEND_FRAME_BYTES;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    /* Clean capture already owns bounded pre-connect audio. Do not hide
     * another 10 seconds in the engine FIFO when the transport slows down. */
    props.send_cbuf_scale = 4;
#else
    props.send_cbuf_scale = 256;
#endif
#ifdef CONFIG_LINGXIN_ENABLE_SCHEDULE_TASK
    props.is_schedule_task_on = 1;
#else
    props.is_schedule_task_on = 0;
#endif
#ifdef CONFIG_LINGXIN_ENABLE_LOG_UPLOAD
    props.is_log_upload_on = 1;
#else
    props.is_log_upload_on = 0;
#endif
    LOGI("[Lingxin] voice_chat_init start, schedule=%d, log_upload=%d",
         props.is_schedule_task_on,
         props.is_log_upload_on);
    lingxin_trace_set("init:voice_chat_init_start");

    int ret = voice_chat_init(&props);
    LOGI("[Lingxin] voice_chat_init returned: %d", ret);
    lingxin_trace_set(ret == 0 ? "init:voice_chat_init_ok" : "init:voice_chat_init_fail");
    if (ret != 0) {
        LOGE("[Lingxin] voice_chat_init failed: %d", ret);
        return ret;
    }

    s_lingxin_inited = 1;
    lingxin_trace_set("init:done");
    LOGI("[Lingxin] voice chat initialized");
    return 0;
#endif
}

int lingxin_server_connect(void)
{
#ifndef CONFIG_VOICE_LINK_LINGXIN
    LOGI("Lingxin link inactive, skip connect");
    return 0;
#else
    LOGI("[Lingxin] connect requested");
    return lingxin_server_init();
#endif
}

void lingxin_server_suppress_current_tts(const char *reason)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE)
    lingxin_audio_suppress_current_tts(reason);
#else
    (void)reason;
#endif
}

#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
static bool lingxin_start_request_current(uint32_t owner, uint32_t epoch)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(owner)) return false;
#else
    (void)owner;
#endif
    return voice_chat_current_epoch() == epoch;
}

static int lingxin_wait_playback_input_ready(uint32_t owner, uint32_t epoch)
{
    /* Start callers run on lingxin.start, the query worker or the shell, not
     * main/voice.ebus. Keep this same request while the old output settles;
     * a new owner/task cancels it rather than inheriting its pending start. */
    for (uint32_t waited = 0U; ; waited += LINGXIN_PLAYBACK_READY_POLL_MS) {
        if (!lingxin_start_request_current(owner, epoch)) return -2;
        if (lingxin_audio_playback_input_ready()) return 0;
        if (waited >= LINGXIN_PLAYBACK_READY_WAIT_MS) break;
        lisa_thread_mdelay(LINGXIN_PLAYBACK_READY_POLL_MS);
    }
    LOGW("[Lingxin] new input deferred: previous playback cleanup incomplete");
    /* The caller retries transitional -2. A full cleanup deadline is a hard
     * failure for this wake request, not eight more five-second waits. */
    return -1;
}

static int lingxin_close_previous_voice_session(uint32_t owner, uint32_t *epoch)
{
    if (!lingxin_start_request_current(owner, *epoch)) return -2;
    uint32_t exit_sequence = __atomic_load_n(&s_lingxin_exit_sequence, __ATOMIC_ACQUIRE);

    /* A keyword starts a separate ASR task. Do not queue terminate_task behind
     * a long old TTS response: WillExit(false) marks a normal local exit, stops
     * the old recorder and closes its socket without waiting for its ACK.
     * The clean recorder's owner/capture-generation checks retain the new
     * wake's pre-recorded PCM. This runs only on the existing start worker. */
    lingxin_trace_set("chat:wakeup_close_old_session");
    int stop_ret = 0;
    if (get_current_chat_state() != State_Idle) {
        stop_ret = lingxin_server_chat_stop();
    } else if (__atomic_load_n(&s_lingxin_exit_callback_complete, __ATOMIC_ACQUIRE) &&
               isVoiceChatInited()) {
        /* Preserve the existing idle-handler recovery. If another closer
         * owns it, this returns false and the barrier waits for its release.
         * Pending EXIT callbacks only wait; never send WillExit from Idle. */
        (void)module_voiceChat_exit();
    }
    uint32_t closed_epoch = voice_chat_current_epoch();
    if (!lingxin_start_request_current(owner, closed_epoch)) return -2;
    if (stop_ret != 0) {
        LOGW("[Lingxin] wake restart aborted: previous session stop failed=%d", stop_ret);
        return -1;
    }

    /* The exit call may return before recorder/player termination callbacks.
     * Idle alone or an empty handler alone does not prove both have finished.
     * A failed close/reset must not fall through the stale-state force-reset
     * branches below, or retry eight times and silently discard new speech. */
    for (uint32_t waited = 0U; ; waited += LINGXIN_PLAYBACK_READY_POLL_MS) {
        if (!lingxin_start_request_current(owner, closed_epoch)) return -2;
        if (get_current_chat_state() == State_Idle && !isVoiceChatInited() &&
            lingxin_audio_playback_input_ready() &&
            (__atomic_load_n(&s_lingxin_exit_sequence, __ATOMIC_ACQUIRE) != exit_sequence ||
             __atomic_load_n(&s_lingxin_exit_callback_complete, __ATOMIC_ACQUIRE))) {
            *epoch = closed_epoch;
            lingxin_server_diag_timing_mark("INT02_local_wakeup_closed", 0);
            return 0;
        }
        if (waited >= LINGXIN_PLAYBACK_READY_WAIT_MS) break;
        lisa_thread_mdelay(LINGXIN_PLAYBACK_READY_POLL_MS);
    }
    LOGW("[Lingxin] wake restart aborted: previous session cleanup incomplete");
    return -1;
}
#endif

static int lingxin_server_chat_start_internal(const char *text, bool continue_after_text,
                                            uint32_t requested_owner, bool use_current_owner, bool asr_probe)
{
#ifndef CONFIG_VOICE_LINK_LINGXIN
    LOGI("Lingxin link inactive, skip chat start");
    return 0;
#elif !defined(CONFIG_LINGXIN_ENGINE_ENABLE)
    LOGE("[Lingxin] SDK engine disabled, skip chat start");
    return -1;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    LOGE("[Lingxin] SDK engine basic profile, chat start disabled");
    return -1;
#else
    char task_id[160];
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (use_current_owner) requested_owner = service_offline_action_generation();
    /* Reject a retired worker before suppressing or closing newer playback. */
    if (!service_offline_action_online_current(requested_owner)) return -2;
#endif

    LOGI("[Lingxin][DIAG] build=%s", LINGXIN_DIAG_BUILD_ID);
    LOGI("[Lingxin] chat start requested");
    /* Retire queued PlayEnd before mutating the new session's goodbye/task
     * ownership. The adapter serializes this boundary with PlayEnd dispatch. */
    if (text == NULL && get_current_chat_state() != State_Idle)
        lingxin_audio_suppress_current_tts("local_wakeup_new_task");
    uint32_t start_epoch = voice_chat_current_epoch();
    int playback_ready = lingxin_wait_playback_input_ready(requested_owner, start_epoch);
    if (playback_ready != 0) return playback_ready;
    if (text == NULL || continue_after_text) {
        /* Idle is published before the EXIT application callback. Even an
         * already-idle wake or device-query handoff must pass the barrier;
         * a fully idle first wake returns immediately without closing anything.
         * Automatic SDK continuous rounds do not call this start function. */
        lingxin_server_diag_timing_mark("INT01_local_wakeup_interrupt", (int)get_current_chat_state());
        int close_ret = lingxin_close_previous_voice_session(requested_owner, &start_epoch);
        if (close_ret != 0) return close_ret;
    }
    lingxin_goodbye_reset("new_chat");
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    taskENTER_CRITICAL();
    s_lingxin_offline_owner_generation = 0;
    s_lingxin_requested_task_id[0] = '\0';
    taskEXIT_CRITICAL();
#endif
    s_lingxin_chat_generation++;
    if (s_lingxin_chat_generation == 0U) {
        s_lingxin_chat_generation = 1U;
    }
    lingxin_trace_clear();
    lingxin_trace_set(LINGXIN_DIAG_BUILD_ID);
    lingxin_trace_set("chat:begin");
    if (text != NULL) {
        lingxin_server_diag_timing_begin("text_round");
    }
    lingxin_server_diag_timing_mark("chat_start_requested", text ? 1 : 0);
    LOGI("[Lingxin] chat start snapshot: inited=%d chat_running=%d sdk_handler=%d ai_responding=%d sm_state=%d",
         s_lingxin_inited,
         s_lingxin_chat_running ? 1 : 0,
         isVoiceChatInited() ? 1 : 0,
         isVoiceChatResponding() ? 1 : 0,
         (int)get_current_chat_state());
    lingxin_trace_int("chat:sm_state", (int)get_current_chat_state());
    LOGI("[Lingxin] chat start step=init");
    int ret = lingxin_server_init();
    lingxin_server_diag_timing_mark("chat_init_done", ret);
    if (ret != 0) {
        LOGE("[Lingxin] chat start blocked: init ret=%d", ret);
        return ret;
    }

    ChatState chat_state = get_current_chat_state();
    /* A closed socket is not proof that its audio has stopped. In particular,
     * never force Exit to Idle while a failed player reset is being retried. */
    if (!lingxin_start_request_current(requested_owner, start_epoch) ||
        !lingxin_audio_playback_input_ready()) return -2;
    if (chat_state != State_Idle &&
        !s_lingxin_chat_running &&
        s_lingxin_chat_finished_for_round) {
        LOGW("[Lingxin] recover stale finished chat state=%d before restart", (int)chat_state);
        lingxin_trace_set("chat:recover_stale_finished_state");
        state_machine_force_reset_to_idle();
        chat_state = get_current_chat_state();
    }
    if (chat_state == State_Exit && !isVoiceChatResponding()) {
        /* Audio cleanup was confirmed above; recover only stale protocol
         * state, never use force-idle as a substitute for stopping output. */
        LOGW("[Lingxin] recover stale exit state before wakeup");
        lingxin_trace_set("chat:recover_stale_exit");
        state_machine_force_reset_to_idle();
        chat_state = get_current_chat_state();
    }
    if (chat_state != State_Idle && !isVoiceChatConnected()) {
        LOGW("[Lingxin] recover stale chat state=%d without websocket before restart", (int)chat_state);
        lingxin_trace_set("chat:recover_stale_state_without_ws");
        state_machine_force_reset_to_idle();
        chat_state = get_current_chat_state();
    }
    if (chat_state == State_Terminate || chat_state == State_Exit ||
        chat_state == State_NoVoice_Terminate || chat_state == State_NoVoice_Start) {
        LOGW("[Lingxin] chat start deferred while state=%d is completing", (int)chat_state);
        lingxin_trace_set("chat:start_blocked_transitional_state");
        return -2;
    }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (requested_owner != service_offline_action_generation()) return -2;
#endif
    s_lingxin_chat_finished_for_round = false;
    LOGI("[Lingxin] chat start step=get_start_props");
    lingxin_trace_set("chat:get_start_props");
    StartNewChatProps props = get_start_new_chat_default_props();
    props.disable_welcome_audio = true;
    /* Query result text is a one-round temp_context input. On completion the
     * SDK consumes it, clears the task id and creates an ordinary audio round.
     * Generic text/shell calls retain their original single-round behavior. */
    props.single_round = asr_probe || (text != NULL && !continue_after_text);
    props.user_input = (char *)text;
    /* Official v2 Cloud VAD half-duplex uses the normal chat task. The
     * endpoint and max_sentence_silence select the Cloud VAD protocol. */
    props.task = "chat";
    /* Every start_task is a new protocol task. Explicit voice wakes have
     * already retired the old transport; text tasks keep the SDK transition. */
    if (lingxin_next_task_id(task_id, sizeof(task_id)) != 0) {
        lingxin_trace_set("chat:task_id_generate_fail");
        return -1;
    }
    props.task_id = task_id;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    taskENTER_CRITICAL();
    s_lingxin_requested_owner_generation = requested_owner;
    memcpy(s_lingxin_requested_task_id, task_id, strlen(task_id) + 1U);
    taskEXIT_CRITICAL();
#endif
    LOGI("[Lingxin] new Cloud VAD v2 task id prepared, previous_state=%d", (int)chat_state);
    lingxin_trace_set("chat:task_id_protocol_ok");
    LOGI("[Lingxin][DIAG] start props snapshot: state=%d single_round=%d text_len=%u disable_welcome=%d task_id=%s",
         (int)chat_state,
         props.single_round ? 1 : 0,
         text ? (unsigned)strlen(text) : 0U,
         props.disable_welcome_audio ? 1 : 0,
         props.task_id ? props.task_id : "(null)");
    lingxin_trace_set("chat:disable_welcome_audio");
    lingxin_trace_set(text ? (continue_after_text ? "chat:query_then_voice" : "chat:text_single_round")
                           : "chat:voice_continuous");
    LOGI("[Lingxin] chat start step=start_new_chat");
    lingxin_trace_set("chat:start_new_chat_call");
    lingxin_server_diag_timing_mark("start_new_chat_call", props.single_round ? 1 : 0);
    /* Live A/B traces show DTIM power save dropping the AP link after about
     * nine seconds of continuous PCM upload, before Cloud VAD can answer with
     * vad_end/vad_exit. Keep Wi-Fi awake for the complete chat session. */
    ret = sys_wifi_set_realtime_mode(true);
    if (ret != 0) {
        LOGW("[Lingxin] failed to enter WiFi realtime mode: %d", ret);
        lingxin_server_diag_timing_mark("wifi_realtime_mode_failed", ret);
    } else {
        lingxin_server_diag_timing_mark("wifi_realtime_mode_enabled", 0);
    }
    if (!lingxin_start_request_current(requested_owner, start_epoch) ||
        !lingxin_audio_playback_input_ready()) {
        lingxin_trace_set("chat:playback_cleanup_pending");
        return -2;
    }
    lingxin_audio_set_codex_prefill(text != NULL && !continue_after_text && !use_current_owner);
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
    if (asr_probe) {
        if (!service_codex_status_office_mode()) {
            (void)sys_wifi_set_realtime_mode(false);
            return -1;
        }
        taskENTER_CRITICAL();
        memset(&s_asr_probe, 0, sizeof(s_asr_probe));
        s_asr_probe.vad_to_final_ms = UINT32_MAX;
        s_asr_probe_vad = 0;
        s_asr_probe.active = true;
        taskEXIT_CRITICAL();
        uint32_t gate = lingxin_server_audio_uplink_gate_close("office_asr_probe");
        lingxin_server_audio_uplink_gate_open(gate, "office_asr_probe");
    }
#endif
    ret = start_new_chat(&props);
    if (ret != 0) lingxin_audio_set_codex_prefill(false);
    lingxin_trace_set(ret == 0 ? "chat:start_new_chat_ret_ok" : "chat:start_new_chat_ret_fail");
    lingxin_server_diag_timing_mark("start_new_chat_ret", ret);
    LOGI("[Lingxin] start_new_chat ret=%d", ret);
    if (ret != 0) {
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
        if (asr_probe) {
            taskENTER_CRITICAL();
            s_asr_probe.active = false;
            taskEXIT_CRITICAL();
        }
#endif
        (void)sys_wifi_set_realtime_mode(false);
    }
    return ret;
#endif
}

int lingxin_server_chat_start(void)
{
    return lingxin_server_chat_start_internal(NULL, false, 0U, true, false);
}

#ifdef CONFIG_APP_CODEX_VOICE_INPUT
int lingxin_server_asr_probe_start(void)
{
    if (!service_codex_status_office_mode() || lingxin_server_chat_is_active()) return -1;
    return lingxin_server_chat_start_internal(NULL, false, 0U, true, true);
}
#endif

int lingxin_server_chat_start_owned(uint32_t owner_generation)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (owner_generation == 0U) return -2;
#endif
    return lingxin_server_chat_start_internal(NULL, false, owner_generation, owner_generation == 0U, false);
}

int lingxin_server_chat_start_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    return lingxin_server_chat_start_internal(text, false, 0U, true, false);
}

int lingxin_server_chat_start_owned_text(const char *text, uint32_t owner_generation)
{
    if (!text || !text[0]) return -1;
    return lingxin_server_chat_start_internal(text, false, owner_generation, false, false);
}

int lingxin_server_chat_start_query_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    return lingxin_server_chat_start_internal(text, true, 0U, true, false);
}

int lingxin_server_chat_end_record(void)
{
#ifndef CONFIG_VOICE_LINK_LINGXIN
    LOGI("Lingxin link inactive, skip chat end record");
    return 0;
#elif !defined(CONFIG_LINGXIN_ENGINE_ENABLE)
    LOGI("[Lingxin] SDK engine disabled, skip chat end record");
    return 0;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    LOGI("[Lingxin] SDK engine basic profile, skip chat end record");
    return 0;
#else
    if (!s_lingxin_inited) {
        LOGI("[Lingxin] chat end record skipped: not initialized");
        return 0;
    }

    ChatState chat_state = get_current_chat_state();
    if (chat_state != State_Upload_Init && chat_state != State_Upload_Transfer) {
        LOGI("[Lingxin] chat end record skipped: state=%d is not uploading", (int)chat_state);
        return 0;
    }

    LOGI("[Lingxin] chat end record requested");
    int ret = stop_chat_record(NULL);
    LOGI("[Lingxin] stop_chat_record ret=%d", ret);
    return ret;
#endif
}

int lingxin_server_chat_stop(void)
{
#ifndef CONFIG_VOICE_LINK_LINGXIN
    LOGI("Lingxin link inactive, skip chat stop");
    return 0;
#elif !defined(CONFIG_LINGXIN_ENGINE_ENABLE)
    LOGI("[Lingxin] SDK engine disabled, skip chat stop");
    return 0;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    LOGI("[Lingxin] SDK engine basic profile, skip chat stop");
    return 0;
#else
    if (!s_lingxin_inited) {
        LOGI("[Lingxin] chat stop skipped: not initialized");
        return 0;
    }

    ExitChatProps props = get_exit_chat_default_props();
    props.disable_close_ws_immediately = false;
    LOGI("[Lingxin] chat stop requested");
    int ret = exit_chat(&props);
    LOGI("[Lingxin] exit_chat ret=%d", ret);
    return ret;
#endif
}

bool lingxin_server_chat_is_active(void)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    return s_lingxin_chat_running;
#else
    return false;
#endif
}

lingxin_dialog_phase_t lingxin_server_dialog_phase(void)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    switch (get_current_chat_state()) {
        case State_Idle:
            return LINGXIN_DIALOG_PHASE_IDLE;

        case State_Upload_Init:
        case State_Upload_Transfer:
            return LINGXIN_DIALOG_PHASE_LISTENING;

        case State_Welcome:
        case State_Download_Init:
        case State_Download_Play:
        case State_Download_End:
            return LINGXIN_DIALOG_PHASE_SPEAKING;

        case State_NoVoice_Start:
        case State_NoVoice_Terminate:
        case State_Terminate:
        case State_Exit:
        case State_Task_Complete:
        default:
            return LINGXIN_DIALOG_PHASE_TRANSITION;
    }
#else
    return LINGXIN_DIALOG_PHASE_UNAVAILABLE;
#endif
}

uint32_t lingxin_server_audio_uplink_gate_close(const char *reason)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    return lingxin_audio_uplink_gate_close(reason);
#else
    (void)reason;
    return 0U;
#endif
}

void lingxin_server_audio_uplink_gate_open(uint32_t generation, const char *reason)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    lingxin_audio_uplink_gate_open(generation, reason);
#else
    (void)generation;
    (void)reason;
#endif
}

uint32_t lingxin_server_audio_uplink_gate_hold(const char *reason)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    return lingxin_audio_uplink_gate_hold(reason);
#else
    (void)reason;
    return 0U;
#endif
}

bool lingxin_server_audio_uplink_gate_is_open(uint32_t *generation)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    return lingxin_audio_uplink_gate_is_open(generation);
#else
    if (generation != NULL) {
        *generation = 0U;
    }
    return true;
#endif
}

int lingxin_server_chat_state(void)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    return (int)get_current_chat_state();
#else
    return -1;
#endif
}

void lingxin_server_diag_timing_begin(const char *reason)
{
#if !(defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC) && defined(CONFIG_LINGXIN_DIALOG_TIMELINE_LOG))
    (void)reason;
#else
    s_lingxin_timing_round_seq++;
    s_lingxin_timing_round_start_ms = lisa_os_get_tick_ms();
    s_lingxin_timing_last_ms = s_lingxin_timing_round_start_ms;
    s_lingxin_timing_step_seq = 0U;
    s_lingxin_timing_round_active = true;
    lingxin_server_diag_timing_log("wakeup", reason, 0, NULL);
#endif
}

void lingxin_server_diag_timing_mark(const char *stage, int value)
{
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
    if (lingxin_server_asr_probe_active() && strcmp(stage, "S07_vad_end") == 0) {
        TickType_t now = xTaskGetTickCount();
        taskENTER_CRITICAL();
        s_asr_probe_vad = now;
        taskEXIT_CRITICAL();
    }
#endif
#if !(defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC) && defined(CONFIG_LINGXIN_DIALOG_TIMELINE_LOG))
    (void)stage;
    (void)value;
#else
    lingxin_server_diag_timing_log(stage, NULL, value, NULL);
#endif
}

void lingxin_server_diag_timing_transition(int from_state, int to_state, int event)
{
#if !(defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC) && defined(CONFIG_LINGXIN_DIALOG_TIMELINE_LOG))
    (void)from_state;
    (void)to_state;
    (void)event;
#else
    char detail[80];

    snprintf(detail, sizeof(detail), "%s(%d)->%s(%d),event=%d",
             lingxin_diag_state_name(from_state), from_state,
             lingxin_diag_state_name(to_state), to_state, event);
    lingxin_server_diag_timing_log("state_transition", NULL, event, detail);
#endif
}

void lingxin_server_diag_timing_end(const char *stage, int value)
{
#if !(defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC) && defined(CONFIG_LINGXIN_DIALOG_TIMELINE_LOG))
    (void)stage;
    (void)value;
#else
    lingxin_server_diag_timing_log(stage, NULL, value, NULL);
    s_lingxin_timing_round_active = false;
#endif
}

int lingxin_server_ws_probe_start(void)
{
#ifndef CONFIG_VOICE_LINK_LINGXIN
    LOGI("Lingxin link inactive, skip websocket probe");
    return -1;
#else
    const char *host = lingxin_server_config_cloud_host();
    int port = lingxin_server_config_cloud_port();
    static char port_buf[16];
    static char timestamp_buf[16];
    static char header_signature[128];
    static char header_sn[64];
    static char header_app_id[64];

    if (!lingxin_server_config_provisioned() || !lingxin_server_config_ready()) {
        LOGE("[Lingxin][probe] blocked: factory identity not provisioned");
        return -1;
    }

    if (host == NULL || host[0] == '\0' || port <= 0) {
        LOGE("[Lingxin][probe] invalid cloud host/port");
        return -1;
    }

    if (s_probe_ctx.ws != NULL) {
        lisa_ws_err_e cleanup = lisa_ws_cleanup(s_probe_ctx.ws);
        if (cleanup != LISA_WS_OK) {
            LOGW("[Lingxin][probe] previous connection still closing: %d", cleanup);
            return -1;
        }
        s_probe_ctx.ws = NULL;
    }

    snprintf(port_buf, sizeof(port_buf), "%d", port);
    snprintf(timestamp_buf, sizeof(timestamp_buf), "%lld", (long long)lingxin_get_timestamp_s() * 1000LL);
    snprintf(header_sn, sizeof(header_sn), "%s", lingxin_server_config_sn());
    snprintf(header_app_id, sizeof(header_app_id), "%s", lingxin_server_config_app_id());
    const char *signature = generateSignature(header_sn, lingxin_server_config_license(), header_app_id, timestamp_buf);
    if (!signature || signature[0] == '\0') {
        lingxin_trace_set("probe:signature_fail");
        LOGE("[Lingxin][probe] generateSignature failed");
        return -1;
    }
    snprintf(header_signature, sizeof(header_signature), "%s", signature);
    lingxin_free((void *)signature);

    lingxin_trace_clear();
    lingxin_trace_set("probe:begin");

    lisa_ws_request_t req = {
        .scheme = (uint8_t *)"wss",
        .host = (uint8_t *)host,
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
        .path = (uint8_t *)"/" WEBSOCKET_CLOUD_VAD_CHAT_PATH,
#else
        .path = (uint8_t *)"/" WEBSOCKET_CHAT_PATH,
#endif
        .port = (uint8_t *)port_buf,
        /* lisa_websocket passes this value to nopoll in seconds. */
        .timeout = 10,
        .extra_header = NULL,
        .user = &s_probe_ctx,
        .on_event = lingxin_probe_ws_event,
        .on_data = lingxin_probe_ws_data,
    };

    static char extra_header[512];
    snprintf(extra_header, sizeof(extra_header),
             "signature: %s\r\n"
             "sn: %s\r\n"
             "app_id: %s\r\n"
             "timestamp: %s",
             header_signature, header_sn, header_app_id, timestamp_buf);
    req.extra_header = extra_header;

    lingxin_trace_set("probe:ws_init_call");
    s_probe_ctx.ws = lisa_ws_init(&req);
    if (s_probe_ctx.ws == NULL) {
        lingxin_trace_set("probe:ws_init_fail");
        LOGE("[Lingxin][probe] lisa_ws_init failed");
        return -1;
    }

    lingxin_trace_set("probe:ws_init_ok");
    LOGI("[Lingxin][probe] connect %s://%s:%d%s", req.scheme, host, port, req.path);
    lisa_ws_err_e ret = lisa_ws_connect(s_probe_ctx.ws);
    if (ret != LISA_WS_OK) {
        lingxin_trace_set("probe:ws_connect_fail");
        LOGE("[Lingxin][probe] lisa_ws_connect failed: %d", ret);
        return -1;
    }

    lingxin_trace_set("probe:ws_connect_ok");
    return 0;
#endif
}
