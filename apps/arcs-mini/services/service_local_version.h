#ifndef SERVICE_LOCAL_VERSION_H
#define SERVICE_LOCAL_VERSION_H
#include <stddef.h>
#include <stdint.h>
#define LOCAL_VERSION_TOKEN_MAX 32U
/* Returns zero for malformed/truncated input; no partial phrase is playable. */
size_t service_local_version_tokens(const char *version, uint16_t *tokens, size_t capacity);
#endif
