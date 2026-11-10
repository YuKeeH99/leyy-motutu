#include "service_voice_action.h"
#include "service_button.h"
#include "service_offline_action.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "cJSON.h"
#include "app_tone.h"
#include "battery/battery_ui.h"
#include "lisa_log.h"
#include "lisa_mutex.h"
#include "lisa_thread.h"
#include "lingxin_server.h"
#include "lingxin_server_config.h"
#include "project_version.h"
#include "tone.h"

#include "service_v2_gait.h"
#include "service_voice_action_policy.h"
#include "service_voice_action_queue.h"
#include "service_voice_query_policy.h"
#include "voice_msg.h"
#include "voice_lingxin_action.h"
#include "voice_player.h"

#define TAG "v2.voice-action"

/*
 * 同一个动作在这个窗口内重复命中就丢掉。
 *
 * 云端偶尔会把同一句话再发一次，或者用户话说了一半又补一句；重复触发"前进"
 * 会让刚起步的步态从头再来，看起来像卡住。窗口取得比一个走行周期短，用户真想
 * 连着下两次同样的命令仍然是通的。
 */
#define VOICE_ACTION_DEBOUNCE_MS 1000U
#define VOICE_QUERY_QUEUE_DEPTH  3U
#define VOICE_QUERY_TASK_STACK   (12U * 1024U)
#define VOICE_QUERY_SETTLE_MS    150U
#define VOICE_QUERY_STOP_POLL_MS 100U
#define VOICE_QUERY_STOP_POLLS   80U
#define VOICE_QUERY_RETRY_MS     100U
#define VOICE_QUERY_RETRY_COUNT  20U

typedef struct {
    service_v2_gait_action_t action;
    const char *const *words;
    size_t word_count;
} voice_action_rule_t;

static const char *const s_words_stop[] = {"停下", "停止", "别动", "站住", "停一下"};
static const char *const s_words_prone[] = {"趴下", "卧倒", "躺下", "趴着", "趴"};
static const char *const s_words_run_backward[] = {"往后跑", "向后跑", "倒着跑", "跑步后退"};
static const char *const s_words_run_forward[] = {"往前跑", "向前跑", "跑步前进", "向前快跑"};
static const char *const s_words_turn_left[] = {"左转", "向左转", "往左转", "往左边转"};
static const char *const s_words_turn_right[] = {"右转", "向右转", "往右转", "往右边转"};
static const char *const s_words_backward[] = {"后退", "往后走", "向后退", "退后", "倒车"};
static const char *const s_words_forward[] = {"前进", "往前走", "向前走", "前走"};
static const char *const s_words_stand[] = {"起立", "站起", "起身", "站好", "起来"};
static const char *const s_words_frontflip[] = {"前空翻", "翻跟头", "翻一个跟头", "翻一次", "翻一下",
                                              "连续翻", "连续翻跟头", "连翻两次", "翻两个跟头", "翻两次"};
static const char *const s_words_handstand[] = {"倒立表演", "倒立", "表演倒立"};
static const char *const s_words_tennis_serve[] = {"网球发球", "发个网球", "挥拍发球", "发球"};
static const char *const s_words_tennis_forehand[] = {"网球正手", "正手击球", "正手"};
static const char *const s_words_tennis_backhand[] = {"网球单反", "单反击球", "单反"};
static const char *const s_words_tennis_double_backhand[] = {"网球双反", "双反击球", "双反"};

/*
 * 顺序即优先级，不能随便调。
 *
 * - "停"类排最前：任何时候都要能刹住。
 * - 跑步必须排在普通前后移动前："往前跑"不能被"往前"抢走。
 * - 所有方向词排在"起立"前：否则"往前跑起来"会被判成起立。
 */
static const voice_action_rule_t s_rules[] = {
    {SERVICE_V2_GAIT_ACTION_STOP, s_words_stop, sizeof(s_words_stop) / sizeof(s_words_stop[0])},
    /* 这里只自检动作族；实际倒立/前空翻的 1/2 段由云端指令码决定。 */
    {SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP, s_words_frontflip,
     sizeof(s_words_frontflip) / sizeof(s_words_frontflip[0])},
    {SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP, s_words_handstand,
     sizeof(s_words_handstand) / sizeof(s_words_handstand[0])},
    {SERVICE_V2_GAIT_ACTION_TENNIS_SERVE, s_words_tennis_serve,
     sizeof(s_words_tennis_serve) / sizeof(s_words_tennis_serve[0])},
    {SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND, s_words_tennis_double_backhand,
     sizeof(s_words_tennis_double_backhand) / sizeof(s_words_tennis_double_backhand[0])},
    {SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND, s_words_tennis_backhand,
     sizeof(s_words_tennis_backhand) / sizeof(s_words_tennis_backhand[0])},
    {SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND, s_words_tennis_forehand,
     sizeof(s_words_tennis_forehand) / sizeof(s_words_tennis_forehand[0])},
    {SERVICE_V2_GAIT_ACTION_PRONE, s_words_prone, sizeof(s_words_prone) / sizeof(s_words_prone[0])},
    {SERVICE_V2_GAIT_ACTION_RUN_BACKWARD, s_words_run_backward,
     sizeof(s_words_run_backward) / sizeof(s_words_run_backward[0])},
    {SERVICE_V2_GAIT_ACTION_RUN_FORWARD, s_words_run_forward,
     sizeof(s_words_run_forward) / sizeof(s_words_run_forward[0])},
    {SERVICE_V2_GAIT_ACTION_TURN_LEFT, s_words_turn_left, sizeof(s_words_turn_left) / sizeof(s_words_turn_left[0])},
    {SERVICE_V2_GAIT_ACTION_TURN_RIGHT, s_words_turn_right, sizeof(s_words_turn_right) / sizeof(s_words_turn_right[0])},
    {SERVICE_V2_GAIT_ACTION_BACKWARD, s_words_backward, sizeof(s_words_backward) / sizeof(s_words_backward[0])},
    {SERVICE_V2_GAIT_ACTION_FORWARD, s_words_forward, sizeof(s_words_forward) / sizeof(s_words_forward[0])},
    {SERVICE_V2_GAIT_ACTION_STAND, s_words_stand, sizeof(s_words_stand) / sizeof(s_words_stand[0])},
};

