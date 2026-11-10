#ifndef SERVICE_VOICE_QUERY_POLICY_H
#define SERVICE_VOICE_QUERY_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVICE_VOICE_QUERY_NONE = 0,
    SERVICE_VOICE_QUERY_SYSTEM_VERSION,
    SERVICE_VOICE_QUERY_DEVICE_CODE,
    SERVICE_VOICE_QUERY_POWER_STATUS,
} service_voice_query_t;

bool service_voice_query_from_cloud_code(const char *code, service_voice_query_t *query);
bool service_voice_query_format_version(char *text, size_t text_size, const char *version);
bool service_voice_query_format_device_code(char *text, size_t text_size, const char *device_code);
bool service_voice_query_format_power(char *text, size_t text_size, uint8_t level,
                                      bool available, bool charging, bool full);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_VOICE_QUERY_POLICY_H */
