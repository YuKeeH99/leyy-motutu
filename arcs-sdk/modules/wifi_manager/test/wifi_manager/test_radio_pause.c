/* Run production wifi_manager.c; HAL hooks schedule transitions at exact call boundaries. */
#include "wifi_manager/wifi_manager.h"
#include "wifi_manager/wifi_manager_storage.h"
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Weak references let the pre-implementation run fail as an explicit contract assertion. */
extern int wifi_mgr_radio_pause(uint32_t timeout_ms) __attribute__((weak));
extern int wifi_mgr_radio_resume(void) __attribute__((weak));
extern bool wifi_mgr_radio_is_paused(void) __attribute__((weak));
extern bool wifi_mgr_auto_connect_exhausted(void) __attribute__((weak));

#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #expr); exit(1); \
} } while (0)

static int locked, scan_calls, connect_calls, push_calls, enabled;
static bool event_during_init;
static bool queue_full;
static int critical;
void radio_test_enter_critical(void) { CHECK(!critical); critical = 1; }
void radio_test_exit_critical(void) { CHECK(critical); critical = 0; }
static int scan_result, connect_result;
static bool connect_emits_terminal;
static int storage_error;
static char attempted_bssids[32][18];
static uint64_t now_ms;
static void (*scan_hook)(void), (*connect_hook)(void), (*delay_hook)(void);
static wifi_mgr_wifi_event_cb_t *callback;
static wifi_manager_os_thread_entry_t worker;
static void *worker_arg;
static jmp_buf worker_return;
static int pop_budget;
static unsigned char queued[16][64];
static size_t queue_item_size;
static unsigned head, tail;
static wifi_mgr_sta_config_t saved_ap = {.ssid = "saved", .pwd = "password"};
static wifi_mgr_autoconn_config_t autoconn = {.interval_ms = 1000};

