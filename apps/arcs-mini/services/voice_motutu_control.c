#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "voice-dog"
#include "lisa_log.h"

#include "app_netcfg_portal.h"
#include "voice_msg.h"

#include "motutu_motion.h"
#include "service_motutu_web.h"
#include "voice_motutu_control.h"

#define VOICE_MOTUTU_TEXT_BUF_SIZE 128

static char s_voice_motutu_iat_text[VOICE_MOTUTU_TEXT_BUF_SIZE];
static bool s_suppress_next_cloud_tts = false;

static bool text_has(const char *text, const char *kw)
{
    return strstr(text, kw) != NULL;
}

static bool text_has_any(const char *text, const char *const *keywords, size_t count)
{
    size_t i = 0;

    for (i = 0; i < count; ++i) {
        if (text_has(text, keywords[i])) {
            return true;
        }
    }

    return false;
}

static void voice_motutu_cache_text(const char *text)
{
    if (text == NULL) {
        s_voice_motutu_iat_text[0] = '\0';
        return;
    }

    strncpy(s_voice_motutu_iat_text, text, sizeof(s_voice_motutu_iat_text) - 1);
    s_voice_motutu_iat_text[sizeof(s_voice_motutu_iat_text) - 1] = '\0';
}

static bool voice_motutu_match_token(const char *text, size_t len, const char *token, size_t *advance)
{
    size_t token_len = strlen(token);

    if (len < token_len || strncmp(text, token, token_len) != 0) {
        return false;
    }

    if (advance != NULL) {
        *advance = token_len;
    }
    return true;
}

static bool voice_motutu_parse_ascii_number_exact(const char *text, size_t len, double *out_value)
{
    char buf[32];
    char *end = NULL;
    double value = 0.0;
    size_t copy_len = len;

    while (copy_len > 0 && isspace((unsigned char)text[copy_len - 1])) {
        --copy_len;
    }
    while (copy_len > 0 && isspace((unsigned char)*text)) {
        ++text;
        --copy_len;
    }
    if (copy_len == 0 || copy_len >= sizeof(buf)) {
        return false;
    }

    memcpy(buf, text, copy_len);
    buf[copy_len] = '\0';
    value = strtod(buf, &end);
    if (end == buf) {
        return false;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }

    *out_value = value;
    return true;
}

static bool voice_motutu_parse_chinese_digit(const char *text, size_t len, int *digit, size_t *advance)
{
    static const struct {
        const char *token;
        int value;
    } tokens[] = {
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
    };
    size_t i = 0;

    for (i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
        size_t adv = 0;

        if (voice_motutu_match_token(text, len, tokens[i].token, &adv)) {
            *digit = tokens[i].value;
            if (advance != NULL) {
                *advance = adv;
            }
            return true;
        }
    }

    return false;
}

static bool voice_motutu_parse_chinese_number_exact(const char *text, size_t len, double *out_value)
{
    int total = 0;
    int current = 0;
    bool has_any = false;
    const char *p = text;
    size_t remain = len;

    while (remain > 0 && isspace((unsigned char)*p)) {
        ++p;
        --remain;
    }
    while (remain > 0 && isspace((unsigned char)p[remain - 1])) {
        --remain;
    }
    if (remain == 0) {
        return false;
    }

    while (remain > 0) {
        int digit = 0;
        size_t adv = 0;

        if (voice_motutu_parse_chinese_digit(p, remain, &digit, &adv)) {
            current = digit;
            has_any = true;
            p += adv;
            remain -= adv;
            continue;
        }
        if (voice_motutu_match_token(p, remain, "十", &adv)) {
            total += (current == 0 ? 1 : current) * 10;
            current = 0;
            has_any = true;
            p += adv;
            remain -= adv;
            continue;
        }
        if (voice_motutu_match_token(p, remain, "百", &adv)) {
            total += (current == 0 ? 1 : current) * 100;
            current = 0;
            has_any = true;
            p += adv;
            remain -= adv;
            continue;
        }
        if (voice_motutu_match_token(p, remain, "零", &adv)) {
            has_any = true;
            current = 0;
            p += adv;
            remain -= adv;
            continue;
        }
        return false;
    }

    *out_value = (double)(total + current);
    return has_any;
}

static bool voice_motutu_parse_number_exact(const char *text, size_t len, double *out_value)
{
    return voice_motutu_parse_ascii_number_exact(text, len, out_value)
        || voice_motutu_parse_chinese_number_exact(text, len, out_value);
}

