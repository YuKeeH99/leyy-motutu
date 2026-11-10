#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int service_motutu_web_init(void);
bool service_motutu_web_get_control_url(char *buf, size_t len);
int service_motutu_web_show_control_page(void);

#ifdef __cplusplus
}
#endif
