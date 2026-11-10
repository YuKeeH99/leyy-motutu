#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "voice-car"
#include "lisa_log.h"

#include "voice_msg.h"

#include "car_motion.h"
#include "voice_car_control.h"

/*
 * 简单关键字匹配控制小车。
 * 识别文本来自云端 ASR，缓存整句并在 VOICE_MSG_CLOUD_IAT_END 时统一匹配，
 * 避免在增量识别阶段被"前/后/左/右"等短词提前误触发。
 */

#define VOICE_CAR_TEXT_BUF_SIZE 128

static char s_voice_car_iat_text[VOICE_CAR_TEXT_BUF_SIZE];
static bool s_suppress_next_cloud_tts = false;
/* 上一条即时运动(方向)函数指针: 单说"加速/减速"时用新速度档重放它。定距/定角命中也记录其即时版。 */
static int (*s_car_last_motion)(car_speed_t) = NULL;

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

static car_speed_t voice_car_parse_speed(const char *text)
{
    /* 先判高速再判低速。"加速"/"减速"均含"速"但互不误匹配(整词包含)。 */
    static const char *const high_keywords[] = {
        "快", "高速", "快速", "快点", "快一点", "加速", "提速", "再快", "更快",
        "飞快", "全速", "最快", "冲",
    };
    static const char *const low_keywords[] = {
        "慢", "低速", "慢速", "慢点", "慢一点", "减速", "降速", "再慢", "更慢",
        "慢慢", "龟速", "最慢",
    };

    if (text_has_any(text, high_keywords, sizeof(high_keywords) / sizeof(high_keywords[0]))) {
        return CAR_SPEED_HIGH;
    }
    if (text_has_any(text, low_keywords, sizeof(low_keywords) / sizeof(low_keywords[0]))) {
        return CAR_SPEED_LOW;
    }

    return CAR_SPEED_HIGH;  /* 默认高速档(路况干扰小, 运动更稳定)。明确说"慢/减速"才切低速 */
}

/* 是否出现了明确的速度词(加速/减速/快/慢…)。用于区分"单说变速"与默认低速, 触发重放上一条运动。 */
static bool voice_car_has_speed_word(const char *text)
{
    static const char *const speed_words[] = {
        "快", "高速", "快速", "快点", "快一点", "加速", "提速", "再快", "更快", "飞快", "全速", "最快", "冲",
        "慢", "低速", "慢速", "慢点", "慢一点", "减速", "降速", "再慢", "更慢", "慢慢", "龟速", "最慢",
    };
    return text_has_any(text, speed_words, sizeof(speed_words) / sizeof(speed_words[0]));
}

static void voice_car_cache_text(const char *text)
{
    if (text == NULL) {
        s_voice_car_iat_text[0] = '\0';
        return;
    }

    strncpy(s_voice_car_iat_text, text, sizeof(s_voice_car_iat_text) - 1);
    s_voice_car_iat_text[sizeof(s_voice_car_iat_text) - 1] = '\0';
}

static bool voice_car_match_token(const char *text, size_t len, const char *token, size_t *advance)
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

static bool voice_car_parse_ascii_number_exact(const char *text, size_t len, double *out_value)
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

static bool voice_car_parse_chinese_digit(const char *text, size_t len, int *digit, size_t *advance)
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

        if (voice_car_match_token(text, len, tokens[i].token, &adv)) {
            *digit = tokens[i].value;
            if (advance != NULL) {
                *advance = adv;
            }
            return true;
        }
    }

    return false;
}