static bool s_ready;
static uint32_t s_matched_count;
static uint32_t s_suppressed_count;
static uint32_t s_cloud_action_count;
static uint32_t s_cloud_unknown_count;
static uint32_t s_queue_dropped_count;
static service_v2_gait_action_t s_last_match = SERVICE_V2_GAIT_ACTION_STOP;
static service_v2_gait_action_t s_debounce_action;
static uint32_t s_debounce_cycles;
static TickType_t s_debounce_tick;
static bool s_debounce_valid;
static service_voice_action_queue_t s_action_queue;
static lisa_mutex_t *s_action_lock;
static bool s_action_dispatching;
static uint32_t s_action_generation;
static QueueHandle_t s_query_queue;
static lisa_thread_t *s_query_thread;
static uint32_t s_query_generation;
static bool s_query_inflight;

typedef struct {
    service_voice_query_t query;
    uint32_t generation;
    uint32_t owner_generation;
} voice_query_request_t;

static const char *voice_query_name(service_voice_query_t query)
{
    switch (query) {
    case SERVICE_VOICE_QUERY_SYSTEM_VERSION:
        return "system_version";
    case SERVICE_VOICE_QUERY_DEVICE_CODE:
        return "device_code_suffix";
    case SERVICE_VOICE_QUERY_POWER_STATUS:
        return "power_status";
    case SERVICE_VOICE_QUERY_NONE:
    default:
        return "none";
    }
}

static bool voice_query_is_current(const voice_query_request_t *request)
{
    return request != NULL &&
           service_offline_action_online_current(request->owner_generation) &&
           request->generation ==
               __atomic_load_n(&s_query_generation, __ATOMIC_ACQUIRE);
}

static bool voice_query_build_text(service_voice_query_t query, char *text, size_t text_size)
{
    switch (query) {
    case SERVICE_VOICE_QUERY_SYSTEM_VERSION:
        return service_voice_query_format_version(text, text_size, PROJECT_VERSION_STR);
    case SERVICE_VOICE_QUERY_DEVICE_CODE:
        return service_voice_query_format_device_code(
            text, text_size, lingxin_server_config_device_code());
    case SERVICE_VOICE_QUERY_POWER_STATUS: {
        voice_msg_battery_info_t battery = {0};
        bool available = battery_ui_get_info(&battery) &&
                         battery.status != VOICE_MSG_BATTERY_STATUS_NO_BATTERY &&
                         battery.status != VOICE_MSG_BATTERY_STATUS_UNKNOWN;
        bool charging = battery.status == VOICE_MSG_BATTERY_STATUS_CHARGING;
        bool full = battery.status == VOICE_MSG_BATTERY_STATUS_CHARGE_DONE;
        return service_voice_query_format_power(text, text_size, battery.level,
                                                available, charging, full);
    }
    case SERVICE_VOICE_QUERY_NONE:
    default:
        return false;
    }
}

/*
 * The deployed cloud configuration emits GET_* action_list items, so the
 * device must turn the local value into speech.  Never start that text round
 * from the eBus callback and never overlap it with the voice task: first stop
 * the old chat and observe its complete Idle boundary, then start one isolated
 * text task whose result returns to normal continuous voice input. This keeps
 * the local value out of subsequent rounds' input and avoids the task collision
 * that previously closed the websocket with status 1006.
 */
static bool voice_query_stop_old_chat(const voice_query_request_t *request)
{
    lingxin_server_suppress_current_tts("device_status_query");

    int ret = lingxin_server_chat_stop();
    if (ret != 0) {
        LISA_LOGW(TAG, "query stop old chat failed: query=%s ret=%d",
                  voice_query_name(request->query), ret);
        return false;
    }

    for (uint32_t poll = 0U; poll < VOICE_QUERY_STOP_POLLS; ++poll) {
        if (!voice_query_is_current(request)) {
            LISA_LOGI(TAG, "discard stale query while stopping: %s",
                      voice_query_name(request->query));
            return false;
        }
        if (!lingxin_server_chat_is_active() &&
            lingxin_server_dialog_phase() == LINGXIN_DIALOG_PHASE_IDLE) {
            LISA_LOGI(TAG, "query old chat stopped: query=%s polls=%u",
                      voice_query_name(request->query), (unsigned)(poll + 1U));
            return true;
        }
        lisa_thread_mdelay(VOICE_QUERY_STOP_POLL_MS);
    }

    LISA_LOGW(TAG, "query old chat stop timeout: query=%s active=%d phase=%d",
              voice_query_name(request->query),
              lingxin_server_chat_is_active() ? 1 : 0,
              (int)lingxin_server_dialog_phase());
    return false;
}

bool service_voice_action_query_idle(void)
{
    return !__atomic_load_n(&s_query_inflight, __ATOMIC_ACQUIRE);
}

