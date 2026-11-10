#include <stdbool.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

#include "lisa_kv.h"
#include "lisa_semaphore.h"
#include "lisa_thread.h"
#include "voice_msg.h"
#include "voice_cloud.h"

#include "app_datas.h"
#include "kv_user.h"
#include "sys_network_diag.h"
#include "sys_network_manager.h"
#include "sys_network_probe.h"
#include "sys_wifi.h"

#if CONFIG_LISA_MODEM
#include "lisa_modem_module.h"
#include "pinmux.h"
#endif

#define TAG "sys.net.mgr"
#include "lisa_log.h"

#define MODEM_UART_DEVICE "uart2"

#ifdef CONFIG_NETWORK_WIFI_DISCONNECT_GRACE_MS
#define WIFI_DISCONNECT_GRACE_MS ((uint32_t)CONFIG_NETWORK_WIFI_DISCONNECT_GRACE_MS)
#else
#define WIFI_DISCONNECT_GRACE_MS 2500U
#endif

typedef struct {
    bool inited;
    bool wifi_available;
    bool switching;
    bool modem_started;
    bool network_connected;
    sys_network_mode_t current_mode;
    sys_network_mode_t pending_mode;
    bool pending_persist;
    bool has_pending_request;
    sys_network_bearer_t active_bearer;
    lisa_semaphore_t *sem;
    lisa_semaphore_t *wifi_disconnect_sem;
    bool wifi_disconnect_pending;
    uint32_t wifi_disconnect_seq;
    uint32_t generation;
} sys_network_manager_ctx_t;

static sys_network_manager_ctx_t s_mgr;

static bool sys_network_mode_is_valid(sys_network_mode_t mode)
{
    return mode == SYS_NETWORK_MODE_WIFI ||
           mode == SYS_NETWORK_MODE_MODEM;
}

static void sys_network_sync_app_data(void)
{
    struct app_datas *app_datas = get_app_datas();

    if (app_datas == NULL) {
        return;
    }

    taskENTER_CRITICAL();
    app_datas->network_mode = (uint8_t)s_mgr.current_mode;
    app_datas->active_bearer = (uint8_t)s_mgr.active_bearer;
    app_datas->network_connected = s_mgr.network_connected ? 1U : 0U;
    app_datas->network_switching = s_mgr.switching ? 1U : 0U;
    app_datas->modem_connected = s_mgr.modem_started ? 1U : 0U;
    taskEXIT_CRITICAL();
}

uint32_t sys_network_generation(void)
{
    taskENTER_CRITICAL();
    uint32_t generation = s_mgr.generation;
    taskEXIT_CRITICAL();
    return generation;
}

bool sys_network_generation_is_current(uint32_t generation)
{
    return generation == sys_network_generation();
}

/* Caller holds the short state critical section. No publication or I/O here. */
static uint32_t sys_network_commit_bearer_locked(sys_network_bearer_t bearer)
{
    s_mgr.active_bearer = bearer;
    s_mgr.network_connected = false;
    return ++s_mgr.generation;
}

static void sys_network_publish_bearer(sys_network_bearer_t bearer, uint32_t msg, uint32_t generation)
{
    sys_network_sync_app_data();
    sys_network_diag_mark("bearer", "set", (int)bearer,
                          msg == 0U ? "silent" : "publish_state_event");

    if (msg != 0 && sys_network_generation_is_current(generation)) {
        /* Consumers recheck this token: a newer IP event may win between this
         * check and asynchronous bus delivery. Never publish under the lock. */
        voice_msg_pub(msg, &generation, sizeof(generation));
    }

    if (bearer != SYS_NETWORK_BEARER_NONE && sys_network_generation_is_current(generation)) {
        network_probe_start();
    }
}

static void sys_network_set_bearer(sys_network_bearer_t bearer, uint32_t msg)
{
    taskENTER_CRITICAL();
    uint32_t generation = sys_network_commit_bearer_locked(bearer);
    taskEXIT_CRITICAL();
    sys_network_publish_bearer(bearer, msg, generation);
}

static int sys_network_start_modem(void)
{
#if CONFIG_LISA_MODEM
    if (s_mgr.modem_started) {
        return 0;
    }

    LISA_LOGI(TAG, "Starting modem");
    if (!lisa_modem_module_init(MODEM_UART_DEVICE)) {
        LISA_LOGW(TAG, "modem is not ready");
        return -1;
    }

    s_mgr.modem_started = true;
    return 0;
#else
    LISA_LOGW(TAG, "modem support is disabled");
    return -1;
#endif
}

