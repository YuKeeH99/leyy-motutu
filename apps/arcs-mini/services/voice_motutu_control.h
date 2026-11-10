#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void voice_motutu_control_init(void);
bool voice_motutu_control_consume_cloud_tts_suppress(void);

#ifdef __cplusplus
}
#endif