static bool voice_query_begin(const voice_query_request_t *request)
{
    taskENTER_CRITICAL();
    bool current = voice_query_is_current(request);
    if (current) __atomic_store_n(&s_query_inflight, true, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL();
    return current;
}

static void voice_query_finish_stale(void)
{
    /* The handoff barrier stays closed until the old SDK call's side effects
     * have drained. New online/offline starts must observe query_idle first,
     * so this global STOP cannot target a newer task. Never timeout-open this
     * barrier: a failed cleanup must remain fail-closed. Only this worker waits. */
    bool stop_accepted = false;
    while (lingxin_server_chat_is_active() ||
           lingxin_server_dialog_phase() != LINGXIN_DIALOG_PHASE_IDLE) {
        if (!stop_accepted) stop_accepted = lingxin_server_chat_stop() == 0;
        lisa_thread_mdelay(VOICE_QUERY_STOP_POLL_MS);
    }
}

static void voice_query_task(void *arg)
{
    (void)arg;
    voice_query_request_t request;
    char text[128];
    char prompt[256];

    for (;;) {
        if (xQueueReceive(s_query_queue, &request, portMAX_DELAY) != pdPASS) {
            continue;
        }

        /* Let the SDK/eBus action-list callback return before invoking any
         * synchronous player or chat lifecycle operation. */
        lisa_thread_mdelay(VOICE_QUERY_SETTLE_MS);
        if (!voice_query_is_current(&request)) {
            LISA_LOGI(TAG, "discard stale query=%s", voice_query_name(request.query));
            continue;
        }
        if (!voice_query_build_text(request.query, text, sizeof(text))) {
            LISA_LOGW(TAG, "format query response failed: %s", voice_query_name(request.query));
            continue;
        }

        int prompt_len = snprintf(
            prompt, sizeof(prompt),
            "[DEVICE_LOCAL_RESULT]请只朗读冒号后的内容，不要调用指令，不要输出JSON：%s",
            text);
        if (prompt_len < 0 || (size_t)prompt_len >= sizeof(prompt)) {
            LISA_LOGW(TAG, "query speech prompt overflow: %s", voice_query_name(request.query));
            continue;
        }

        if (!voice_query_begin(&request)) continue;
        if (!voice_query_stop_old_chat(&request)) {
            if (!voice_query_is_current(&request)) voice_query_finish_stale();
            __atomic_store_n(&s_query_inflight, false, __ATOMIC_RELEASE);
            continue;
        }

        int ret = -1;
        uint32_t attempt = 0U;
        for (; attempt < VOICE_QUERY_RETRY_COUNT; ++attempt) {
            if (!voice_query_is_current(&request)) {
                ret = -ECANCELED;
                break;
            }
            ret = lingxin_server_chat_start_query_text(prompt);
            if (!voice_query_is_current(&request)) {
                ret = -ECANCELED;
                break;
            }
            if (ret != -2) {
                break;
            }
            lisa_thread_mdelay(VOICE_QUERY_RETRY_MS);
        }
        if (!voice_query_is_current(&request)) voice_query_finish_stale();
        __atomic_store_n(&s_query_inflight, false, __ATOMIC_RELEASE);
        LISA_LOGI(TAG, "query speech round started: type=%s ret=%d attempts=%u",
                  voice_query_name(request.query), ret, (unsigned)(attempt + 1U));
    }
}

static bool voice_query_submit(service_voice_query_t query)
{
    voice_query_request_t request = {
        .query = query,
        .generation = __atomic_load_n(&s_query_generation, __ATOMIC_ACQUIRE),
        .owner_generation = service_offline_action_generation(),
    };

    if (s_query_queue == NULL || xQueueSend(s_query_queue, &request, 0U) != pdPASS) {
        LISA_LOGW(TAG, "query queue full: %s", voice_query_name(query));
        return false;
    }
    LISA_LOGI(TAG, "query queued: %s", voice_query_name(query));
    return true;
}

bool service_voice_action_match(const char *text, service_v2_gait_action_t *action)
{
    if (text == NULL || action == NULL || text[0] == '\0') {
        return false;
    }

    /*
     * UTF-8 下直接用 strstr 找中文子串是安全的：多字节序列的后续字节都 >= 0x80，
     * 不会和另一个字的首字节对齐出误匹配。
     */
    for (size_t r = 0U; r < sizeof(s_rules) / sizeof(s_rules[0]); ++r) {
        for (size_t w = 0U; w < s_rules[r].word_count; ++w) {
            if (strstr(text, s_rules[r].words[w]) != NULL) {
                *action = s_rules[r].action;
                return true;
            }
        }
    }

    return false;
}

/*
 * 云端指令码 -> 步态动作。
 *
 * 指令码是在 eAgent 控制台"指令集管理 -> 自定义指令集"里，每条指令的
 * "具体下发指令"那一栏填的字符串（只能英文/数字/下划线）。云端在
 * text_output 的 action_list 里把它原样发下来。
 *
 * 表里除了本工程实际配置的 DOG_* 之外，还收了一批常见写法：平台的系统预置
 * 指令集用的是 Forward 这类通名（官方文档 1.4.6 的例子就是
 * {"action":"Forward"}），控制台里也可能被人改成别的拼法。多认几个别名的代价
 * 只是几行常量，认不出来就得连回控制台改配置再发布一遍，不划算。
 *
 * 比较不区分大小写：控制台没有强制大小写，Forward / FORWARD 都该通。
 */
typedef struct {
    const char *code;
    service_v2_gait_action_t action;
} voice_action_code_t;

static const voice_action_code_t s_codes[] = {
    {"DOG_PUSHUP", SERVICE_V2_GAIT_ACTION_PUSHUP},
    {"DOG_PLAY_MUSIC", SERVICE_V2_GAIT_ACTION_MUSIC_BEAT},
    {"PUSHUP", SERVICE_V2_GAIT_ACTION_PUSHUP},
    {"DOG_BACKFLIP", SERVICE_V2_GAIT_ACTION_BACKFLIP},
    {"BACKFLIP", SERVICE_V2_GAIT_ACTION_BACKFLIP},
    {"DOG_STAND_UP", SERVICE_V2_GAIT_ACTION_STAND},
    {"STAND_UP", SERVICE_V2_GAIT_ACTION_STAND},
    {"STAND", SERVICE_V2_GAIT_ACTION_STAND},
    {"GET_UP", SERVICE_V2_GAIT_ACTION_STAND},

    {"DOG_LIE_DOWN", SERVICE_V2_GAIT_ACTION_PRONE},
    {"LIE_DOWN", SERVICE_V2_GAIT_ACTION_PRONE},
    {"LIEDOWN", SERVICE_V2_GAIT_ACTION_PRONE},
    {"PRONE", SERVICE_V2_GAIT_ACTION_PRONE},
    {"CROUCH", SERVICE_V2_GAIT_ACTION_PRONE},

    {"DOG_MOVE_FORWARD", SERVICE_V2_GAIT_ACTION_FORWARD},
    {"MOVE_FORWARD", SERVICE_V2_GAIT_ACTION_FORWARD},
    {"GO_FORWARD", SERVICE_V2_GAIT_ACTION_FORWARD},
    {"FORWARD", SERVICE_V2_GAIT_ACTION_FORWARD},

    {"DOG_RUN_FORWARD", SERVICE_V2_GAIT_ACTION_RUN_FORWARD},
    {"RUN_FORWARD", SERVICE_V2_GAIT_ACTION_RUN_FORWARD},
    {"GO_FORWARD_FAST", SERVICE_V2_GAIT_ACTION_RUN_FORWARD},

    {"DOG_MOVE_BACKWARD", SERVICE_V2_GAIT_ACTION_BACKWARD},
    {"MOVE_BACKWARD", SERVICE_V2_GAIT_ACTION_BACKWARD},
    {"GO_BACKWARD", SERVICE_V2_GAIT_ACTION_BACKWARD},
    {"BACKWARD", SERVICE_V2_GAIT_ACTION_BACKWARD},
    {"BACK", SERVICE_V2_GAIT_ACTION_BACKWARD},

    {"DOG_RUN_BACKWARD", SERVICE_V2_GAIT_ACTION_RUN_BACKWARD},
    {"RUN_BACKWARD", SERVICE_V2_GAIT_ACTION_RUN_BACKWARD},
    {"GO_BACKWARD_FAST", SERVICE_V2_GAIT_ACTION_RUN_BACKWARD},

    {"DOG_TURN_LEFT", SERVICE_V2_GAIT_ACTION_TURN_LEFT},
    {"TURN_LEFT", SERVICE_V2_GAIT_ACTION_TURN_LEFT},
    {"ROTATE_LEFT", SERVICE_V2_GAIT_ACTION_TURN_LEFT},
    {"LEFT", SERVICE_V2_GAIT_ACTION_TURN_LEFT},

    {"DOG_TURN_RIGHT", SERVICE_V2_GAIT_ACTION_TURN_RIGHT},
    {"TURN_RIGHT", SERVICE_V2_GAIT_ACTION_TURN_RIGHT},
    {"ROTATE_RIGHT", SERVICE_V2_GAIT_ACTION_TURN_RIGHT},
    {"RIGHT", SERVICE_V2_GAIT_ACTION_TURN_RIGHT},

    /* New semantic names preserve the tested one-/two-segment trajectories.
     * Legacy codes retain their physical behavior for deployed backends. */
    {"DOG_HANDSTAND", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"DOG_FLIP_ONCE", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"DOG_FLIP", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"FLIP_ONCE", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"ROLL_ONCE", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},

    {"DOG_FRONTFLIP", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"DOG_FLIP_CONTINUOUS", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"DOG_CONTINUOUS_FLIP", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"FLIP_CONTINUOUS", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"CONTINUOUS_FLIP", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},
    {"ROLL_DOUBLE", SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP},

    {"DOG_TENNIS_SERVE", SERVICE_V2_GAIT_ACTION_TENNIS_SERVE},
    {"TENNIS_SERVE", SERVICE_V2_GAIT_ACTION_TENNIS_SERVE},
    {"DOG_TENNIS_FOREHAND", SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND},
    {"TENNIS_FOREHAND", SERVICE_V2_GAIT_ACTION_TENNIS_FOREHAND},
    {"DOG_TENNIS_BACKHAND", SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND},
    {"TENNIS_BACKHAND", SERVICE_V2_GAIT_ACTION_TENNIS_BACKHAND},
    {"DOG_TENNIS_DOUBLE_BACKHAND", SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND},
    {"TENNIS_DOUBLE_BACKHAND", SERVICE_V2_GAIT_ACTION_TENNIS_DOUBLE_BACKHAND},

    {"DOG_HANDSHAKE", SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE},
    {"HANDSHAKE", SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE},
    {"DOG_GREETING", SERVICE_V2_GAIT_ACTION_GREETING},
    {"GREETING", SERVICE_V2_GAIT_ACTION_GREETING},
    {"DOG_SCRATCH", SERVICE_V2_GAIT_ACTION_SCRATCH},
    {"SCRATCH", SERVICE_V2_GAIT_ACTION_SCRATCH},
    {"DOG_SPIN", SERVICE_V2_GAIT_ACTION_SPIN},
    {"SPIN", SERVICE_V2_GAIT_ACTION_SPIN},
    {"DOG_SIT_DOWN", SERVICE_V2_GAIT_ACTION_SIT},
    {"SIT_DOWN", SERVICE_V2_GAIT_ACTION_SIT},
    {"DOG_KISS", SERVICE_V2_GAIT_ACTION_KISS},
    {"KISS", SERVICE_V2_GAIT_ACTION_KISS},
    {"DOG_DANCE", SERVICE_V2_GAIT_ACTION_DANCE},
    {"DANCE", SERVICE_V2_GAIT_ACTION_DANCE},
    {"DOG_JUMP", SERVICE_V2_GAIT_ACTION_JUMP},
    {"JUMP", SERVICE_V2_GAIT_ACTION_JUMP},
    {"DOG_WIGGLE_BUTT", SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT},
    {"WIGGLE_BUTT", SERVICE_V2_GAIT_ACTION_WIGGLE_BUTT},

    {"DOG_STOP", SERVICE_V2_GAIT_ACTION_STOP},
    {"STOP_MOVING", SERVICE_V2_GAIT_ACTION_STOP}, /* 控制台系统预置"移动控制"里用的就是这个码 */
    {"STOP", SERVICE_V2_GAIT_ACTION_STOP},
    {"HALT", SERVICE_V2_GAIT_ACTION_STOP},
};

/* 只比较 ASCII，指令码本来就限定英文/数字/下划线。 */
static bool code_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 'a' + 'A') : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 'a' + 'A') : *b;

        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool cloud_code_is_double_roll(const char *code)
{
    static const char *const double_codes[] = {
        "DOG_FRONTFLIP", "DOG_FLIP_CONTINUOUS", "DOG_CONTINUOUS_FLIP", "FLIP_CONTINUOUS",
        "CONTINUOUS_FLIP", "ROLL_DOUBLE",
    };

    for (size_t i = 0U; i < sizeof(double_codes) / sizeof(double_codes[0]); ++i) {
        if (code_equal(code, double_codes[i])) {
            return true;
        }
    }
    return false;
}

