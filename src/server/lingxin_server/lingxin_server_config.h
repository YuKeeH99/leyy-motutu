#pragma once

#ifdef __cplusplus
extern "C" {
#endif

const char *lingxin_server_config_cloud_host(void);
int lingxin_server_config_cloud_port(void);
const char *lingxin_server_config_app_id(void);
const char *lingxin_server_config_license(void);
const char *lingxin_server_config_app_code(void);
const char *lingxin_server_config_device_code(void);
const char *lingxin_server_config_sn(void);
int lingxin_server_config_set(const char *app_id,
                              const char *license,
                              const char *sn,
                              const char *app_code,
                              const char *device_code);
int lingxin_server_config_clear(void);
int lingxin_server_config_reload(void);
int lingxin_server_config_fetch(void);
int lingxin_server_config_provisioned(void);
int lingxin_server_config_ready(void);
void lingxin_server_config_dump(void);

#ifdef __cplusplus
}
#endif