static bool voice_car_parse_chinese_number_exact(const char *text, size_t len, double *out_value)
{
    int total = 0;
    int current = 0;
    bool has_any = false;
    double fraction = 0.0;
    double place = 0.1;
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

    if (voice_car_match_token(p, remain, "半", NULL) && strlen("半") == remain) {
        *out_value = 0.5;
        return true;
    }

    while (remain > 0) {
        int digit = 0;
        size_t adv = 0;

        if (voice_car_parse_chinese_digit(p, remain, &digit, &adv)) {
            current = digit;
            has_any = true;
            p += adv;
            remain -= adv;
            continue;
        }
        if (voice_car_match_token(p, remain, "十", &adv)) {
            total += (current == 0 ? 1 : current) * 10;
            current = 0;
            has_any = true;
            p += adv;
            remain -= adv;
            continue;
        }
        if (voice_car_match_token(p, remain, "百", &adv)) {
            total += (current == 0 ? 1 : current) * 100;
            current = 0;
            has_any = true;
            p += adv;
            remain -= adv;
            continue;
        }
        if (voice_car_match_token(p, remain, "零", &adv)) {
            has_any = true;
            current = 0;
            p += adv;
            remain -= adv;
            continue;
        }
        if (voice_car_match_token(p, remain, "点", &adv)) {
            total += current;
            current = 0;
            has_any = true;
            p += adv;
            remain -= adv;
            while (remain > 0) {
                if (!voice_car_parse_chinese_digit(p, remain, &digit, &adv)) {
                    return false;
                }
                fraction += (double)digit * place;
                place /= 10.0;
                p += adv;
                remain -= adv;
            }
            *out_value = (double)total + fraction;
            return true;
        }

        return false;
    }

    *out_value = (double)(total + current);
    return has_any;
}

static bool voice_car_parse_number_exact(const char *text, size_t len, double *out_value)
{
    return voice_car_parse_ascii_number_exact(text, len, out_value)
        || voice_car_parse_chinese_number_exact(text, len, out_value);
}

static bool voice_car_extract_value_before_unit(const char *text, const char *unit, double *out_value)
{
    const char *unit_pos = strstr(text, unit);
    size_t unit_len = strlen(unit);

    while (unit_pos != NULL) {
        const char *scan = text;

        while (scan < unit_pos) {
            if (voice_car_parse_number_exact(scan, (size_t)(unit_pos - scan), out_value)) {
                return true;
            }
            ++scan;
        }

        unit_pos = strstr(unit_pos + unit_len, unit);
    }

    return false;
}

static bool voice_car_extract_distance_mm(const char *text, uint32_t *distance_mm)
{
    double value = 0.0;

    if (voice_car_extract_value_before_unit(text, "厘米", &value)
        || voice_car_extract_value_before_unit(text, "公分", &value)) {
        *distance_mm = (uint32_t)(value * 10.0 + 0.5);
        return true;
    }
    if (voice_car_extract_value_before_unit(text, "米", &value)) {
        *distance_mm = (uint32_t)(value * 1000.0 + 0.5);
        return true;
    }

    return false;
}

static bool voice_car_extract_angle_deg(const char *text, uint16_t *angle_deg)
{
    double value = 0.0;

    if (!voice_car_extract_value_before_unit(text, "度", &value)) {
        return false;
    }

    *angle_deg = (uint16_t)(value + 0.5);
    return true;
}