static bool voice_motutu_extract_value_before_unit(const char *text, const char *unit, uint8_t *out_value)
{
    const char *unit_pos = strstr(text, unit);
    size_t unit_len = strlen(unit);

    while (unit_pos != NULL) {
        const char *scan = text;

        while (scan < unit_pos) {
            double value = 0.0;
            if (voice_motutu_parse_number_exact(scan, (size_t)(unit_pos - scan), &value)) {
                if (value < 1.0) {
                    value = 1.0;
                }
                if (value > 20.0) {
                    value = 20.0;
                }
                *out_value = (uint8_t)(value + 0.5);
                return true;
            }
            ++scan;
        }

        unit_pos = strstr(unit_pos + unit_len, unit);
    }

    return false;
}

static uint8_t voice_motutu_extract_steps(const char *text)
{
    uint8_t steps = 1U;

    if (text == NULL || text[0] == '\0') {
        return steps;
    }

    if (voice_motutu_extract_value_before_unit(text, "步", &steps)
        || voice_motutu_extract_value_before_unit(text, "次", &steps)
        || voice_motutu_extract_value_before_unit(text, "下", &steps)) {
        return steps;
    }

    return 1U;
}

static motutu_speed_t voice_motutu_parse_speed(const char *text)
{
    static const char *const fast_keywords[] = {
        "快", "快速", "快点", "高频", "高速",
    };
    static const char *const slow_keywords[] = {
        "慢", "慢点", "慢速", "低速", "轻轻",
    };

    if (text_has_any(text, fast_keywords, sizeof(fast_keywords) / sizeof(fast_keywords[0]))) {
        return MOTUTU_SPEED_FAST;
    }
    if (text_has_any(text, slow_keywords, sizeof(slow_keywords) / sizeof(slow_keywords[0]))) {
        return MOTUTU_SPEED_SLOW;
    }

    return MOTUTU_SPEED_NORMAL;
}

static bool voice_motutu_is_dog_command(const char *text)
{
    static const char *const dog_keywords[] = {
        "小狗", "机器狗", "狗狗", "motutu", "摩途兔", "木图图", "木兔兔",
    };
    static const char *const dog_action_keywords[] = {
        "站立", "站好", "起立", "趴下", "卧倒", "前进", "后退", "左转", "右转", "翻跟头", "翻滚",
        "招手", "挥手", "原地旋转", "发球", "网球发球", "校正", "初始化", "复位",
        "配网", "联网", "网络配置",
    };

    return text_has_any(text, dog_keywords, sizeof(dog_keywords) / sizeof(dog_keywords[0]))
        || text_has_any(text, dog_action_keywords, sizeof(dog_action_keywords) / sizeof(dog_action_keywords[0]));
}

static bool voice_motutu_is_web_command(const char *text)
{
    static const char *const web_keywords[] = {
        "显示ip", "显示IP", "ip地址", "IP地址", "网络地址", "控制页",
        "控制页面", "控制地址", "网页控制", "打开页面", "打开控制页",
    };

    return text_has_any(text, web_keywords, sizeof(web_keywords) / sizeof(web_keywords[0]));
}

static bool voice_motutu_is_netcfg_command(const char *text)
{
    static const char *const netcfg_keywords[] = {
        "进入配网模式", "开始配网", "打开配网", "进入联网模式", "开始联网", "网络配置",
        "配置网络", "连接网络", "重新配网", "配网模式",
    };

    return text_has_any(text, netcfg_keywords, sizeof(netcfg_keywords) / sizeof(netcfg_keywords[0]));
}

