#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "lisa_log.h"
#include "mcp.h"
#include "car_motion.h"

#define TAG "mcp_tool_car"

/*
 * L3 MCP 工具: ls.car_move -- 通过语音意图控制小车运动。
 * 上层(语音意图/LLM)根据下面的中文描述选择本工具并填参数, dispatch 到 L2 原子运动。
 */

static bool str_equals_any(const char *value, const char *const *candidates, size_t count)
{
    size_t i = 0;

    if (!value) {
        return false;
    }

    for (i = 0; i < count; ++i) {
        if (strcmp(value, candidates[i]) == 0) {
            return true;
        }
    }

    return false;
}

static const char *car_normalize_action(const cJSON *action_json)
{
    static const char *const forward_aliases[] = {
        "forward", "go_forward", "move_forward", "ahead", "front",
        "前进", "向前", "往前",
    };
    static const char *const backward_aliases[] = {
        "backward", "go_backward", "move_backward", "back", "reverse",
        "后退", "倒退", "向后", "往后",
    };
    static const char *const turn_left_aliases[] = {
        "turn_left", "left_turn", "left", "go_left", "move_left_turn",
        "左转", "向左转", "往左转", "拐左",
    };
    static const char *const turn_right_aliases[] = {
        "turn_right", "right_turn", "right", "go_right", "move_right_turn",
        "右转", "向右转", "往右转", "拐右",
    };
    static const char *const spin_left_aliases[] = {
        "spin_left", "rotate_left", "turn_left_in_place",
        "原地左转", "原地左", "左自转",
    };
    static const char *const spin_right_aliases[] = {
        "spin_right", "rotate_right", "turn_right_in_place",
        "原地右转", "原地右", "右自转",
    };
    static const char *const strafe_left_aliases[] = {
        "strafe_left", "shift_left", "slide_left", "move_left", "sidestep_left",
        "向左平移", "左平移", "横移左", "左移", "侧移左", "向左走",
    };
    static const char *const strafe_right_aliases[] = {
        "strafe_right", "shift_right", "slide_right", "move_right", "sidestep_right",
        "向右平移", "右平移", "横移右", "右移", "侧移右", "向右走",
    };
    static const char *const stop_aliases[] = {
        "stop", "halt", "brake", "pause",
        "停", "停止", "停车", "停下",
    };
    const char *action = NULL;

    if (!action_json || !cJSON_IsString(action_json) || !action_json->valuestring) {
        return NULL;
    }

    action = action_json->valuestring;
    if (str_equals_any(action, forward_aliases, sizeof(forward_aliases) / sizeof(forward_aliases[0]))) {
        return "forward";
    }
    if (str_equals_any(action, backward_aliases, sizeof(backward_aliases) / sizeof(backward_aliases[0]))) {
        return "backward";
    }
    if (str_equals_any(action, turn_left_aliases, sizeof(turn_left_aliases) / sizeof(turn_left_aliases[0]))) {
        return "turn_left";
    }
    if (str_equals_any(action, turn_right_aliases, sizeof(turn_right_aliases) / sizeof(turn_right_aliases[0]))) {
        return "turn_right";
    }
    if (str_equals_any(action, spin_left_aliases, sizeof(spin_left_aliases) / sizeof(spin_left_aliases[0]))) {
        return "spin_left";
    }
    if (str_equals_any(action, spin_right_aliases, sizeof(spin_right_aliases) / sizeof(spin_right_aliases[0]))) {
        return "spin_right";
    }
    if (str_equals_any(action, strafe_left_aliases, sizeof(strafe_left_aliases) / sizeof(strafe_left_aliases[0]))) {
        return "strafe_left";
    }
    if (str_equals_any(action, strafe_right_aliases, sizeof(strafe_right_aliases) / sizeof(strafe_right_aliases[0]))) {
        return "strafe_right";
    }
    if (str_equals_any(action, stop_aliases, sizeof(stop_aliases) / sizeof(stop_aliases[0]))) {
        return "stop";
    }

    return NULL;
}