static void handle_command_text(const char *text)
{
    static const char *const stop_keywords[] = {
        "停车", "停下", "停止", "刹车", "别动", "不要动", "停住", "停",
    };
    static const char *const spin_keywords[] = {
        "原地", "自转", "旋转", "转圈",
    };
    static const char *const strafe_keywords[] = {
        "平移", "横移", "侧移", "平行", "螃蟹",
    };
    static const char *const turn_keywords[] = {
        "转", "拐", "偏",
    };
    static const char *const backward_keywords[] = {
        "后退", "倒退", "往后", "向后", "退后", "倒车", "后撤",
    };
    static const char *const forward_keywords[] = {
        "前进", "往前", "向前", "前行", "直行", "直走", "开动", "出发", "启动",
    };
    int ret = 0;
    bool handled = false;
    uint32_t distance_mm = 0;
    uint16_t angle_deg = 0;
    bool has_distance = false;
    bool has_angle = false;
    int (*inst_fn)(car_speed_t) = NULL;  /* 本条命中的即时运动函数, 供"加速/减速"重放记录 */

    if (text == NULL || text[0] == '\0') {
        return;
    }

    if (!car_motion_is_ready()) {
        LOGE("voice-car: car motion backend not ready");
        return;
    }

    car_speed_t sp = voice_car_parse_speed(text);
    has_distance = voice_car_extract_distance_mm(text, &distance_mm);
    has_angle = voice_car_extract_angle_deg(text, &angle_deg);

    /*
     * 模糊匹配: 先判方向(左/右), 再判动作类别(原地/平移/转弯), 组合出具体动作。
     * 这样 "左转" "左边转" "往左拐" "朝左偏" "向左横一下" 都能命中, 不再依赖整词列表。
     * 匹配顺序: 停止 > 原地(spin) > 平移(strafe) > 转弯(turn) > 后退 > 前进。
     * 方向词 "左/右" 只在需要方向的类别里判断; 前进/后退不看方向。
     */
    bool has_left = text_has(text, "左");
    bool has_right = text_has(text, "右");
    bool want_spin = text_has_any(text, spin_keywords, sizeof(spin_keywords) / sizeof(spin_keywords[0]));
    bool want_strafe = text_has_any(text, strafe_keywords, sizeof(strafe_keywords) / sizeof(strafe_keywords[0]));
    bool want_turn = text_has_any(text, turn_keywords, sizeof(turn_keywords) / sizeof(turn_keywords[0]));

    /* 斜向复合方向词, 最具体, 优先于原地/平移/转弯判断。命中如 "左前移动10cm" "往右后走"。
     * 关键词泛化: 前=上、后=下, 兼容 左上/右上/左下/右下 及 左斜前/左斜后 等口语说法。 */
    bool diag_fl = text_has(text, "左前") || text_has(text, "左上") || text_has(text, "左斜前");
    bool diag_fr = text_has(text, "右前") || text_has(text, "右上") || text_has(text, "右斜前");
    bool diag_bl = text_has(text, "左后") || text_has(text, "左下") || text_has(text, "左斜后");
    bool diag_br = text_has(text, "右后") || text_has(text, "右下") || text_has(text, "右斜后");
    bool want_diag = diag_fl || diag_fr || diag_bl || diag_br;

    if (text_has_any(text, stop_keywords, sizeof(stop_keywords) / sizeof(stop_keywords[0]))) {
        ret = car_stop();
        handled = true;
        LOGW("voice-car: 停止");
    } else if (want_diag) {
        /* 斜向角度: 默认 45; 若说了度数, 吸附到最近的 30/45/60 档。 */
        uint16_t diag_angle = 45U;
        if (has_angle) {
            if (angle_deg < 38U) {
                diag_angle = 30U;
            } else if (angle_deg > 52U) {
                diag_angle = 60U;
            } else {
                diag_angle = 45U;
            }
        }
        if (diag_fl) {
            ret = has_distance ? car_diag_front_left_distance_mm_angle(distance_mm, sp, diag_angle)
                               : car_diag_front_left_angle(sp, diag_angle);
            inst_fn = car_diag_front_left;
            LOGW("voice-car: 左前斜移 deg=%u distance_mm=%lu", diag_angle, (unsigned long)distance_mm);
        } else if (diag_fr) {
            ret = has_distance ? car_diag_front_right_distance_mm_angle(distance_mm, sp, diag_angle)
                               : car_diag_front_right_angle(sp, diag_angle);
            inst_fn = car_diag_front_right;
            LOGW("voice-car: 右前斜移 deg=%u distance_mm=%lu", diag_angle, (unsigned long)distance_mm);
        } else if (diag_bl) {
            ret = has_distance ? car_diag_rear_left_distance_mm_angle(distance_mm, sp, diag_angle)
                               : car_diag_rear_left_angle(sp, diag_angle);
            inst_fn = car_diag_rear_left;
            LOGW("voice-car: 左后斜移 deg=%u distance_mm=%lu", diag_angle, (unsigned long)distance_mm);
        } else {
            ret = has_distance ? car_diag_rear_right_distance_mm_angle(distance_mm, sp, diag_angle)
                               : car_diag_rear_right_angle(sp, diag_angle);
            inst_fn = car_diag_rear_right;
            LOGW("voice-car: 右后斜移 deg=%u distance_mm=%lu", diag_angle, (unsigned long)distance_mm);
        }
        handled = true;
    } else if (want_spin && (has_left || has_right)) {
        if (has_left) {
            ret = has_angle ? car_spin_left_angle_deg(angle_deg, sp) : car_spin_left(sp);
            inst_fn = car_spin_left;
            LOGW("voice-car: 原地左转 angle=%u", angle_deg);
        } else {
            ret = has_angle ? car_spin_right_angle_deg(angle_deg, sp) : car_spin_right(sp);
            inst_fn = car_spin_right;
            LOGW("voice-car: 原地右转 angle=%u", angle_deg);
        }
        handled = true;
    } else if (want_strafe && (has_left || has_right)) {
        if (has_left) {
            ret = has_distance ? car_strafe_left_distance_mm(distance_mm, sp) : car_strafe_left(sp);
            inst_fn = car_strafe_left;
            LOGW("voice-car: 左横移 distance_mm=%lu", (unsigned long)distance_mm);
        } else {
            ret = has_distance ? car_strafe_right_distance_mm(distance_mm, sp) : car_strafe_right(sp);
            inst_fn = car_strafe_right;
            LOGW("voice-car: 右横移 distance_mm=%lu", (unsigned long)distance_mm);
        }
        handled = true;
    } else if (want_turn && (has_left || has_right)) {
        if (has_left) {
            ret = has_angle ? car_turn_left_angle_deg(angle_deg, sp) : car_turn_left(sp);
            inst_fn = car_turn_left;
            LOGW("voice-car: 左转 angle=%u", angle_deg);
        } else {
            ret = has_angle ? car_turn_right_angle_deg(angle_deg, sp) : car_turn_right(sp);
            inst_fn = car_turn_right;
            LOGW("voice-car: 右转 angle=%u", angle_deg);
        }
        handled = true;
    } else if (text_has_any(text, backward_keywords, sizeof(backward_keywords) / sizeof(backward_keywords[0]))) {
        ret = has_distance ? car_backward_distance_mm(distance_mm, sp) : car_backward(sp);
        inst_fn = car_backward;
        handled = true;
        LOGW("voice-car: 后退 distance_mm=%lu", (unsigned long)distance_mm);
    } else if (text_has_any(text, forward_keywords, sizeof(forward_keywords) / sizeof(forward_keywords[0]))) {
        ret = has_distance ? car_forward_distance_mm(distance_mm, sp) : car_forward(sp);
        inst_fn = car_forward;
        handled = true;
        LOGW("voice-car: 前进 distance_mm=%lu", (unsigned long)distance_mm);
    } else if (voice_car_has_speed_word(text) && s_car_last_motion != NULL) {
        /* 单说"加速/减速"等纯速度词: 用新速度档重放上一条即时运动。 */
        ret = s_car_last_motion(sp);
        handled = true;
        LOGW("voice-car: 变速重放上一条运动 sp=%d", (int)sp);
    } else {
        LOGI("voice-car: ignore text=\"%s\"", text);
        return;
    }

    /* 记录本条即时运动(方向), 供下次单说速度词重放。变速重放分支不覆盖(保持原方向)。 */
    if (inst_fn != NULL) {
        s_car_last_motion = inst_fn;
    }

    if (ret != 0) {
        LOGE("voice-car: execute failed, ret=%d, text=\"%s\"", ret, text);
        return;
    }

    if (handled) {
        /* 本地关键词已成功执行，吞掉本轮云端兜底 TTS，但不提前结束整个 session。 */
        s_suppress_next_cloud_tts = true;
    }
}

