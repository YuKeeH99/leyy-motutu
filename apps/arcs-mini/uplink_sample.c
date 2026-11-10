/* Temporary, explicitly armed RAM-only diagnostic. Not a production recorder. */
#ifndef UPLINK_SAMPLE_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "sysutils.h"
#include "lisa_thread.h"
#include "lisa_time.h"
#include "shell.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#endif

#define SAMPLE_BYTES 640000U
#define SAMPLE_FRAMES 500U
static uint8_t s_pcm[SAMPLE_BYTES] __psram_bss__;
static struct { uint32_t offset, length, epoch, ms; } s_meta[SAMPLE_FRAMES] __psram_bss__;
static uint32_t s_bytes, s_frames, s_arm_ms, s_first_ms;
static bool s_active, s_used;

static int sample_arm(void)
{
    taskENTER_CRITICAL();
    if (s_used) { taskEXIT_CRITICAL(); return -1; }
    s_used = s_active = true;
    s_bytes = s_frames = 0;
    s_arm_ms = lisa_os_get_tick_ms();
    taskEXIT_CRITICAL();
    return 0;
}

/* Called only for PCM accepted by the WebSocket queue, before caller reuse. */
void lingxin_uplink_sample_append(const void *data, uint32_t size, uint32_t epoch)
{
    uint32_t now = lisa_os_get_tick_ms();
    taskENTER_CRITICAL();
    if (!s_active) { taskEXIT_CRITICAL(); return; }
    if ((!s_frames && now - s_arm_ms >= 60000U) ||
        (s_frames && now - s_first_ms >= 20000U) ||
        s_frames >= SAMPLE_FRAMES || size > SAMPLE_BYTES - s_bytes) {
        s_active = false;
        taskEXIT_CRITICAL();
        return;
    }
    if (!data || !size) { taskEXIT_CRITICAL(); return; }
    if (!s_frames) s_first_ms = now;
    s_meta[s_frames].offset = s_bytes;
    s_meta[s_frames].length = size;
    s_meta[s_frames].epoch = epoch;
    s_meta[s_frames].ms = now - s_first_ms;
    memcpy(s_pcm + s_bytes, data, size);
    s_bytes += size;
    s_frames++;
    if (s_bytes == SAMPLE_BYTES || s_frames == SAMPLE_FRAMES) s_active = false;
    taskEXIT_CRITICAL();
}

#ifndef UPLINK_SAMPLE_TEST
static int uplink_sample_cmd(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();
    if (argc == 2 && strcmp(argv[1], "arm") == 0) {
        shellPrint(sh, "SAMPLE arm=%d max_pcm_ms=20000 one_shot=1\n", sample_arm());
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "stop") == 0 ||
                      strcmp(argv[1], "dump") == 0 || strcmp(argv[1], "clear") == 0)) {
        taskENTER_CRITICAL();
        s_active = false;
        taskEXIT_CRITICAL();
        if (strcmp(argv[1], "clear") == 0) {
            memset(s_pcm, 0, s_bytes);
            memset(s_meta, 0, sizeof(s_meta));
            s_bytes = s_frames = 0;
            shellPrint(sh, "SAMPLE cleared\n");
            return 0;
        }
        if (strcmp(argv[1], "dump") == 0) {
            uint32_t hash = 2166136261U;
            shellPrint(sh, "SAMPLE BEGIN bytes=%u frames=%u\n", s_bytes, s_frames);
            for (uint32_t i = 0; i < s_frames; ++i) {
                shellPrint(sh, "SM %u %u %u %u\n", s_meta[i].offset,
                           s_meta[i].length, s_meta[i].epoch, s_meta[i].ms);
            }
            for (uint32_t offset = 0; offset < s_bytes; offset += 48U) {
                static const char hex[] = "0123456789abcdef";
                char line[97];
                uint32_t n = s_bytes - offset;
                if (n > 48U) n = 48U;
                for (uint32_t j = 0; j < n; ++j) {
                    uint8_t b = s_pcm[offset+j];
                    line[2*j] = hex[b >> 4]; line[2*j+1] = hex[b & 15];
                    hash = (hash ^ b) * 16777619U;
                }
                line[2*n] = 0;
                shellPrint(sh, "SD %u %s\n", offset, line);
                if ((offset / 48U) % 32U == 31U) lisa_thread_mdelay(10);
            }
            shellPrint(sh, "SAMPLE END bytes=%u fnv=%08x\n", s_bytes, hash);
            return 0;
        }
    }
    taskENTER_CRITICAL();
    uint32_t bytes = s_bytes, frames = s_frames;
    bool active = s_active;
    taskEXIT_CRITICAL();
    shellPrint(sh, "SAMPLE status active=%u bytes=%u frames=%u\n", active, bytes, frames);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                 SHELL_CMD_DISABLE_RETURN, uplink_sample, uplink_sample_cmd, Temporary RAM capture);
#endif
