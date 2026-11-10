#include "service_voice_query_policy.h"

#include <stdio.h>
#include <string.h>

static bool query_code_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 'a' + 'A') : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 'a' + 'A') : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool query_snprintf_ok(int result, size_t text_size)
{
    return result >= 0 && (size_t)result < text_size;
}

bool service_voice_query_from_cloud_code(const char *code, service_voice_query_t *query)
{
    if (query == NULL) {
        return false;
    }
    if (query_code_equal(code, "GET_SYSTEM_VERSION")) {
        *query = SERVICE_VOICE_QUERY_SYSTEM_VERSION;
        return true;
    }
    if (query_code_equal(code, "GET_DEVICE_CODE")) {
        *query = SERVICE_VOICE_QUERY_DEVICE_CODE;
        return true;
    }
    if (query_code_equal(code, "GET_POWER_STATUS")) {
        *query = SERVICE_VOICE_QUERY_POWER_STATUS;
        return true;
    }
    *query = SERVICE_VOICE_QUERY_NONE;
    return false;
}

bool service_voice_query_format_version(char *text, size_t text_size, const char *version)
{
    if (text == NULL || text_size == 0U) {
        return false;
    }
    if (version == NULL || version[0] == '\0') {
        return query_snprintf_ok(snprintf(text, text_size, "暂时无法获取系统版本。"), text_size);
    }
    return query_snprintf_ok(snprintf(text, text_size, "当前系统版本是%s。", version), text_size);
}

bool service_voice_query_format_device_code(char *text, size_t text_size, const char *device_code)
{
    if (text == NULL || text_size == 0U) {
        return false;
    }
    if (device_code == NULL || device_code[0] == '\0') {
        return query_snprintf_ok(snprintf(text, text_size, "当前设备还没有写入设备号。"), text_size);
    }

    size_t length = strlen(device_code);
    size_t suffix_length = length > 5U ? 5U : length;
    const char *suffix = device_code + length - suffix_length;
    char spaced_suffix[10];
    size_t out = 0U;
    for (size_t i = 0U; i < suffix_length; ++i) {
        if (i != 0U) {
            spaced_suffix[out++] = ' ';
        }
        spaced_suffix[out++] = suffix[i];
    }
    spaced_suffix[out] = '\0';

    return query_snprintf_ok(
        snprintf(text, text_size, "当前设备号后五位是%s。", spaced_suffix), text_size);
}

bool service_voice_query_format_power(char *text, size_t text_size, uint8_t level,
                                      bool available, bool charging, bool full)
{
    if (text == NULL || text_size == 0U) {
        return false;
    }
    if (!available) {
        return query_snprintf_ok(snprintf(text, text_size, "暂时无法获取电量信息。"), text_size);
    }
    if (level > 100U) {
        level = 100U;
    }
    if (full) {
        return query_snprintf_ok(
            snprintf(text, text_size, "当前电量百分之%u，已经充满。", (unsigned)level), text_size);
    }
    if (charging) {
        return query_snprintf_ok(
            snprintf(text, text_size, "当前电量百分之%u，正在充电。", (unsigned)level), text_size);
    }
    return query_snprintf_ok(
        snprintf(text, text_size, "当前电量百分之%u。", (unsigned)level), text_size);
}
