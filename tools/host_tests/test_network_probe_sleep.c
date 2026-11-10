#include "network_probe_stub.h"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
uint32_t service_offline_action_generation(void);
uint32_t service_offline_action_network_epoch(void);
bool service_offline_action_network_prompt_allowed(void);
/* Compile the real worker, including its retry loop and event publication. */
#include "../../src/system/sys_network_probe.c"

/* Diagnostic history is an independent observer, covered by
 * test_network_diag_ring.py. Keep it outside this worker's sleep/epoch model:
 * recording an event must not change radio state, retry timing or ownership. */
void sys_network_diag_mark(const char *domain, const char *event, int value, const char *detail)
{
    (void)domain;
    (void)event;
    (void)value;
    (void)detail;
}

static jmp_buf finished;
static int mode, takes, queries, success_events, failure_events;
static bool blocked;
static uint32_t epoch;
static uint32_t owner;
static uint32_t conversation;
static uint32_t bearer_generation;
static bool forced;
static bool internet;
static unsigned offline_updates;
uint32_t service_offline_action_generation(void) {return conversation;}
uint32_t service_offline_action_network_epoch(void) {return owner;}
bool service_offline_action_network_prompt_allowed(void) {return !forced;}
bool sys_network_business_reachable(void) {return mode == 13;}
static int sem;
int lisa_semaphore_take(lisa_semaphore_t *s, uint32_t ms)
{
    (void)s;
    if (mode >= 10) {
        assert(ms != LISA_OS_WAIT_FOREVER);
        if (takes++ >= (mode == 10 || mode == 12 ? 3 : 1)) longjmp(finished, 1);
        return mode == 14 ? 0 : -1; /* 14: explicit refresh on a healthy link */
    }
    if (takes++) longjmp(finished, 1);
    return 0;
}
lisa_semaphore_t *lisa_semaphore_create(unsigned n) { (void)n; return &sem; }
int lisa_semaphore_give(lisa_semaphore_t *s) { (void)s; return 0; }
lisa_thread_t *lisa_thread_create(lisa_thread_attr_t *a, void (*f)(void *), void *p)
{ (void)a; (void)f; (void)p; return &sem; }
unsigned lisa_os_get_tick_ms(void) { return queries * 5000U; }
void vTaskDelay(unsigned ms) { (void)ms; if (mode < 10) blocked = true; }
bool sys_wifi_radio_suspended(void) { return blocked; }
uint32_t sys_wifi_radio_epoch(void) { return epoch; }
uint32_t sys_network_generation(void) { return bearer_generation; }
bool sys_network_generation_is_current(uint32_t generation) { return generation == bearer_generation; }
int sys_network_get_status(sys_network_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->active_bearer = SYS_NETWORK_BEARER_WIFI;
    s->wifi_connected = true; /* Deliberately stale upper-layer link status. */
    s->connected = internet;
    return 0;
}
void sys_network_report_probe_result(bool ok) { assert(!blocked); internet=ok; if (!ok) offline_updates++; }
bool sys_network_report_probe_result_for_generation(bool ok, uint32_t generation) {
    if (mode == 16) ++bearer_generation; /* Reassociation just before commit. */
    if (!sys_network_generation_is_current(generation)) return false;
    sys_network_report_probe_result(ok);
    return true;
}
int voice_msg_pub(uint32_t id, void *d, uint32_t n)
{
    assert(d != NULL && n == sizeof(sys_network_probe_event_t));
    sys_network_probe_event_t event;
    memcpy(&event,d,sizeof(event));
    assert(event.owner == owner && event.generation == bearer_generation);
    if (id == 1) success_events++; else failure_events++;
    return 0;
}
int lisa_sntp_query(const char **s, unsigned n, unsigned timeout, struct lisa_sntp_time *t)
{
    (void)s; (void)n; (void)timeout;
    ++queries;
    t->sec = 1; t->nsec = 0;
    if (mode == 15 || mode == 17) ++bearer_generation;
    if (mode >= 15) return mode == 17 ? -1 : 0;
    if (mode >= 10) {
        if (mode == 10 || mode == 12 || mode == 13) return queries == 1 || queries == 2 ? -1 : 0;
        return 0;
    }
    assert(queries == 1); /* A sleep must never produce another retry. */
    if (mode == 2 || mode == 3) blocked = true;
    if (mode == 4) epoch++; /* off + on completed before the old query returns */
    if (mode == 6) owner++; /* Local mode switch on the same Wi-Fi radio. */
    if (mode == 7) forced=true;
    if (mode == 8) network_probe_start(); /* A newer refresh supersedes this attempt. */
    if (mode == 9) conversation++; /* A normal wake must not cancel internet reachability. */
    return mode == 2 || mode == 5 ? -1 : 0;
}
int settimeofday(const struct timeval *tv, const void *tz)
{ (void)tv; (void)tz; return 0; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    mode = atoi(argv[1]); blocked = mode == 1;
    internet = mode >= 10 && mode < 15;
    forced = mode == 12;
    network_probe_sem = &sem;
    if (!setjmp(finished)) network_probe_task(NULL);
    if (mode >= 15) {
        assert(queries == 1 && !internet && !success_events && !failure_events && !offline_updates);
        return 0;
    }
    if (mode >= 10) {
        assert(internet);
        assert(failure_events == (mode == 10 ? 1 : 0));
        assert(success_events == (mode == 10 ? 1 : 0));
        assert(offline_updates == (mode == 10 || mode == 12 ? 1 : 0));
        return 0;
    }
    assert(queries == (mode == 1 ? 0 : 1));
    assert(success_events == (mode == 0 || mode == 9 ? 1 : 0));
    assert(failure_events == (mode == 5 ? 1 : 0));
    return 0;
}
