#include "lingxin_device_info.h"

#include "lingxin_server_config.h"

char *get_lingxin_device_name()
{
    return (char *)lingxin_server_config_device_code();
}

char *get_lingxin_device_version()
{
    return "0.0.1";
}
