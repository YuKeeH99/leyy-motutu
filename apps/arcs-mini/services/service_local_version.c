#include "service_local_version.h"

size_t service_local_version_tokens(const char *version, uint16_t *tokens, size_t capacity)
{
    if (!version || !tokens || capacity < 2U) return 0;
    size_t length = 0;
    while (length < LOCAL_VERSION_TOKEN_MAX && version[length]) ++length;
    if (!length || length >= LOCAL_VERSION_TOKEN_MAX || length + 1U > capacity) return 0;
    for (size_t i = 0; i < length; ++i) {
        char ch = version[i];
        if (ch >= '0' && ch <= '9') continue;
        if (ch != '.' || i == 0 || i + 1U == length || version[i-1] == '.') return 0;
    }
    tokens[0] = 119; /* Current system version. */
    for (size_t i = 0; i < length; ++i)
        tokens[i + 1U] = version[i] == '.' ? 120U : (uint16_t)(121U + version[i] - '0');
    return length + 1U;
}