static car_speed_t car_parse_speed(cJSON *args)
{
    static const char *const high_aliases[] = {
        "high", "fast", "quick", "max", "fastest", "speed_up", "accelerate",
        "高速", "快速", "快", "加速", "提速", "飞快", "全速", "最快",
    };
    static const char *const low_aliases[] = {
        "low", "slow", "min", "slowest", "slow_down", "decelerate",
        "低速", "慢速", "慢", "减速", "降速", "龟速", "最慢",
    };
    const cJSON *speed_json = mcp_tool_call_args_get(args, "speed");

    if (!speed_json) {
        return CAR_SPEED_LOW;
    }

    if (cJSON_IsNumber(speed_json)) {
        return speed_json->valuedouble > 50 ? CAR_SPEED_HIGH : CAR_SPEED_LOW;
    }

    if (cJSON_IsString(speed_json) && speed_json->valuestring) {
        const char *speed = speed_json->valuestring;
        if (str_equals_any(speed, high_aliases, sizeof(high_aliases) / sizeof(high_aliases[0]))) {
            return CAR_SPEED_HIGH;
        }
        if (str_equals_any(speed, low_aliases, sizeof(low_aliases) / sizeof(low_aliases[0]))) {
            return CAR_SPEED_LOW;
        }
    }

    return CAR_SPEED_LOW;
}