static void *mutex_create(void) { return &locked; }
static int mutex_lock(void *mutex, uint32_t timeout)
{ (void)timeout; CHECK(mutex == &locked); CHECK(!critical && !locked); locked = 1; return 0; }
static int mutex_unlock(void *mutex)
{ (void)mutex; CHECK(locked); locked = 0; return 0; }
static void object_delete(void *object) { (void)object; }
static void *queue_create(uint32_t length, const char *name, size_t size)
{ (void)length; (void)name; CHECK(size <= 64); queue_item_size = size; return queued; }
static int queue_push(void *queue, const void *item, size_t size, uint32_t timeout)
{
    (void)queue; CHECK(size == queue_item_size);
    CHECK(!critical);
    if (queue_full) { CHECK(timeout == 0); return -EAGAIN; }
    CHECK(tail - head < 16); memcpy(queued[tail++ % 16], item, size); push_calls++; return 0;
}
static int queue_pop(void *queue, void *item, size_t size, uint32_t timeout)
{
    (void)queue; (void)timeout;
    if (!pop_budget--) longjmp(worker_return, 1);
    if (head == tail) return -ETIMEDOUT;
    memcpy(item, queued[head++ % 16], size); return 0;
}
static void *thread_create(wifi_manager_os_thread_attr_t *attr,
                           wifi_manager_os_thread_entry_t entry, void *arg)
{ (void)attr; worker = entry; worker_arg = arg; return &worker; }
static void run_worker(int iterations)
{ pop_budget = iterations; if (!setjmp(worker_return)) worker(worker_arg); }
uint64_t lisa_os_get_tick_ms(void) { return now_ms; }
int lisa_thread_mdelay(uint32_t ms)
{
    CHECK(!critical && !locked); now_ms += ms;
    if (delay_hook) { void (*hook)(void) = delay_hook; delay_hook = NULL; hook(); }
    return 0;
}
static void *align_alloc(size_t alignment, size_t size) { (void)alignment; return malloc(size); }
static int wifi_init(void) { return 0; }
static int sta_enable(void) { CHECK(!locked); enabled = 1; return 0; }
static int sta_disable(void) { CHECK(!locked); enabled = 0; return 0; }
static bool sta_is_enabled(void) { return enabled; }
static int add_callback(wifi_mgr_wifi_event_cb_t *cb)
{
    callback = cb;
    if (event_during_init) cb->handler(WIFI_MGR_WIFI_EVT_STA_CONNECTED, NULL, 0, cb->arg);
    return 0;
}
static int remove_callback(wifi_mgr_wifi_event_cb_t *cb) { (void)cb; callback = NULL; return 0; }
static void emit(wifi_mgr_wifi_event_t event)
{ CHECK(!locked); callback->handler(event, NULL, 0, callback->arg); }
static int scan(wifi_mgr_scan_info_t *aps, uint32_t size, uint32_t timeout)
{
    (void)timeout; CHECK(!critical && !locked); scan_calls++;
    if (scan_hook) scan_hook();
    if (scan_result > 0 && aps && size) {
        for (int i = 0; i < scan_result && i < (int)size; i++) {
            memset(&aps[i], 0, sizeof(*aps)); strcpy(aps[i].ssid, "saved");
            snprintf(aps[i].bssid, sizeof(aps[i].bssid), "12:34:56:78:9A:%02X", (unsigned)i & 255U);
            aps[i].rssi = -20 - (scan_result - i - 1);
        }
    }
    return scan_result;
}
static int connect_ap(wifi_mgr_sta_config_t *config, uint32_t timeout)
{
    (void)config; (void)timeout; CHECK(!critical && !locked); connect_calls++;
    if (connect_calls <= 32) strcpy(attempted_bssids[connect_calls - 1], config->bssid);
    if (connect_hook) connect_hook();
    if (connect_emits_terminal && (connect_result == -EIO || connect_result == -ECONNRESET))
        emit(WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED);
    return connect_result;
}
static int disconnect_ap(uint32_t timeout) { (void)timeout; CHECK(!locked); return 0; }
static wifi_mgr_wifi_status_t get_status(void) { return WIFI_MGR_WIFI_STATUS_STA_DISCONNECTED; }
int wifi_storage_init(wifi_storage_ctx_t *ctx, wifi_storage_ops_t *ops, void *mutex)
{ ctx->ops = *ops; ctx->mutex = mutex; return 0; }
int wifi_storage_deinit(wifi_storage_ctx_t *ctx) { (void)ctx; return 0; }
int wifi_storage_save_ap(wifi_storage_ctx_t *ctx, wifi_mgr_sta_config_t *ap)
{ (void)ctx; (void)ap; return 0; }
int wifi_storage_delete_ap(wifi_storage_ctx_t *ctx, wifi_mgr_sta_config_t *ap)
{ (void)ctx; (void)ap; return 0; }
int wifi_storage_search_ap(wifi_storage_ctx_t *ctx, wifi_mgr_sta_config_t **list,
                          wifi_mgr_storage_search_mode_t mode, void *target)
{
    (void)ctx; (void)mode; (void)target;
    if (storage_error) { *list = NULL; return storage_error; }
    *list = malloc(sizeof(**list)); **list = saved_ap; return 1;
}
static void setup(void)
{
    locked = scan_calls = connect_calls = push_calls = 0; enabled = 1;
    scan_result = 1; connect_result = -EIO; head = tail = 0; now_ms = 0;
    queue_full = false;
    storage_error = 0;
    connect_emits_terminal = true;
    scan_hook = connect_hook = delay_hook = NULL; saved_ap.pmk_valid = 0;
    static wifi_manager_mem_ops_t mem = {malloc, calloc, align_alloc, malloc, free};
    static wifi_manager_os_ops_t os = {mutex_create, mutex_lock, mutex_unlock, object_delete,
        queue_create, queue_push, queue_pop, object_delete, thread_create, object_delete};
    static wifi_manager_wifi_ops_t wifi = {.init = wifi_init, .deinit = wifi_init,
        .sta_enable = sta_enable, .sta_disable = sta_disable, .sta_is_enable = sta_is_enabled,
        .add_callback = add_callback, .remove_callback = remove_callback,
        .scan_ap = scan, .sta_connect = connect_ap, .sta_disconnect = disconnect_ap,
        .sta_get_status = get_status};
    wifi_mgr_ops_t ops = {.mem_ops = &mem, .os_ops = &os, .wifi_ops = &wifi};
    CHECK(wifi_mgr_init(&ops) == 0);
}
static void teardown(void) { run_worker((int)(tail - head)); CHECK(wifi_mgr_deinit() == 0); }

