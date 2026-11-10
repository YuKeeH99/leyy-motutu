#include "service_codex_discovery.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lisa_time.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"

#define DISCOVERY_PORT 8767U
#define DISCOVERY_TIMEOUT_MS 450U
#define DISCOVERY_ATTEMPTS 2U

static bool valid_reply(const char *reply, int length, const char *nonce, const char *address, bool v2,
                        char port[CODEX_PORT_BYTES])
{
    static const char digits[] = "0123456789abcdef";
    char payload[48];
    uint8_t digest[32];
    int size = snprintf(payload, sizeof(payload), "LS26%s! %s %s", v2 ? "2" : "", nonce, address);
    if (size <= 0 || size >= (int)sizeof(payload)) return false;
    int port_start = 0;
    if (v2) {
        if (length <= size + 1 || reply[size] != ' ') return false;
        int start = ++size;
        port_start = start;
        unsigned value = 0U;
        while (size < length && size - start < 5 && reply[size] >= '0' && reply[size] <= '9') {
            value = value * 10U + (unsigned)(reply[size] - '0');
            ++size;
        }
        if (!value || value > 65535U || reply[start] == '0' || size >= (int)sizeof(payload))
            return false;
        memcpy(payload + start - 1, reply + start - 1, size - start + 1);
    }
    if (size <= 0 || size >= (int)sizeof(payload) ||
        length != size + 1 + 2 * (int)sizeof(digest) ||
        memcmp(reply, payload, size) != 0 || reply[size] != ' ') return false;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, (const uint8_t *)CONFIG_APP_CODEX_STATUS_TOKEN,
                              strlen(CONFIG_APP_CODEX_STATUS_TOKEN),
                              (const uint8_t *)payload, size, digest) != 0) return false;
    unsigned different = 0U;
    for (unsigned i = 0; i < sizeof(digest); ++i) {
        different |= reply[size + 1 + 2U * i] ^ digits[digest[i] >> 4U];
        different |= reply[size + 2 + 2U * i] ^ digits[digest[i] & 15U];
    }
    if (different != 0U) return false;
    if (v2) {
        size_t digits_count = (size_t)(size - port_start);
        memcpy(port, payload + port_start, digits_count);
        port[digits_count] = '\0';
    } else {
        snprintf(port, CODEX_PORT_BYTES, "%s", CONFIG_APP_CODEX_STATUS_PORT);
    }
    return true;
}

static bool discover(char host[CODEX_HOST_BYTES], char port[CODEX_PORT_BYTES], bool v2)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    const int enabled = 1;
    const struct timeval timeout = {.tv_usec = DISCOVERY_TIMEOUT_MS * 1000U};
    struct sockaddr_in broadcast = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVERY_PORT),
        .sin_addr = {.s_addr = htonl(INADDR_BROADCAST)},
    };
    char nonce[17], request[25];
    snprintf(nonce, sizeof(nonce), "%08lx%08lx",
             (unsigned long)lisa_rand32(), (unsigned long)lisa_rand32());
    int size = snprintf(request, sizeof(request), "LS26%s? %s", v2 ? "2" : "", nonce);
    bool found = false;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0) {
        for (unsigned attempt = 0; attempt < DISCOVERY_ATTEMPTS && !found; ++attempt) {
            if (sendto(fd, request, size, 0, (const struct sockaddr *)&broadcast, sizeof(broadcast)) != size)
                continue;
            TickType_t started = xTaskGetTickCount();
            do {
                struct sockaddr_in source;
                socklen_t source_len = sizeof(source);
                char reply[112], address[CODEX_HOST_BYTES];
                int length = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&source, &source_len);
                if (length < 0) break;
                if (source.sin_family == AF_INET &&
                    inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address)) &&
                    valid_reply(reply, length, nonce, address, v2, port)) {
                    strcpy(host, address);
                    found = true;
                    break;
                }
            } while ((TickType_t)(xTaskGetTickCount() - started) < pdMS_TO_TICKS(DISCOVERY_TIMEOUT_MS));
        }
    }
    closesocket(fd);
    return found;
}

bool service_codex_discover(char host[CODEX_HOST_BYTES], char port[CODEX_PORT_BYTES])
{
    return discover(host, port, true) || discover(host, port, false);
}
