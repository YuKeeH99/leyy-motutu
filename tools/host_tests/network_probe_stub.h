#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>
int settimeofday(const struct timeval *, const void *);
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#define LISA_OS_WAIT_FOREVER UINT32_MAX
#define LISA_OS_PRIORITY_NORMAL 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int lisa_semaphore_t;
typedef int lisa_thread_t;
typedef struct { const char *name; unsigned stack_size, priority; } lisa_thread_attr_t;
struct lisa_sntp_time { uint32_t sec, nsec; };
int lisa_semaphore_take(lisa_semaphore_t *, uint32_t);
lisa_semaphore_t *lisa_semaphore_create(unsigned);
int lisa_semaphore_give(lisa_semaphore_t *);
lisa_thread_t *lisa_thread_create(lisa_thread_attr_t *, void (*)(void *), void *);
int lisa_sntp_query(const char **, unsigned, unsigned, struct lisa_sntp_time *);
unsigned lisa_os_get_tick_ms(void);
void vTaskDelay(unsigned);
enum { VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS=1, VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL=2 };
int voice_msg_pub(uint32_t, void *, uint32_t);
