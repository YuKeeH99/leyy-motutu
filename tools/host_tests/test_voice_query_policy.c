#include <assert.h>
#include <string.h>

#include "service_voice_query_policy.h"

int main(void)
{
    service_voice_query_t query = SERVICE_VOICE_QUERY_NONE;
    char text[128];

    assert(service_voice_query_from_cloud_code("GET_SYSTEM_VERSION", &query));
    assert(query == SERVICE_VOICE_QUERY_SYSTEM_VERSION);
    assert(service_voice_query_from_cloud_code("get_device_code", &query));
    assert(query == SERVICE_VOICE_QUERY_DEVICE_CODE);
    assert(service_voice_query_from_cloud_code("GET_POWER_STATUS", &query));
    assert(query == SERVICE_VOICE_QUERY_POWER_STATUS);
    assert(!service_voice_query_from_cloud_code("DOG_STAND_UP", &query));

    assert(service_voice_query_format_version(text, sizeof(text), "2.6.3"));
    assert(strcmp(text, "当前系统版本是2.6.3。") == 0);

    assert(service_voice_query_format_device_code(text, sizeof(text), "AIT81_AMFaVxxB5UuTNTIRG4RtKkgR"));
    assert(strcmp(text, "当前设备号后五位是t K k g R。") == 0);
    assert(strstr(text, "AIT81_") == NULL);
    assert(service_voice_query_format_device_code(text, sizeof(text), "123"));
    assert(strcmp(text, "当前设备号后五位是1 2 3。") == 0);
    assert(service_voice_query_format_device_code(text, sizeof(text), ""));
    assert(strcmp(text, "当前设备还没有写入设备号。") == 0);

    assert(service_voice_query_format_power(text, sizeof(text), 88U, true, false, false));
    assert(strcmp(text, "当前电量百分之88。") == 0);
    assert(service_voice_query_format_power(text, sizeof(text), 56U, true, true, false));
    assert(strcmp(text, "当前电量百分之56，正在充电。") == 0);
    assert(service_voice_query_format_power(text, sizeof(text), 100U, true, false, true));
    assert(strcmp(text, "当前电量百分之100，已经充满。") == 0);
    assert(service_voice_query_format_power(text, sizeof(text), 0U, false, false, false));
    assert(strcmp(text, "暂时无法获取电量信息。") == 0);
    return 0;
}