static void handle_command_text(const char *text)
{
    static const char *const stand_keywords[] = {"站立", "站好", "起立", "立正", "站起来", "复位", "复位姿态"};
    static const char *const calibrate_keywords[] = {"校正", "初始化", "初始化舵机", "校正舵机", "舵机校正"};
    static const char *const down_keywords[] = {"趴下", "卧倒", "趴着", "躺下", "伏下"};
    static const char *const forward_keywords[] = {"前进", "往前", "向前", "走", "走路"};
    static const char *const backward_keywords[] = {"后退", "倒退", "往后", "向后"};
    static const char *const left_keywords[] = {"左转", "向左转", "往左转", "朝左转"};
    static const char *const right_keywords[] = {"右转", "向右转", "往右转", "朝右转"};
    static const char *const flip_keywords[] = {"翻跟头", "翻滚", "翻一个", "打滚"};
    static const char *const wave_keywords[] = {"招手", "挥手", "挥一挥"};
    static const char *const spin_keywords[] = {"原地旋转", "原地转圈", "自转", "旋转"};
    static const char *const serve_keywords[] = {"发球", "网球发球", "挥拍"};
    static const char *const netcfg_keywords[] = {
        "进入配网模式", "开始配网", "打开配网", "进入联网模式", "开始联网", "网络配置",
        "配置网络", "连接网络", "重新配网", "配网模式",
    };
    static const char *const show_ip_keywords[] = {
        "显示ip", "显示IP", "显示IP地址", "显示ip地址", "查看ip地址", "查看IP地址",
        "查看网络地址", "显示网络地址", "打开控制页面", "打开控制页", "显示控制页面",
        "显示控制页", "查看控制页", "查看控制地址", "网页控制",
    };
    uint8_t steps = 1U;
    motutu_speed_t speed = MOTUTU_SPEED_NORMAL;
    int ret = 0;
    bool handled = false;

    if (text == NULL || text[0] == '\0') {
        return;
    }
    if (!voice_motutu_is_dog_command(text) && !voice_motutu_is_web_command(text) && !voice_motutu_is_netcfg_command(text)) {
        LOGI("voice-dog: ignore non-dog text=\"%s\"", text);
        return;
    }

    steps = voice_motutu_extract_steps(text);
    speed = voice_motutu_parse_speed(text);

    if (text_has_any(text, show_ip_keywords, sizeof(show_ip_keywords) / sizeof(show_ip_keywords[0]))) {
        ret = service_motutu_web_show_control_page();
        handled = true;
        LOGW("voice-dog: show control page");
    } else if (text_has_any(text, netcfg_keywords, sizeof(netcfg_keywords) / sizeof(netcfg_keywords[0]))) {
        if (app_netcfg_portal_is_active()) {
            ret = 0;
        } else {
            ret = app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST);
        }
        handled = true;
        LOGW("voice-dog: start netcfg portal");
    } else if (!motutu_motion_is_ready()) {
        LOGE("voice-dog: motutu motion backend not ready");
        return;
    } else if (text_has_any(text, calibrate_keywords, sizeof(calibrate_keywords) / sizeof(calibrate_keywords[0]))) {
        ret = motutu_calibrate_pose();
        handled = true;
        LOGW("voice-dog: calibrate pose");
    } else if (text_has_any(text, stand_keywords, sizeof(stand_keywords) / sizeof(stand_keywords[0]))) {
        ret = motutu_stand();
        handled = true;
        LOGW("voice-dog: stand");
    } else if (text_has_any(text, down_keywords, sizeof(down_keywords) / sizeof(down_keywords[0]))) {
        ret = motutu_down();
        handled = true;
        LOGW("voice-dog: down");
    } else if (text_has_any(text, flip_keywords, sizeof(flip_keywords) / sizeof(flip_keywords[0]))) {
        ret = motutu_flip();
        handled = true;
        LOGW("voice-dog: flip");
    } else if (text_has_any(text, wave_keywords, sizeof(wave_keywords) / sizeof(wave_keywords[0]))) {
        if (strstr(text, "左") != NULL) {
            ret = motutu_wave_left();
            LOGW("voice-dog: wave left");
        } else {
            ret = motutu_wave_right();
            LOGW("voice-dog: wave right");
        }
        handled = true;
    } else if (text_has_any(text, serve_keywords, sizeof(serve_keywords) / sizeof(serve_keywords[0]))) {
        if (strstr(text, "左") != NULL) {
            ret = motutu_serve_left();
            LOGW("voice-dog: serve left");
        } else {
            ret = motutu_serve_right();
            LOGW("voice-dog: serve right");
        }
        handled = true;
    } else if (text_has_any(text, spin_keywords, sizeof(spin_keywords) / sizeof(spin_keywords[0]))) {
        if (strstr(text, "左") != NULL) {
            ret = motutu_spin_left(steps, speed);
            LOGW("voice-dog: spin left steps=%u speed=%d", (unsigned int)steps, (int)speed);
        } else {
            ret = motutu_spin_right(steps, speed);
            LOGW("voice-dog: spin right steps=%u speed=%d", (unsigned int)steps, (int)speed);
        }
        handled = true;
    } else if (text_has_any(text, left_keywords, sizeof(left_keywords) / sizeof(left_keywords[0]))) {
        ret = motutu_turn_left(steps, speed);
        handled = true;
        LOGW("voice-dog: turn left steps=%u speed=%d", (unsigned int)steps, (int)speed);
    } else if (text_has_any(text, right_keywords, sizeof(right_keywords) / sizeof(right_keywords[0]))) {
        ret = motutu_turn_right(steps, speed);
        handled = true;
        LOGW("voice-dog: turn right steps=%u speed=%d", (unsigned int)steps, (int)speed);
    } else if (text_has_any(text, backward_keywords, sizeof(backward_keywords) / sizeof(backward_keywords[0]))) {
        ret = motutu_backward(steps, speed);
        handled = true;
        LOGW("voice-dog: backward steps=%u speed=%d", (unsigned int)steps, (int)speed);
    } else if (text_has_any(text, forward_keywords, sizeof(forward_keywords) / sizeof(forward_keywords[0]))) {
        ret = motutu_forward(steps, speed);
        handled = true;
        LOGW("voice-dog: forward steps=%u speed=%d", (unsigned int)steps, (int)speed);
    } else {
        LOGI("voice-dog: ignore text=\"%s\"", text);
        return;
    }

    if (!handled || ret != 0) {
        LOGE("voice-dog: execute failed ret=%d text=\"%s\"", ret, text);
        return;
    }

    s_suppress_next_cloud_tts = true;
}