bool service_voice_action_from_cloud_code(const char *code, service_v2_gait_action_t *action)
{
    if (code == NULL || action == NULL || code[0] == '\0') {
        return false;
    }

    for (size_t i = 0U; i < sizeof(s_codes) / sizeof(s_codes[0]); ++i) {
        if (code_equal(code, s_codes[i].code)) {
            *action = s_codes[i].action;
            return true;
        }
    }

    return false;
}

/** 返回 true 表示这次命中与最近一条已成功入队的命令重复。 */
static bool debounce_matches(service_v2_gait_action_t action, uint32_t cycles)
{
    TickType_t now = xTaskGetTickCount();

    return s_debounce_valid && s_debounce_action == action && s_debounce_cycles == cycles &&
           (now - s_debounce_tick) < pdMS_TO_TICKS(VOICE_ACTION_DEBOUNCE_MS);
}

static void debounce_commit(service_v2_gait_action_t action, uint32_t cycles)
{
    s_debounce_valid = true;
    s_debounce_action = action;
    s_debounce_cycles = cycles;
    s_debounce_tick = xTaskGetTickCount();
}

/*
 * 走行周期数上限。云端参数是模型填的，不能直接信：一条 cycles=9999 会让狗走上
 * 几个小时，而且中途只能靠下一条指令打断。
 */