/* 订阅回调: 先缓存 ASR 整句，等 IAT_END 再统一执行一次。 */
static void voice_car_iat_cb(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    (void)unused;
    (void)len;
    (void)user_data;

    if (msg_id == VOICE_MSG_CLOUD_IAT_START) {
        s_voice_car_iat_text[0] = '\0';
        s_suppress_next_cloud_tts = false;
        return;
    }

    if (msg_id == VOICE_MSG_CLOUD_IAT_UPDATE) {
        if (data != NULL) {
            voice_car_cache_text((const char *)data);
            LOGW("voice-car: ASR update=\"%s\"", s_voice_car_iat_text);
        }
        return;
    }

    if (msg_id == VOICE_MSG_CLOUD_IAT_END) {
        LOGW("voice-car: ASR final=\"%s\"", s_voice_car_iat_text);
        handle_command_text(s_voice_car_iat_text);
    }
}

void voice_car_control_init(void)
{
    s_voice_car_iat_text[0] = '\0';
    s_suppress_next_cloud_tts = false;
    voice_msg_sub(VOICE_MSG_CLOUD_IAT_START, voice_car_iat_cb, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_IAT_UPDATE, voice_car_iat_cb, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_IAT_END, voice_car_iat_cb, NULL);
    LOGW("voice-car: ready (subscribe ASR IAT start/update/end)");
}

bool voice_car_control_consume_cloud_tts_suppress(void)
{
    bool suppress = s_suppress_next_cloud_tts;

    s_suppress_next_cloud_tts = false;
    return suppress;
}