static const char *voice_motutu_msg_name(uint32_t msg_id)
{
    switch (msg_id) {
    case VOICE_MSG_CLOUD_RECOGNIZED_COMMAND:
        return "cloud-command";
    case VOICE_MSG_RECOGNIZED_ONLINE_COMMAND:
        return "online-command";
    case VOICE_MSG_RECOGNIZED_OFFLINE_COMMAND:
        return "offline-command";
    default:
        return "unknown-command";
    }
}

static void voice_motutu_iat_cb(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    (void)unused;
    (void)len;
    (void)user_data;

    if (msg_id == VOICE_MSG_CLOUD_IAT_START) {
        s_voice_motutu_iat_text[0] = '\0';
        s_suppress_next_cloud_tts = false;
        return;
    }

    if (msg_id == VOICE_MSG_CLOUD_IAT_UPDATE) {
        if (data != NULL) {
            voice_motutu_cache_text((const char *)data);
            LOGW("voice-dog: ASR update=\"%s\"", s_voice_motutu_iat_text);
        }
        return;
    }

    if (msg_id == VOICE_MSG_CLOUD_IAT_END) {
        LOGW("voice-dog: ASR final=\"%s\"", s_voice_motutu_iat_text);
        handle_command_text(s_voice_motutu_iat_text);
        return;
    }

    if (msg_id == VOICE_MSG_CLOUD_RECOGNIZED_COMMAND || msg_id == VOICE_MSG_RECOGNIZED_ONLINE_COMMAND ||
        msg_id == VOICE_MSG_RECOGNIZED_OFFLINE_COMMAND) {
        const voice_msg_cloud_recognized_command_t *cmd = (const voice_msg_cloud_recognized_command_t *)data;

        if (cmd == NULL || cmd->command[0] == '\0') {
            return;
        }

        LOGW("voice-dog: %s=\"%s\"", voice_motutu_msg_name(msg_id), cmd->command);
        handle_command_text(cmd->command);
    }
}

void voice_motutu_control_init(void)
{
    s_voice_motutu_iat_text[0] = '\0';
    s_suppress_next_cloud_tts = false;
    voice_msg_sub(VOICE_MSG_CLOUD_IAT_START, voice_motutu_iat_cb, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_IAT_UPDATE, voice_motutu_iat_cb, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_IAT_END, voice_motutu_iat_cb, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_RECOGNIZED_COMMAND, voice_motutu_iat_cb, NULL);
    voice_msg_sub(VOICE_MSG_RECOGNIZED_ONLINE_COMMAND, voice_motutu_iat_cb, NULL);
    voice_msg_sub(VOICE_MSG_RECOGNIZED_OFFLINE_COMMAND, voice_motutu_iat_cb, NULL);
    LOGW("voice-dog: ready (subscribe IAT + recognized command messages)");
}

bool voice_motutu_control_consume_cloud_tts_suppress(void)
{
    bool suppress = s_suppress_next_cloud_tts;

    s_suppress_next_cloud_tts = false;
    return suppress;
}
