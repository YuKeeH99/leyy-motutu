#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lisa_websocket.h"

bool service_codex_voice_route_wakeup(void);
void service_codex_voice_capture(const int16_t *samples, uint32_t count);
void service_codex_voice_cache_chunk(uint32_t generation, const void *data, uint32_t size);
void service_codex_voice_reply(const char *message, uint32_t length);
bool service_codex_voice_poll(lisa_ws_t *ws);
