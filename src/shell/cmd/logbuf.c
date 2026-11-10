#include "cmd.h"
#include "shell.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log_buffer.h"

#define LOGBUF_DEFAULT_DUMP_BYTES 8192U
#define LOGBUF_MAX_DUMP_BYTES     (64U * 1024U)
#define LOGBUF_PRINT_CHUNK        240U

static uint32_t logbuf_parse_dump_bytes(int argc, char **argv)
{
    char *endptr = NULL;
    unsigned long value = 0;

    if (argc < 2 || argv[1] == NULL) {
        return LOGBUF_DEFAULT_DUMP_BYTES;
    }

    value = strtoul(argv[1], &endptr, 10);
    if (endptr == argv[1] || *endptr != '\0' || value == 0UL) {
        return LOGBUF_DEFAULT_DUMP_BYTES;
    }
    if (value > LOGBUF_MAX_DUMP_BYTES) {
        return LOGBUF_MAX_DUMP_BYTES;
    }
    return (uint32_t)value;
}

static void logbuf_print_span_tail(const uint8_t *data,
                                   uint32_t size,
                                   uint32_t skip_bytes,
                                   uint32_t *printed_bytes)
{
    uint32_t offset = 0;

    if (data == NULL || printed_bytes == NULL || size <= skip_bytes) {
        return;
    }

    offset = skip_bytes;
    while (offset < size) {
        uint32_t chunk = size - offset;
        if (chunk > LOGBUF_PRINT_CHUNK) {
            chunk = LOGBUF_PRINT_CHUNK;
        }
        shellPrint(shellGetCurrent(), "%.*s", (int)chunk, (const char *)(data + offset));
        offset += chunk;
        *printed_bytes += chunk;
    }
}

static int logbuf_cmd_dump(int argc, char **argv)
{
    struct log_buffer_snapshot snapshot = {0};
    uint32_t dump_bytes = logbuf_parse_dump_bytes(argc, argv);
    uint32_t skip_total = 0;
    uint32_t printed = 0;
    int ret = log_buffer_snapshot_acquire(&snapshot);

    if (ret != 0) {
        shellPrint(shellGetCurrent(), "log buffer snapshot unavailable: %d\n", ret);
        return -1;
    }

    if (snapshot.valid_bytes == 0U) {
        shellPrint(shellGetCurrent(), "log buffer empty\n");
        log_buffer_snapshot_release();
        return 0;
    }

    if (dump_bytes > snapshot.valid_bytes) {
        dump_bytes = snapshot.valid_bytes;
    }
    skip_total = snapshot.valid_bytes - dump_bytes;

    shellPrint(shellGetCurrent(),
               "log buffer dump: showing last %u / %u bytes\n",
               dump_bytes,
               snapshot.valid_bytes);

    if (skip_total < snapshot.first.size) {
        logbuf_print_span_tail(snapshot.first.data, snapshot.first.size, skip_total, &printed);
        logbuf_print_span_tail(snapshot.second.data, snapshot.second.size, 0U, &printed);
    } else {
        uint32_t second_skip = skip_total - snapshot.first.size;
        logbuf_print_span_tail(snapshot.second.data, snapshot.second.size, second_skip, &printed);
    }

    if (printed > 0U) {
        shellPrint(shellGetCurrent(), "\n");
    }

    log_buffer_snapshot_release();
    return 0;
}

static int logbuf_cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    log_buffer_reset();
    shellPrint(shellGetCurrent(), "log buffer reset\n");
    return 0;
}

static int logbuf_cmd_help(int argc, char **argv);

static const struct listen_cmd_t g_logbuf_cmds[] = {
    {"dump", logbuf_cmd_dump, "Dump recent log buffer tail, ex: logbuf dump 16384"},
    {"reset", logbuf_cmd_reset, "Reset buffered logs, ex: logbuf reset"},
    {"help", logbuf_cmd_help, "Show help information"},
};

static int logbuf_cmd_help(int argc, char **argv)
{
    int i;

    (void)argc;
    (void)argv;

    shellPrint(shellGetCurrent(), "Log buffer commands:\n");
    for (i = 0; i < (int)(sizeof(g_logbuf_cmds) / sizeof(g_logbuf_cmds[0])); i++) {
        shellPrint(shellGetCurrent(), "%-10s\t:\t%s\n", g_logbuf_cmds[i].name, g_logbuf_cmds[i].help);
    }
    return 0;
}

static int logbuf_cmd_handler(int argc, char **argv)
{
    int i;

    if (argc <= 1) {
        return logbuf_cmd_help(argc, argv);
    }

    for (i = 0; i < (int)(sizeof(g_logbuf_cmds) / sizeof(g_logbuf_cmds[0])); i++) {
        if (strcmp(g_logbuf_cmds[i].name, argv[1]) == 0) {
            return g_logbuf_cmds[i].exec(argc - 1, argv + 1);
        }
    }

    return logbuf_cmd_help(argc, argv);
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 logbuf,
                 logbuf_cmd_handler,
                 Dump recent buffered logs);
