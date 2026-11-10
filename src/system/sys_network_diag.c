#include "sys_network_diag.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lisa_time.h"

#ifdef CONFIG_NETWORK_DIALOG_TIMELINE_LOG
#define TAG "net.dialog"
#include "lisa_log.h"
#endif

static sys_network_diag_entry_t s_network_diag_ring[SYS_NETWORK_DIAG_CAPACITY];
static uint32_t s_network_diag_seq;
static uint32_t s_network_diag_count;

static void diag_copy_text(char *target, size_t capacity, const char *source)
{
    size_t length = 0U;
    if (source == NULL) source = "-";
    while (length + 1U < capacity && source[length] != '\0') {
        target[length] = source[length];
        ++length;
    }
    target[length] = '\0';
}

void sys_network_diag_mark(const char *domain, const char *event, int value, const char *detail)
{
    sys_network_diag_entry_t entry = {
        .uptime_ms = (uint32_t)lisa_os_get_tick_ms(),
        .value = value,
    };
    diag_copy_text(entry.domain, sizeof(entry.domain), domain);
    diag_copy_text(entry.event, sizeof(entry.event), event);
    diag_copy_text(entry.detail, sizeof(entry.detail), detail);

    /* Callers may already own the network manager lock. Never acquire it here;
     * the only shared operation is a bounded, single-record RAM copy. */
    taskENTER_CRITICAL();
    entry.seq = ++s_network_diag_seq;
    s_network_diag_ring[(entry.seq - 1U) % SYS_NETWORK_DIAG_CAPACITY] = entry;
    if (s_network_diag_count < SYS_NETWORK_DIAG_CAPACITY) ++s_network_diag_count;
    taskEXIT_CRITICAL();

#ifdef CONFIG_NETWORK_DIALOG_TIMELINE_LOG
    LISA_LOGI(TAG,
              "[NET-HD] seq=%u uptime=%ums domain=%s event=%s value=%d detail=%s",
              (unsigned)entry.seq, (unsigned)entry.uptime_ms,
              entry.domain, entry.event, entry.value, entry.detail);
#endif
}

uint32_t sys_network_diag_bounds(uint32_t *first_seq)
{
    taskENTER_CRITICAL();
    uint32_t count = s_network_diag_count;
    if (first_seq != NULL) *first_seq = count != 0U ? s_network_diag_seq - count + 1U : 0U;
    taskEXIT_CRITICAL();
    return count;
}

bool sys_network_diag_read(uint32_t seq, sys_network_diag_entry_t *entry)
{
    if (entry == NULL) return false;
    taskENTER_CRITICAL();
    bool present = (uint32_t)(s_network_diag_seq - seq) < s_network_diag_count;
    if (present) *entry = s_network_diag_ring[(seq - 1U) % SYS_NETWORK_DIAG_CAPACITY];
    taskEXIT_CRITICAL();
    return present;
}