static void sys_network_stop_modem(void)
{
    if (!s_mgr.modem_started) {
        return;
    }

#if CONFIG_LISA_MODEM
    if (!lisa_modem_module_deinit()) {
        LISA_LOGW(TAG, "modem deinit reported failure");
    }
#endif

    s_mgr.modem_started = false;
    sys_network_sync_app_data();
}

static bool sys_network_start_bearer(sys_network_bearer_t bearer)
{
    if (bearer == SYS_NETWORK_BEARER_WIFI) {
        if (!s_mgr.wifi_available) {
            LISA_LOGW(TAG, "WiFi bearer is unavailable");
            return false;
        }

        LISA_LOGI(TAG, "Starting WiFi bearer");
        if (sys_wifi_start(true) != 0) {
            LISA_LOGW(TAG, "Failed to start WiFi bearer");
            return false;
        }

        /* WiFi 连接异步，已经有连接就直接发布，否则等 WIFI_IP_GOT */
        if (sys_wifi_is_connected()) {
            sys_network_set_bearer(SYS_NETWORK_BEARER_WIFI, VOICE_MSG_SYSTEM_NETWORK_CONNECTED);
        }
        return true;
    }

    if (bearer == SYS_NETWORK_BEARER_MODEM) {
        if (sys_network_start_modem() != 0) {
            return false;
        }
        sys_network_set_bearer(SYS_NETWORK_BEARER_MODEM, VOICE_MSG_SYSTEM_NETWORK_CONNECTED);
        return true;
    }

    return false;
}

static void sys_network_stop_bearer(sys_network_bearer_t bearer)
{
    if (s_mgr.active_bearer == bearer) {
        sys_network_set_bearer(SYS_NETWORK_BEARER_NONE, 0);
    }

    if (bearer == SYS_NETWORK_BEARER_WIFI) {
        sys_wifi_stop();
    } else if (bearer == SYS_NETWORK_BEARER_MODEM) {
        sys_network_stop_modem();
    }
}

static void sys_network_apply_mode(sys_network_mode_t mode, bool persist)
{
    bool success;
    bool probe_ready;

    taskENTER_CRITICAL();
    s_mgr.current_mode = mode;
    s_mgr.switching = true;
    ++s_mgr.generation;
    taskEXIT_CRITICAL();
    sys_network_sync_app_data();

    if (persist && lisa_kv_set_int(KV_KEY_NETWORK_MODE, (int)mode) != 0) {
        LISA_LOGW(TAG, "Failed to persist network mode: %d", mode);
    }

    voice_msg_pub(VOICE_MSG_SYSTEM_NETWORK_SWITCHING, NULL, 0);
    voice_cloud_disconnect();

    if (mode == SYS_NETWORK_MODE_WIFI) {
        /* 严格 WiFi：不起 modem，也不回落 */
        sys_network_stop_bearer(SYS_NETWORK_BEARER_MODEM);
        success = sys_network_start_bearer(SYS_NETWORK_BEARER_WIFI);
    } else {
        /* 严格 MODEM：不起 WiFi，也不回落 */
        sys_network_stop_bearer(SYS_NETWORK_BEARER_WIFI);
#if CONFIG_LISA_MODEM
        lisa_uart2_pinmux();
#endif
        success = sys_network_start_bearer(SYS_NETWORK_BEARER_MODEM);
        if (!success) {
            LISA_LOGW(TAG, "modem unavailable");
        }
    }

    taskENTER_CRITICAL();
    s_mgr.switching = false;
    probe_ready = success &&
                  ((s_mgr.active_bearer == SYS_NETWORK_BEARER_WIFI && sys_wifi_is_connected()) ||
                   (s_mgr.active_bearer == SYS_NETWORK_BEARER_MODEM && s_mgr.modem_started));
    taskEXIT_CRITICAL();
    sys_network_sync_app_data();
    voice_msg_pub(success ? VOICE_MSG_SYSTEM_NETWORK_SWITCH_DONE
                          : VOICE_MSG_SYSTEM_NETWORK_SWITCH_FAIL,
                  NULL, 0);
    /* A ready bearer may complete its first probe before switching clears.
     * Such a result is deliberately rejected; retry outside the state lock. */
    if (probe_ready) {
        network_probe_start();
    }
}