/* Missing gate would admit a new radio operation after successful pause. */
static void test_pause_closes_intake_without_queue_and_preserves_recovery(void)
{
    setup(); CHECK(wifi_mgr_radio_pause(0) == 0); CHECK(wifi_mgr_radio_is_paused());
    CHECK(wifi_mgr_sta_connect(&saved_ap, false) == -EBUSY);
    CHECK(wifi_mgr_scan_ap(NULL, 0, true) == -EBUSY);
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == -EBUSY);
    CHECK(scan_calls == 0 && connect_calls == 0 && push_calls == 0);
    CHECK(wifi_mgr_sta_disconnect(false) == 0); CHECK(wifi_mgr_sta_disable() == 0);
    CHECK(wifi_mgr_sta_enable() == 0); CHECK(wifi_mgr_radio_resume() == 0);
    run_worker(1); CHECK(scan_calls == 0);
    CHECK(wifi_mgr_scan_ap(NULL, 0, false) == 1); teardown();
}
static void pause_inside_hal(void)
{
    uint64_t start = now_ms; int pushes = push_calls;
    CHECK(wifi_mgr_radio_pause(20) == -ETIMEDOUT);
    CHECK(now_ms - start == 20); CHECK(wifi_mgr_radio_is_paused());
    CHECK(wifi_mgr_scan_ap(NULL, 0, false) == -EBUSY);
    CHECK(wifi_mgr_sta_connect(&saved_ap, false) == -EBUSY);
    CHECK(push_calls == pushes);
}
/* Returning from a HAL callback must not release a still executing call. */
static void test_pause_waits_for_sync_scan_and_connect_calls(void)
{
    setup(); scan_hook = pause_inside_hal;
    CHECK(wifi_mgr_scan_ap(NULL, 0, false) == 1); CHECK(wifi_mgr_radio_pause(0) == 0);
    CHECK(wifi_mgr_radio_resume() == 0); scan_hook = NULL; connect_hook = pause_inside_hal;
    CHECK(wifi_mgr_sta_connect(&saved_ap, false) == -EIO);
    CHECK(wifi_mgr_radio_pause(0) == 0); CHECK(wifi_mgr_radio_resume() == 0);
    run_worker(2); CHECK(scan_calls == 1); teardown();
}
static void connect_done(void) { emit(WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED); }
static void scan_done(void) { emit(WIFI_MGR_WIFI_EVT_SCAN_DONE); }
/* Async return is only submission; the corresponding terminal event completes the barrier. */
static void test_pause_waits_for_async_completion_without_resuming_auto(void)
{
    setup(); connect_result = 0;
    CHECK(wifi_mgr_sta_connect(&saved_ap, true) == 0);
    delay_hook = connect_done; CHECK(wifi_mgr_radio_pause(20) == 0);
    CHECK(now_ms > 0); CHECK(wifi_mgr_radio_resume() == 0); run_worker(2);
    CHECK(scan_calls == 0);
    CHECK(wifi_mgr_scan_ap(NULL, 0, true) == 1);
    CHECK(wifi_mgr_radio_pause(0) == -ETIMEDOUT);
    delay_hook = scan_done; CHECK(wifi_mgr_radio_pause(20) == 0); teardown();
}
/* A queued START must stay cancelled even if consumed after resume. */
static void test_pause_invalidates_queued_start_across_resume(void)
{
    setup(); CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0);
    CHECK(wifi_mgr_radio_pause(0) == 0); CHECK(wifi_mgr_radio_resume() == 0);
    run_worker(2); CHECK(scan_calls == 0);
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0);
    run_worker(1); CHECK(scan_calls == 1 && connect_calls == 1); teardown();
}
/* A pause during auto scan must cancel its candidates; during PMK connect cancels fallback. */
static void test_pause_cancels_remaining_auto_cycle(void)
{
    setup(); scan_hook = pause_inside_hal;
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(1);
    CHECK(scan_calls == 1 && connect_calls == 0); CHECK(wifi_mgr_radio_pause(0) == 0);
    CHECK(wifi_mgr_radio_resume() == 0); scan_hook = NULL; connect_hook = pause_inside_hal;
    saved_ap.pmk_valid = 1;
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(1);
    CHECK(scan_calls == 2 && connect_calls == 1); CHECK(wifi_mgr_radio_pause(0) == 0);
    teardown();
}
/* HAL timeout is not a hardware cancellation: association can still finish later. */
static void test_sync_connect_timeout_stays_pending_until_event(void)
{
    setup(); connect_result = -ETIMEDOUT;
    CHECK(wifi_mgr_sta_connect(&saved_ap, false) == -ETIMEDOUT);
    CHECK(wifi_mgr_radio_pause(0) == -ETIMEDOUT);
    emit(WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED);
    CHECK(wifi_mgr_radio_pause(0) == 0); teardown();
}
static void early_scan_done(void) { scan_done(); pause_inside_hal(); }
static void early_connect_done(void) { connect_done(); pause_inside_hal(); }
/* An inline event must not let pause succeed while its HAL stack still runs. */
static void test_inline_terminal_event_keeps_hal_call_active(void)
{
    setup(); scan_hook = early_scan_done;
    CHECK(wifi_mgr_scan_ap(NULL, 0, true) == 1); CHECK(wifi_mgr_radio_pause(0) == 0);
    CHECK(wifi_mgr_radio_resume() == 0); connect_hook = early_connect_done; connect_result = 0;
    CHECK(wifi_mgr_sta_connect(&saved_ap, true) == 0); CHECK(wifi_mgr_radio_pause(0) == 0);
    teardown();
}
/* Storage can hold the shared manager mutex while pause still closes intake. */
static void test_pause_does_not_wait_for_storage_mutex(void)
{
    setup(); locked = 1;
    CHECK(wifi_mgr_radio_pause(0) == 0); CHECK(wifi_mgr_radio_is_paused());
    CHECK(wifi_mgr_sta_connect(&saved_ap, false) == -EBUSY);
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == -EBUSY);
    locked = 0; teardown();
}
static void reject_overlapping_radio_call(void)
{
    CHECK(wifi_mgr_sta_connect(&saved_ap, false) == -EBUSY);
    CHECK(wifi_mgr_scan_ap(NULL, 0, false) == -EBUSY);
}
static void test_radio_work_serializes_hal_operations(void)
{
    setup(); scan_hook = reject_overlapping_radio_call;
    CHECK(wifi_mgr_scan_ap(NULL, 0, false) == 1);
    CHECK(connect_calls == 0 && scan_calls == 1); teardown();
}
static void test_callback_registration_can_emit_event_inline(void)
{
    event_during_init = true; setup(); event_during_init = false;
    CHECK(wifi_mgr_radio_pause(0) == 0); teardown();
}
static void pause_then_resume_inside_hal(void)
{
    pause_inside_hal(); CHECK(wifi_mgr_radio_resume() == 0);
}
static void test_timeout_rollback_does_not_revive_old_auto_cycle(void)
{
    setup(); scan_hook = pause_then_resume_inside_hal;
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(2);
    CHECK(scan_calls == 1 && connect_calls == 0);
    CHECK(wifi_mgr_radio_pause(0) == 0); teardown();
}
static void test_auto_timeout_does_not_start_pmk_fallback(void)
{
    setup(); saved_ap.pmk_valid = 1; connect_result = -ETIMEDOUT;
    delay_hook = pause_inside_hal;
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(2);
    CHECK(scan_calls == 1 && connect_calls == 1);
    CHECK(wifi_mgr_radio_pause(0) == -ETIMEDOUT);
    emit(WIFI_MGR_WIFI_EVT_STA_DISCONNECTED);
    CHECK(wifi_mgr_radio_pause(0) == 0); teardown();
}
static void test_exhaustion_requires_all_candidates_and_valid_scan(void)
{
    setup(); CHECK(wifi_mgr_auto_connect_exhausted != NULL);
    CHECK(!wifi_mgr_auto_connect_exhausted());
    scan_result = 5;
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(1);
    CHECK(connect_calls == 5 && wifi_mgr_auto_connect_exhausted());
    CHECK(strcmp(attempted_bssids[0], "12:34:56:78:9A:04") == 0);
    CHECK(strcmp(attempted_bssids[1], "12:34:56:78:9A:03") == 0);
    CHECK(strcmp(attempted_bssids[2], "12:34:56:78:9A:02") == 0);
    CHECK(strcmp(attempted_bssids[3], "12:34:56:78:9A:01") == 0);
    CHECK(strcmp(attempted_bssids[4], "12:34:56:78:9A:00") == 0);
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0);
    CHECK(!wifi_mgr_auto_connect_exhausted());
    scan_result = -EIO; run_worker(1); CHECK(!wifi_mgr_auto_connect_exhausted());
    scan_result = 1; storage_error = -ENOMEM; run_worker(1);
    CHECK(!wifi_mgr_auto_connect_exhausted());
    storage_error = 0; scan_result = 0; run_worker(1);
    CHECK(wifi_mgr_auto_connect_exhausted());
    CHECK(wifi_mgr_radio_pause(0) == 0); CHECK(!wifi_mgr_auto_connect_exhausted());
    CHECK(wifi_mgr_radio_resume() == 0); CHECK(!wifi_mgr_auto_connect_exhausted()); teardown();
}
static void pending_failure(void)
{
    CHECK(connect_calls == 1 && !wifi_mgr_auto_connect_exhausted());
    connect_done(); connect_result = -EIO;
}
static void test_pending_terminal_event_finishes_before_next_ap(void)
{
    setup(); scan_result = 3; connect_result = -ETIMEDOUT; delay_hook = pending_failure;
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(1);
    CHECK(connect_calls == 3 && wifi_mgr_auto_connect_exhausted());
    emit(WIFI_MGR_WIFI_EVT_STA_CONNECTED); CHECK(!wifi_mgr_auto_connect_exhausted()); teardown();
}
static void test_stop_invalidates_queued_start_before_worker_runs(void)
{
    setup(); CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0);
    CHECK(wifi_mgr_auto_connect_stop() == 0); run_worker(1);
    CHECK(scan_calls == 0 && !wifi_mgr_auto_connect_exhausted()); teardown();
}
static void test_connect_submission_errors_never_exhaust_candidates(void)
{
    const int errors[] = {-ENOMEM, -EBUSY, -EAGAIN, -ECANCELED, -EIO};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        setup(); connect_emits_terminal = false; connect_result = errors[i];
        saved_ap.pmk_valid = 1;
        CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(1);
        CHECK(connect_calls == 1 && !wifi_mgr_auto_connect_exhausted());
        /* Local resource errors must not blacklist the saved AP. */
        saved_ap.pmk_valid = 0; run_worker(3); CHECK(connect_calls == 4);
        connect_emits_terminal = true; connect_result = -EIO; run_worker(1);
        CHECK(connect_calls == 5 && wifi_mgr_auto_connect_exhausted()); teardown();
    }
}
static void fail_pmk_fallback_allocation(void)
{
    if (connect_calls == 2) { connect_emits_terminal = false; connect_result = -ENOMEM; }
}
static void test_pmk_fallback_requires_its_own_terminal_failure(void)
{
    setup(); saved_ap.pmk_valid = 1; connect_hook = fail_pmk_fallback_allocation;
    CHECK(wifi_mgr_auto_connect_start(&autoconn) == 0); run_worker(1);
    CHECK(connect_calls == 2 && !wifi_mgr_auto_connect_exhausted()); teardown();
}
static void test_explicit_stop_cancels_manual_async_auto_resume(void)
{
    setup(); connect_result = 0;
    CHECK(wifi_mgr_sta_connect(&saved_ap, true) == 0);
    CHECK(wifi_mgr_auto_connect_stop() == 0);
    connect_done(); run_worker(4);
    CHECK(scan_calls == 0 && !wifi_mgr_auto_connect_exhausted()); teardown();
}
static void test_callback_auto_resume_never_waits_for_full_queue(void)
{
    setup(); connect_result = 0;
    CHECK(wifi_mgr_sta_connect(&saved_ap, true) == 0);
    queue_full = true; emit(WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED);
    CHECK(wifi_mgr_radio_pause(0) == 0); queue_full = false; teardown();
}
static int callback_calls;
static void pause_in_user_callback(wifi_mgr_connection_info_t *info, void *arg)
{
    (void)info; (void)arg;
    callback_calls++;
    CHECK(wifi_mgr_radio_pause(0) == -ETIMEDOUT);
}
static void test_pause_waits_for_separate_event_callback(void)
{
    setup(); connect_result = 0; callback_calls = 0;
    CHECK(wifi_mgr_sta_add_connection_cb(pause_in_user_callback, NULL) == 0);
    CHECK(wifi_mgr_sta_connect(&saved_ap, true) == 0);
    emit(WIFI_MGR_WIFI_EVT_STA_CONNECTED);
    CHECK(callback_calls == 1);
    CHECK(wifi_mgr_radio_pause(0) == 0);
    /* An event arriving after pause succeeded must not restart DHCP through
     * an application callback while the owner is disabling the radio. */
    emit(WIFI_MGR_WIFI_EVT_STA_CONNECTED);
    CHECK(callback_calls == 1);
    teardown();
}
int main(void)
{
    test_connect_submission_errors_never_exhaust_candidates();
    test_pmk_fallback_requires_its_own_terminal_failure();
    test_explicit_stop_cancels_manual_async_auto_resume();
    test_exhaustion_requires_all_candidates_and_valid_scan();
    test_pending_terminal_event_finishes_before_next_ap();
    test_stop_invalidates_queued_start_before_worker_runs();
    CHECK(wifi_mgr_radio_pause != NULL && wifi_mgr_radio_resume != NULL &&
          wifi_mgr_radio_is_paused != NULL);
    test_pause_closes_intake_without_queue_and_preserves_recovery();
    test_pause_waits_for_sync_scan_and_connect_calls();
    test_pause_waits_for_async_completion_without_resuming_auto();
    test_pause_invalidates_queued_start_across_resume();
    test_pause_cancels_remaining_auto_cycle();
    test_sync_connect_timeout_stays_pending_until_event();
    test_inline_terminal_event_keeps_hal_call_active();
    test_pause_does_not_wait_for_storage_mutex();
    test_radio_work_serializes_hal_operations();
    test_callback_registration_can_emit_event_inline();
    test_timeout_rollback_does_not_revive_old_auto_cycle();
    test_auto_timeout_does_not_start_pmk_fallback();
    test_callback_auto_resume_never_waits_for_full_queue();
    test_pause_waits_for_separate_event_callback();
    puts("PASS: 20 wifi_manager radio pause/exhaustion scenarios"); return 0;
}