static cJSON *car_move_list(const char *name)
{
    cJSON *speed_property = NULL;
    cJSON *speed_enum = NULL;
    cJSON *tool = mcp_tool_list_info_create_default(
        name,
        "小车/车子/车辆运动控制工具。"
        "当用户说“让小车前进”“把车子往前开”“往后退”“左转”“右转”“原地左转”“向左横移”“停车”时必须调用。"
        "支持前进/后退/左转/右转/原地旋转/左右横移/停止。"
        "区分说明: "
        "[转向] turn_left/turn_right=差速转弯,车头偏向一侧同时前进; "
        "[原地旋转] spin_left/spin_right=原地绕中心旋转,车不前进只转圈; "
        "[横移平移] strafe_left/strafe_right=车身整体侧向平移,车头方向不变(麦克纳姆轮专属)。"
        "arguments.action 优先填写以下英文枚举之一: forward/backward/turn_left/turn_right/"
        "spin_left/spin_right/strafe_left/strafe_right/stop。"
        "如果只是笼统说“控制车子/开动车子”，优先理解为 forward。");
    if (!tool) {
        return NULL;
    }

    cJSON *action_property = cJSON_CreateObject();
    cJSON_AddStringToObject(action_property, "type", "string");
    cJSON_AddStringToObject(action_property, "description",
        "运动类型: forward=前进/往前/向前/把车子往前开, "
        "backward=后退/往后/倒退, "
        "turn_left=左转/拐左, turn_right=右转/拐右, "
        "spin_left=原地左转, spin_right=原地右转, "
        "strafe_left=向左横移/左移/向左走, strafe_right=向右横移/右移/向右走, "
        "stop=停止/停车/停下");
    cJSON *action_enum = cJSON_CreateArray();
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("forward"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("backward"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("turn_left"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("turn_right"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("spin_left"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("spin_right"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("strafe_left"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("strafe_right"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("stop"));
    cJSON_AddItemToObject(action_property, "enum", action_enum);
    mcp_tool_info_add_json_property(tool, "action", action_property, true);

    speed_property = cJSON_CreateObject();
    speed_enum = cJSON_CreateArray();
    if (!speed_property || !speed_enum) {
        cJSON_Delete(speed_property);
        cJSON_Delete(speed_enum);
        cJSON_Delete(tool);
        return NULL;
    }
    cJSON_AddStringToObject(speed_property, "type", "string");
    cJSON_AddStringToObject(speed_property, "description",
        "速度档: low=低速, high=高速。可选, 默认 low。stop 时忽略。"
        "用户说快一点/快速/高速时用 high；未明确速度时默认 low。"
        "服务端若生成中文别名如'高速'/'低速'，设备端也会兼容。");
    cJSON_AddItemToArray(speed_enum, cJSON_CreateString("low"));
    cJSON_AddItemToArray(speed_enum, cJSON_CreateString("high"));
    cJSON_AddItemToObject(speed_property, "enum", speed_enum);
    mcp_tool_info_add_json_property(tool, "speed", speed_property, false);

    return tool;
}

static cJSON *car_move_result(const char *name, const char *text, bool is_error)
{
    cJSON *result = mcp_tool_call_result_create(name);
    if (!result) {
        return NULL;
    }
    cJSON *content_array = cJSON_CreateArray();
    cJSON *content_item = cJSON_CreateObject();
    cJSON_AddStringToObject(content_item, "type", "text");
    cJSON_AddStringToObject(content_item, "text", text);
    cJSON_AddItemToArray(content_array, content_item);
    cJSON_AddItemToObject(result, "content", content_array);
    cJSON_AddBoolToObject(result, "isError", is_error);
    return result;
}

static cJSON *car_move_call(const char *id, const char *name, cJSON *args)
{
    (void)id;
    const cJSON *action_json = mcp_tool_call_args_get(args, "action");
    const char *action = car_normalize_action(action_json);
    const char *raw_action = (action_json && cJSON_IsString(action_json)) ? action_json->valuestring : NULL;
    car_speed_t speed = CAR_SPEED_LOW;
    const char *reply = NULL;
    int ret = 0;

    if (!raw_action) {
        LOGE("car_move: action parameter missing/invalid");
        return car_move_result(name, "缺少 action 参数", true);
    }

    if (!car_motion_is_ready()) {
        LOGE("car_move: car motion backend not ready");
        return car_move_result(name, "车辆控制未就绪，请检查舵机/PWM初始化日志", true);
    }

    speed = car_parse_speed(args);

    LOGI("car_move: raw_action=%s action=%s speed=%s",
         raw_action, action ? action : "unknown", speed == CAR_SPEED_HIGH ? "high" : "low");

    if (!action) {
        char error_text[128];
        snprintf(error_text, sizeof(error_text),
                 "不支持的 action: %s，请使用 forward/backward/turn_left/turn_right/"
                 "spin_left/spin_right/strafe_left/strafe_right/stop",
                 raw_action);
        LOGE("car_move: unknown action %s", raw_action);
        return car_move_result(name, error_text, true);
    }

    if (strcmp(action, "forward") == 0) {
        ret = car_forward(speed);
        reply = "正在前进";
    } else if (strcmp(action, "backward") == 0) {
        ret = car_backward(speed);
        reply = "正在后退";
    } else if (strcmp(action, "turn_left") == 0) {
        ret = car_turn_left(speed);
        reply = "正在左转";
    } else if (strcmp(action, "turn_right") == 0) {
        ret = car_turn_right(speed);
        reply = "正在右转";
    } else if (strcmp(action, "spin_left") == 0) {
        ret = car_spin_left(speed);
        reply = "正在原地左转";
    } else if (strcmp(action, "spin_right") == 0) {
        ret = car_spin_right(speed);
        reply = "正在原地右转";
    } else if (strcmp(action, "strafe_left") == 0) {
        ret = car_strafe_left(speed);
        reply = "正在向左横移";
    } else if (strcmp(action, "strafe_right") == 0) {
        ret = car_strafe_right(speed);
        reply = "正在向右横移";
    } else if (strcmp(action, "stop") == 0) {
        ret = car_stop();
        reply = "已停车";
    }

    if (ret != 0) {
        LOGE("car_move: execute action %s failed ret=%d", action, ret);
        return car_move_result(name, "车辆控制执行失败，请检查底层驱动日志", true);
    }

    return car_move_result(name, reply, false);
}

/* 非 built_in 自定义工具(与已验证可用的 ls.set_volume 同一机制): 设备经 tools/list
 * 上报, 云端 LLM 可调用。 */
MCP_TOOL_DEFINE(ls.car_move, car_move_list, car_move_call);
MCP_TOOL_DEFINE(ls.control_car, car_move_list, car_move_call);