static void sys_network_worker(void *arg)
{
    (void)arg;

    while (1) {
        sys_network_mode_t mode;
        bool persist;

        lisa_semaphore_take(s_mgr.sem, LISA_OS_WAIT_FOREVER);

        if (!s_mgr.has_pending_request) {
            continue;
        }

        mode = s_mgr.pending_mode;
        persist = s_mgr.pending_persist;
        s_mgr.has_pending_request = false;

        sys_network_apply_mode(mode, persist);
    }
}

static void sys_network_wifi_disconnect_worker(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t pending_seq;

        lisa_semaphore_take(s_mgr.wifi_disconnect_sem, LISA_OS_WAIT_FOREVER);
        taskENTER_CRITICAL();
        bool pending = s_mgr.wifi_disconnect_pending;
        pending_seq = s_mgr.wifi_disconnect_seq;
        taskEXIT_CRITICAL();
        if (!pending) {
            continue;
        }

        if (WIFI_DISCONNECT_GRACE_MS != 0U) {
            lisa_thread_mdelay(WIFI_DISCONNECT_GRACE_MS);
        }

        /* DHCP/IP callbacks and the grace worker commit against one state
         * lock. Checking the sequence before this section is insufficient:
         * a recovered IP could otherwise be overwritten by stale NONE. */
        taskENTER_CRITICAL();
        if (!s_mgr.wifi_disconnect_pending || pending_seq != s_mgr.wifi_disconnect_seq) {
            taskEXIT_CRITICAL();
            sys_network_diag_mark("bearer", "wifi_disconnect_grace_cancelled",
                                  (int)pending_seq, "newer_link_event");
            continue;
        }

        s_mgr.wifi_disconnect_pending = false;
        if (s_mgr.active_bearer != SYS_NETWORK_BEARER_WIFI) {
            sys_network_bearer_t bearer = s_mgr.active_bearer;
            taskEXIT_CRITICAL();
            sys_network_diag_mark("bearer", "wifi_disconnect_grace_ignored",
                                  (int)bearer, "wifi_not_active");
            continue;
        }

        if (sys_wifi_is_connected()) {
            taskEXIT_CRITICAL();
            sys_network_diag_mark("bearer", "wifi_disconnect_grace_recovered",
                                  (int)pending_seq, "link_restored");
            continue;
        }

        uint32_t msg = s_mgr.switching || sys_wifi_radio_suspended() ?
                           0U : VOICE_MSG_SYSTEM_NETWORK_DISCONNECTED;
        uint32_t generation = sys_network_commit_bearer_locked(SYS_NETWORK_BEARER_NONE);
        taskEXIT_CRITICAL();
        sys_network_diag_mark("bearer", "wifi_disconnect_confirmed",
                              (int)pending_seq, "grace_expired");
        sys_network_publish_bearer(SYS_NETWORK_BEARER_NONE, msg, generation);
    }
}

static void sys_network_wifi_ip_got(void *unused, uint32_t evt, void *data, uint32_t len, void *user_data)
{
    (void)unused; (void)evt; (void)data; (void)len; (void)user_data;

    taskENTER_CRITICAL();
    if (!sys_wifi_is_connected()) {
        taskEXIT_CRITICAL();
        return; /* A queued pre-standby event cannot resurrect the bearer. */
    }

    /* modem 已接管就忽略 WiFi 拿到 IP 的事件 */
    if (s_mgr.modem_started) {
        taskEXIT_CRITICAL();
        sys_network_diag_mark("bearer", "wifi_ip_ignored", 0, "modem_active");
        return;
    }

    s_mgr.wifi_disconnect_pending = false;
    uint32_t sequence = ++s_mgr.wifi_disconnect_seq;
    uint32_t generation = sys_network_commit_bearer_locked(SYS_NETWORK_BEARER_WIFI);
    taskEXIT_CRITICAL();
    sys_network_diag_mark("bearer", "wifi_ip_got", (int)sequence,
                          "select_wifi_cancel_disconnect_grace");
    sys_network_publish_bearer(SYS_NETWORK_BEARER_WIFI, VOICE_MSG_SYSTEM_NETWORK_CONNECTED, generation);
}