#define VOICE_ACTION_CLOUD_MAX_CYCLES 20U

static void voice_action_try_start_next(void)
{
    if (!service_offline_action_online_current(service_offline_action_generation())) return;
    service_voice_action_queue_item_t next;
    uint32_t generation;

    lisa_mutex_lock(s_action_lock, LISA_WAIT_FOREVER);
    if (s_action_dispatching || !service_voice_action_queue_peek(&s_action_queue, &next)) {
        lisa_mutex_unlock(s_action_lock);
        return;
    }
    s_action_dispatching = true;
    generation = s_action_generation;
    lisa_mutex_unlock(s_action_lock);

    int ret = service_v2_gait_submit_speed(next.action, next.cycles, next.speed_percent);
    bool canceled_during_submit = false;

    lisa_mutex_lock(s_action_lock, LISA_WAIT_FOREVER);
    if (generation != s_action_generation) {
        canceled_during_submit = ret == 0;
    } else if (ret == 0) {
        (void)service_voice_action_queue_mark_active(&s_action_queue);
    }
    s_action_dispatching = false;
    uint32_t pending = service_voice_action_queue_count(&s_action_queue);
    lisa_mutex_unlock(s_action_lock);

    if (canceled_during_submit) {
        (void)service_v2_gait_submit(SERVICE_V2_GAIT_ACTION_STOP, 0U);
        LISA_LOGI(TAG, "cancel action submitted across task boundary: %s",
                  service_v2_gait_action_name(next.action));
        return;
    }
    if (ret != 0) {
        /* Keep the item at the queue head.  A currently running gait (touch,
         * IMU or the previous cloud action) will publish FINISHED and retry. */
        LISA_LOGI(TAG, "defer %s until gait is idle: %d",
                  service_v2_gait_action_name(next.action), ret);
        return;
    }

    debounce_commit(next.action, next.cycles);
    LISA_LOGI(TAG, "start queued action %s (cycles=%u speed=%u pending=%u)",
              service_v2_gait_action_name(next.action), (unsigned)next.cycles,
              (unsigned)next.speed_percent, (unsigned)pending);
}

