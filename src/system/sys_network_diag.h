#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_NETWORK_DIAG_CAPACITY 32U

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    int value;
    char domain[16];
    char event[48];
    char detail[64];
} sys_network_diag_entry_t;

/* Task-context diagnostic history, retained even when timeline logging is off.
 * Use fixed reason labels and numeric details only: never pass SSIDs, passwords,
 * access tokens, credentials or user content. Strings are copied and truncated.
 * The fixed RAM history is lost on reboot; no files or audio are recorded. */
void sys_network_diag_mark(const char *domain, const char *event, int value, const char *detail);

/* Capture the current count and first sequence (zero when empty). Sequences
 * wrap modulo uint32_t. No ring lock is held when either function returns. */
uint32_t sys_network_diag_bounds(uint32_t *first_seq);

/* Copy one record. False means invalid output or an absent/overwritten sequence;
 * callers printing a captured range should skip such records, never retry it. */
bool sys_network_diag_read(uint32_t seq, sys_network_diag_entry_t *entry);

#ifdef __cplusplus
}
#endif