static void sys_network_wifi_disconnected(void *unused, uint32_t evt, void *data, uint32_t len, void *user_data)
{
    (void)unused; (void)evt; (void)data; (void)len; (void)user_data;

    taskENTER_CRITICAL();
    if (s_mgr.active_bearer != SYS_NETWORK_BEARER_WIFI || sys_wifi_is_connected()) {
        sys_network_bearer_t bearer = s_mgr.active_bearer;
        taskEXIT_CRITICAL();
        sys_network_diag_mark("bearer", "wifi_disconnect_ignored", (int)bearer,
                              "wifi_not_active");
        return;
    }

    if (s_mgr.switching || sys_wifi_radio_suspended() || WIFI_DISCONNECT_GRACE_MS == 0U) {
        s_mgr.wifi_disconnect_pending = false;
        s_mgr.wifi_disconnect_seq++;
        bool switching = s_mgr.switching;
        uint32_t msg = switching || sys_wifi_radio_suspended() ? 0U : VOICE_MSG_SYSTEM_NETWORK_DISCONNECTED;
        uint32_t generation = sys_network_commit_bearer_locked(SYS_NETWORK_BEARER_NONE);
        taskEXIT_CRITICAL();
        sys_network_diag_mark("bearer", "wifi_disconnected", switching ? 1 : 0,
                              "drop_active_bearer_immediately");
        sys_network_publish_bearer(SYS_NETWORK_BEARER_NONE, msg, generation);
        return;
    }

    /* Stop new cloud work immediately, but keep the selected bearer during a
     * short reassociation window. A quick DHCP recovery cancels the pending
     * disconnect event and avoids contradictory spoken prompts. */
    s_mgr.network_connected = false;
    if (s_mgr.wifi_disconnect_pending) {
        /* Duplicate scan/failure notifications belong to the same outage.
         * Do not move the deadline and indefinitely suppress its reminder. */
        taskEXIT_CRITICAL();
        sys_network_sync_app_data();
        return;
    }
    s_mgr.wifi_disconnect_pending = true;
    uint32_t sequence = ++s_mgr.wifi_disconnect_seq;
    ++s_mgr.generation; /* Invalidate a probe already running on the lost link. */
    taskEXIT_CRITICAL();
    sys_network_sync_app_data();
    sys_network_diag_mark("bearer", "wifi_disconnect_grace_started",
                          (int)sequence, "wait_for_reassociation");
    lisa_semaphore_give(s_mgr.wifi_disconnect_sem);
}

int sys_network_manager_init(bool wifi_available)
{
    lisa_thread_attr_t attr = {
        .name = "sys.net.mgr",
        .stack_size = 3072,
        .priority = LISA_OS_PRIORITY_NORMAL,
    };

    if (s_mgr.inited) {
        s_mgr.wifi_available = wifi_available;
        return 0;
    }

    memset(&s_mgr, 0, sizeof(s_mgr));
    s_mgr.wifi_available = wifi_available;
    s_mgr.current_mode = SYS_NETWORK_MODE_WIFI;

    struct app_datas *app_datas = get_app_datas();
    if (app_datas != NULL && sys_network_mode_is_valid((sys_network_mode_t)app_datas->network_mode)) {
        s_mgr.current_mode = (sys_network_mode_t)app_datas->network_mode;
    }

    s_mgr.sem = lisa_semaphore_create(1);
    if (s_mgr.sem == NULL) {
        return -1;
    }

    s_mgr.wifi_disconnect_sem = lisa_semaphore_create(1);
    if (s_mgr.wifi_disconnect_sem == NULL) {
        return -1;
    }

    if (lisa_thread_create(&attr, sys_network_worker, NULL) == NULL) {
        return -1;
    }

    attr.name = "sys.net.drop";
    if (lisa_thread_create(&attr, sys_network_wifi_disconnect_worker, NULL) == NULL) {
        return -1;
    }

    voice_msg_sub(VOICE_MSG_WIFI_IP_GOT, sys_network_wifi_ip_got, NULL);
    voice_msg_sub(VOICE_MSG_WIFI_DISCONNECTED, sys_network_wifi_disconnected, NULL);

    s_mgr.inited = true;
    sys_network_sync_app_data();
    return sys_network_request_mode(s_mgr.current_mode, false);
}