static void voice_action_cancel_all(const char *source)
{
    service_v2_gait_status_t gait = {0};

    lisa_mutex_lock(s_action_lock, LISA_WAIT_FOREVER);
    uint32_t dropped = service_voice_action_queue_count(&s_action_queue);
    bool had_active = service_voice_action_queue_active(&s_action_queue);
    service_voice_action_queue_clear(&s_action_queue);
    s_action_generation++;
    s_debounce_valid = false;
    voice_chat_action_hold_cancel();
    lisa_mutex_unlock(s_action_lock);

    if (service_v2_gait_get_status(&gait) == 0 && gait.busy) {
        int ret = service_v2_gait_submit(SERVICE_V2_GAIT_ACTION_STOP, 0U);
        LISA_LOGI(TAG, "%s cancels gait=%s queued=%u active=%d ret=%d", source,
                  service_v2_gait_action_name(gait.current), (unsigned)dropped,
                  had_active ? 1 : 0, ret);
    } else if (dropped != 0U || had_active) {
        LISA_LOGI(TAG, "%s clears queued=%u active=%d", source, (unsigned)dropped,
                  had_active ? 1 : 0);
    }
}

/** 命中之后的共用收尾：去抖、进入上层 FIFO、记账。 */
static void voice_action_dispatch(service_v2_gait_action_t action, uint32_t cycles,
                                  uint8_t speed_percent, const char *source)
{
    if (action == SERVICE_V2_GAIT_ACTION_STOP) {
        voice_action_cancel_all(source);
        return;
    }

    if (debounce_matches(action, cycles)) {
        s_suppressed_count++;
        LISA_LOGI(TAG, "duplicate %s within %ums, ignored", service_v2_gait_action_name(action),
                  (unsigned)VOICE_ACTION_DEBOUNCE_MS);
        return;
    }

    const service_voice_action_queue_item_t item = {
        .action = action,
        .cycles = cycles,
        .speed_percent = speed_percent,
    };
    lisa_mutex_lock(s_action_lock, LISA_WAIT_FOREVER);
    bool queued = service_voice_action_queue_push(&s_action_queue, &item);
    if (queued) {
        voice_chat_action_hold_acquire();
    }
    uint32_t pending = service_voice_action_queue_count(&s_action_queue);
    lisa_mutex_unlock(s_action_lock);
    if (!queued) {
        s_queue_dropped_count++;
        LISA_LOGW(TAG, "cloud action FIFO full, dropping %s",
                  service_v2_gait_action_name(action));
        return;
    }

    s_matched_count++;
    s_last_match = action;
    LISA_LOGI(TAG, "%s queued -> %s (cycles=%u speed=%u pending=%u)", source,
              service_v2_gait_action_name(action), (unsigned)cycles,
              (unsigned)speed_percent, (unsigned)pending);
    voice_action_try_start_next();
}

static void voice_action_on_gait_started(void *unused, uint32_t msg_id, void *data,
                                         uint32_t len, void *user_data)
{
    (void)unused;
    (void)msg_id;
    (void)user_data;

    if (data == NULL || len < sizeof(service_v2_gait_action_t) ||
        *(const service_v2_gait_action_t *)data != SERVICE_V2_GAIT_ACTION_DANCE) {
        return;
    }

    /* Start the local soundtrack only after pose, battery and HOME checks
     * have succeeded.  A merely queued command is not proof of motion. */
    lingxin_server_suppress_current_tts("dog_dance_local_music");
    const char *dance_url = app_tone_get_url(TONE_ID_110);
    if (!voice_player_try_play_prompt_tone_now_url(dance_url)) {
        LISA_LOGI(TAG, "robot dance music skipped because tone player is busy");
    } else {
        LISA_LOGI(TAG, "robot dance music started after gait motion start");
    }
}

static void voice_action_on_wakeup(void *unused, uint32_t msg_id, void *data,
                                   uint32_t len, void *user_data)
{
    if (msg_id == VOICE_MSG_WAKEUP_BUTTON_START &&
        !service_button_wakeup_event_current(data, len)) return;
    (void)unused;
    (void)msg_id;
    (void)data;
    (void)len;
    (void)user_data;

    voice_player_beat_music_cancel();
    __atomic_add_fetch(&s_query_generation, 1U, __ATOMIC_ACQ_REL);
    if (s_query_queue != NULL) {
        xQueueReset(s_query_queue);
    }
    voice_action_cancel_all("wakeup");
}

void service_voice_action_cancel_pending(void)
{
    if (s_action_lock != NULL) voice_action_on_wakeup(NULL, 0, NULL, 0, NULL);
}

static void voice_action_on_gait_finished(void *unused, uint32_t msg_id, void *data,
                                          uint32_t len, void *user_data)
{
    (void)unused;
    (void)msg_id;
    (void)user_data;

    if (data == NULL || len < sizeof(service_v2_gait_action_finished_t)) {
        return;
    }

    const service_v2_gait_action_finished_t *finished = data;
    if (finished->background) {
        /* A touch/idle result cannot complete a same-named cloud command. */
        voice_action_try_start_next();
        return;
    }
    lisa_mutex_lock(s_action_lock, LISA_WAIT_FOREVER);
    bool completed = service_voice_action_queue_complete(&s_action_queue, finished->action);
    uint32_t pending = service_voice_action_queue_count(&s_action_queue);
    lisa_mutex_unlock(s_action_lock);

    if (completed) {
        LISA_LOGI(TAG, "cloud action complete %s result=%d pending=%u",
                  service_v2_gait_action_name(finished->action), finished->result,
                  (unsigned)pending);
        voice_chat_action_hold_release();
    }
    /* Also retry after unrelated touch/IMU/startup motions release gait. */
    voice_action_try_start_next();
}

/** 前后走、前后跑、左右转各用实机定好的默认周期数。 */
static uint32_t voice_action_default_cycles(service_v2_gait_action_t action)
{
    return service_v2_gait_default_cycles(action);
}

