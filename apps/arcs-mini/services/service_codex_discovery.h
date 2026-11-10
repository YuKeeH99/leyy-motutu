#pragma once

#include <stdbool.h>

/* IPv4 dotted decimal plus the terminator. */
#define CODEX_HOST_BYTES 16U

#define CODEX_PORT_BYTES 6U

bool service_codex_discover(char host[CODEX_HOST_BYTES], char port[CODEX_PORT_BYTES]);