int sys_network_request_mode(sys_network_mode_t mode, bool persist)
{
    if (!s_mgr.inited || !sys_network_mode_is_valid(mode)) {
        return -1;
    }

    s_mgr.pending_mode = mode;
    s_mgr.pending_persist = persist;
    s_mgr.has_pending_request = true;
    lisa_semaphore_give(s_mgr.sem);
    return 0;
}

int sys_network_connect_wifi(const char *ssid, const char *pwd, const char *bssid)
{
    if (!s_mgr.inited || !s_mgr.wifi_available) {
        return -1;
    }

    /* 当前跑在 modem 上：只存凭据不切 bearer，等用户显式切回 WiFi */
    if (s_mgr.active_bearer == SYS_NETWORK_BEARER_MODEM) {
        LISA_LOGI(TAG, "modem bearer is active, save WiFi credentials only");
        return sys_wifi_save_ap(ssid, pwd, bssid);
    }

    if (!sys_wifi_is_started() && sys_wifi_start(false) != 0) {
        LISA_LOGW(TAG, "Failed to start WiFi before connect");
        return -1;
    }

    return sys_wifi_connect(ssid, pwd, bssid);
}

int sys_network_toggle_mode(bool persist)
{
    if (!s_mgr.inited) {
        return -1;
    }

    sys_network_mode_t next = (s_mgr.current_mode == SYS_NETWORK_MODE_WIFI)
                                  ? SYS_NETWORK_MODE_MODEM
                                  : SYS_NETWORK_MODE_WIFI;
    return sys_network_request_mode(next, persist);
}

bool sys_network_report_probe_result_for_generation(bool connected, uint32_t generation)
{
    taskENTER_CRITICAL();
    if (!s_mgr.inited || generation != s_mgr.generation || s_mgr.switching ||
        s_mgr.active_bearer == SYS_NETWORK_BEARER_NONE ||
        (s_mgr.active_bearer == SYS_NETWORK_BEARER_WIFI && !sys_wifi_is_connected())) {
        taskEXIT_CRITICAL();
        sys_network_diag_mark("probe", "result_ignored", connected ? 1 : 0, "stale_or_no_bearer");
        return false;
    }

    s_mgr.network_connected = connected;
    taskEXIT_CRITICAL();
    sys_network_sync_app_data();
    sys_network_diag_mark("probe", connected ? "internet_ready" : "internet_unavailable",
                          connected ? 1 : 0, NULL);
    return true;
}

void sys_network_report_probe_result(bool connected)
{
    (void)sys_network_report_probe_result_for_generation(connected, sys_network_generation());
}

void sys_network_report_transport_failure(void)
{
    if (!s_mgr.inited || s_mgr.switching || s_mgr.active_bearer == SYS_NETWORK_BEARER_NONE) {
        return;
    }

    /* The SDK also reports start_task/TTS/end_audio timeouts as websocket
     * connection failure while the socket and Wi-Fi are still connected.
     * Let the reachability worker decide; otherwise every timed-out turn
     * announces offline followed immediately by SNTP's online result. */
    sys_network_diag_mark("transport", "verify_reachability", s_mgr.network_connected ? 1 : 0,
                          "preserve_state_until_probe_result");
    (void)network_probe_refresh();
}

int sys_network_get_status(sys_network_status_t *status)
{
    if (status == NULL) {
        return -1;
    }

    taskENTER_CRITICAL();
    status->mode = s_mgr.current_mode;
    status->active_bearer = s_mgr.active_bearer;
    status->connected = s_mgr.network_connected;
    status->switching = s_mgr.switching;
    status->wifi_available = s_mgr.wifi_available;
    status->wifi_connected = s_mgr.wifi_available && sys_wifi_is_connected();
    status->modem_connected = s_mgr.modem_started;
    taskEXIT_CRITICAL();
    return 0;
}

bool sys_network_get_signal_quality(int *rssi, int *ber)
{
    switch (s_mgr.active_bearer) {
    case SYS_NETWORK_BEARER_WIFI:
        if (ber != NULL) {
            *ber = 99;
        }
        return sys_wifi_get_signal_quality(rssi);

    case SYS_NETWORK_BEARER_MODEM:
#if CONFIG_LISA_MODEM
        return lisa_modem_get_signal_quality(rssi, ber);
#else
        return false;
#endif

    case SYS_NETWORK_BEARER_NONE:
    default:
        return false;
    }
}