/*
 * 云端原生动作通路。
 *
 * payload 是 lingxin_server 发过来的一整条 action_list 元素：
 *   {"action":"FORWARD","parameters":{"SPEED":50,"TIME":5}}
 * parameters 可以没有。控制台系统预置"移动控制"给 FORWARD/BACKWARD 配了
 * SPEED / DISTANCE / TIME 三个参数，参数名大小写由模型填，所以下面统一用
 * cJSON_GetObjectItem（大小写不敏感）取。
 *
 * TIME 按动作自己的周期时长换算：慢走 640 ms、快跑 320 ms、转向 600 ms。
 * 转向 ANGLE 优先，COUNT/cycles/TIME 控制普通走行周期。SPEED 仅调走行节拍；
 * DISTANCE 兼容解释为周期数，不是厘米。表演动作保持实机标定时间轴。
 */
static void voice_action_on_cloud(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    if (!service_offline_action_online_current(service_offline_action_generation())) return;
    (void)unused;
    (void)msg_id;
    (void)user_data;

    if (data == NULL || len == 0U) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)data, len);

    if (root == NULL) {
        LISA_LOGW(TAG, "cloud action parse failed, len=%u", (unsigned)len);
        return;
    }

    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "action"));
    service_v2_gait_action_t action;
    service_voice_query_t query;

    if (service_voice_query_from_cloud_code(code, &query)) {
        s_cloud_action_count++;
        (void)voice_query_submit(query);
        cJSON_Delete(root);
        return;
    }

    /* Volume has a separate consumer; it is not an unknown motion command. */
    if (voice_lingxin_action_is_supported(code)) {
        cJSON_Delete(root);
        return;
    }

    if (code == NULL || !service_voice_action_from_cloud_code(code, &action)) {
        s_cloud_unknown_count++;
        LISA_LOGW(TAG, "unmapped cloud action code: %s", code != NULL ? code : "(none)");
        cJSON_Delete(root);
        return;
    }

    bool fixed_roll_cycles = action == SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP ||
                             action == SERVICE_V2_GAIT_ACTION_BACKFLIP;
    bool fixed_product_timing = service_voice_action_has_fixed_timing(action);
    uint32_t cycles = fixed_roll_cycles ? (cloud_code_is_double_roll(code) ? 2U : 1U)
                                        : voice_action_default_cycles(action);
    cJSON *params = cJSON_GetObjectItem(root, "parameters");
    if (params == NULL) params = cJSON_GetObjectItem(root, "params");
    uint8_t speed_percent = 50U;
    cJSON *speed_item = params != NULL ? cJSON_GetObjectItem(params, "SPEED") : NULL;
    if (!fixed_product_timing && cJSON_IsNumber(speed_item)) {
        double speed = speed_item->valuedouble;
        if (speed < 20.0) speed = 20.0;
        if (speed > 100.0) speed = 100.0;
        speed_percent = (uint8_t)speed;
    }

    if (action == SERVICE_V2_GAIT_ACTION_RIGHT_FRONT_HANDSHAKE && params != NULL) {
        const char *hand = cJSON_GetStringValue(cJSON_GetObjectItem(params, "HAND"));
        if (hand != NULL && (code_equal(hand, "LEFT") || strcmp(hand, "左") == 0)) {
            action = SERVICE_V2_GAIT_ACTION_LEFT_FRONT_HANDSHAKE;
        }
    }
    if (action == SERVICE_V2_GAIT_ACTION_SPIN && params != NULL) {
        const char *direction = cJSON_GetStringValue(cJSON_GetObjectItem(params, "DIRECTION"));
        if (direction != NULL && code_equal(direction, "LEFT")) action = SERVICE_V2_GAIT_ACTION_TURN_LEFT;
        else if (direction != NULL && code_equal(direction, "RIGHT")) action = SERVICE_V2_GAIT_ACTION_TURN_RIGHT;
    }

    bool explicit_turn_angle = false;
    if (action == SERVICE_V2_GAIT_ACTION_PUSHUP && params != NULL) {
        cJSON *count = cJSON_GetObjectItem(params, "cycles");
        if (!cJSON_IsNumber(count)) count = cJSON_GetObjectItem(params, "COUNT");
        if (cJSON_IsNumber(count)) cycles = service_voice_action_resolve_pushup_cycles(count->valuedouble);
    }
    /* 转向优先采用明确 ANGLE；其它动作兼容 cycles、COUNT、TIME。 */
    if (cycles != 0U && !fixed_roll_cycles && !fixed_product_timing) {
        cJSON *cycles_item = params != NULL ? cJSON_GetObjectItem(params, "cycles") : NULL;
        cJSON *count_item = params != NULL ? cJSON_GetObjectItem(params, "COUNT") : NULL;
        cJSON *time_item = params != NULL ? cJSON_GetObjectItem(params, "TIME") : NULL;
        cJSON *distance_item = params != NULL ? cJSON_GetObjectItem(params, "DISTANCE") : NULL;
        cJSON *angle_item = params != NULL ? cJSON_GetObjectItem(params, "ANGLE") : NULL;
        uint32_t cycle_ms = service_v2_gait_cycle_ms(action);
        double want = -1.0;

        bool is_turn = action == SERVICE_V2_GAIT_ACTION_TURN_LEFT ||
                       action == SERVICE_V2_GAIT_ACTION_TURN_RIGHT;
        if (is_turn && cJSON_IsNumber(angle_item) && angle_item->valuedouble > 0.0) {
            /* 当前实机默认 8 周期约对应 90 度；仍是开环近似值。 */
            want = (angle_item->valuedouble * (double)SERVICE_V2_GAIT_DEFAULT_TURN_CYCLES + 89.0) / 90.0;
            explicit_turn_angle = true;
        } else if (cJSON_IsNumber(cycles_item)) {
            want = cycles_item->valuedouble;
        } else if (cJSON_IsNumber(count_item)) {
            want = count_item->valuedouble;
        } else if (cycle_ms != 0U && cJSON_IsNumber(time_item) && time_item->valuedouble > 0.0) {
            /* 向上取整，"往前走 1 秒"至少也要走完一个完整周期，半个周期会劈叉。 */
            want = (time_item->valuedouble * 1000.0 + (double)(cycle_ms - 1U)) / (double)cycle_ms;
        } else if ((action == SERVICE_V2_GAIT_ACTION_FORWARD ||
                    action == SERVICE_V2_GAIT_ACTION_BACKWARD ||
                    action == SERVICE_V2_GAIT_ACTION_RUN_FORWARD ||
                    action == SERVICE_V2_GAIT_ACTION_RUN_BACKWARD) &&
                   cJSON_IsNumber(distance_item) && distance_item->valuedouble > 0.0) {
            /* 无里程计：DISTANCE 暂按完整步态周期数解释并在日志中明确打印。 */
            want = distance_item->valuedouble;
        }

        if (want >= 0.0) {
            /* 先在 double 域内截断再转整数。这同时安全处理 +Inf，
             * 并避免云端极大数转 uint32_t 的未定义行为。NaN 不满足
             * want >= 0.0，会保留动作默认周期。 */
            uint32_t n = want > (double)VOICE_ACTION_CLOUD_MAX_CYCLES
                             ? VOICE_ACTION_CLOUD_MAX_CYCLES
                             : (uint32_t)want;

            /* 云端的 0 不能透传成“无限执行”，语音动作必须有限并自动回 Home。 */
            if (n == 0U) {
                n = 1U;
            }
            cycles = n;
        }
    }

    if (action == SERVICE_V2_GAIT_ACTION_TURN_LEFT ||
        action == SERVICE_V2_GAIT_ACTION_TURN_RIGHT) {
        cycles = service_voice_action_resolve_turn_cycles(cycles, explicit_turn_angle);
    }

    s_cloud_action_count++;
    voice_action_dispatch(action, cycles, speed_percent, "cloud");
    cJSON_Delete(root);
}

int service_voice_action_init(void)
{
    if (s_ready) {
        return 0;
    }

    if (s_action_lock == NULL) {
        s_action_lock = lisa_mutex_create();
        if (s_action_lock == NULL) {
            LISA_LOGE(TAG, "create action FIFO lock failed");
            return -ENOMEM;
        }
    }
    service_voice_action_queue_init(&s_action_queue);

    if (s_query_queue == NULL) {
        s_query_queue = xQueueCreate(VOICE_QUERY_QUEUE_DEPTH, sizeof(voice_query_request_t));
        if (s_query_queue == NULL) {
            LISA_LOGE(TAG, "create device-query queue failed");
            return -ENOMEM;
        }
    }
    if (s_query_thread == NULL) {
        lisa_thread_attr_t query_attr = {
            .name = (uint8_t *)"v2_voice_query",
            .stack_size = VOICE_QUERY_TASK_STACK,
            .priority = LISA_OS_PRIORITY_LOW,
        };
        s_query_thread = lisa_thread_create(&query_attr, voice_query_task, NULL);
        if (s_query_thread == NULL) {
            LISA_LOGE(TAG, "create device-query worker failed");
            return -ENOMEM;
        }
    }

    /*
     * 真正语音控制只走 eAgent 的 action_list：ASR -> LLM -> 结构化指令 -> 动作。
     * 不再订阅 IAT_UPDATE 做本地关键词触发，避免绕过平台意图判断。
     * service_voice_action_match() 仅保留给 v2hw gait say 做不动腿的词表自检。
     */
    if (voice_msg_sub(VOICE_MSG_CLOUD_ACTION_LIST, voice_action_on_cloud, NULL) != 0) {
        LISA_LOGE(TAG, "subscribe cloud action_list failed");
        return -EIO;
    }
    if (voice_msg_sub(VOICE_MSG_V2_GAIT_ACTION_STARTED, voice_action_on_gait_started, NULL) != 0 ||
        voice_msg_sub(VOICE_MSG_V2_GAIT_ACTION_FINISHED, voice_action_on_gait_finished, NULL) != 0 ||
        voice_msg_sub(VOICE_MSG_WAKEUP_KEYWORD, voice_action_on_wakeup, NULL) != 0 ||
        voice_msg_sub(VOICE_MSG_WAKEUP_BUTTON_START, voice_action_on_wakeup, NULL) != 0) {
        LISA_LOGE(TAG, "subscribe gait/wakeup lifecycle failed");
        return -EIO;
    }
    s_ready = true;
    LISA_LOGI(TAG, "voice action bridge ready");
    return 0;
}

int service_voice_action_get_status(service_voice_action_status_t *status)
{
    if (status == NULL) {
        return -EINVAL;
    }

    status->ready = s_ready;
    status->matched_count = s_matched_count;
    status->suppressed_count = s_suppressed_count;
    status->cloud_action_count = s_cloud_action_count;
    status->cloud_unknown_count = s_cloud_unknown_count;
    if (s_action_lock != NULL) {
        lisa_mutex_lock(s_action_lock, LISA_WAIT_FOREVER);
        status->queued_count = service_voice_action_queue_count(&s_action_queue);
        status->action_active = service_voice_action_queue_active(&s_action_queue);
        lisa_mutex_unlock(s_action_lock);
    } else {
        status->queued_count = 0U;
        status->action_active = false;
    }
    status->queue_dropped_count = s_queue_dropped_count;
    status->last_match = s_last_match;
    return s_ready ? 0 : -ENODEV;
}
