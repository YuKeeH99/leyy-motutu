#define TAG "netcfg-portal"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "lwip/def.h"
#include "lwip/apps/dhcps.h"

/* mrpc_wifi_api_client.h 不自带依赖，ls_err_t 和 wifi 类型必须排在它前面。 */
#include "ls_err.h"
#include "ls_wifi_type.h"
#include "ls_event.h"
#include "ipc/mrpc_wifi_api_client.h"
#include "net_al.h"
#include "net_ip.h"
#include "wifi_manager/wifi_manager.h"

#include "app_netcfg_portal.h"
#include "app_ble_common.h"
#include "netcfg_portal_priv.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_v2_rgb_effect.h"
#include "service_v2_voice_standby.h"
#include "service_offline_action.h"
#else
#include "service_led.h"
#endif
#include "sys_network_manager.h"
#include "sys_network_diag.h"
#include "sys_wifi.h"
#include "tone.h"
#include "app_tone.h"
#include "voice_player_comm.h"

#include "lisa_log.h"

/* 每次提交凭据的 STA 连接尝试有界；配网门户本身不做空闲超时。 */
#define PORTAL_CONNECT_TIMEOUT_MS 30000
#define PORTAL_CONNECT_ATTEMPTS   3
#define PORTAL_CONNECT_RETRY_MS   1000
/*
 * 连接失败时 wpa_supplicant 会在调用者上下文里用 sscanf 解析
 * "CTRL-EVENT-EAP-FAILURE2 <mac>" 之类的事件串，newlib 的 __ssvfscanf_r
 * 单个栈帧就要 1312 字节，加上 sscanf 自身的 160 字节，一次解析吃掉近 1.5KB。
 * 原来的 4096 字节栈在第三次重连时被这一下捅穿（sp 落到栈底以下 116 字节），
 * 触发 24(stack overflow) 异常，整机死机。
 *
 * 实测：三次关联失败(error:208)走完，该任务峰值占用 6234 字节。
 * 8192 只剩 1960 字节余量，不够再兜一次 sscanf 尖峰，故给到 12288（峰值约 51%）。
 */
#define PORTAL_TASK_STACK         12288
#define PORTAL_TASK_PRIO          5
typedef struct {
    bool inited;
    volatile bool active;
    volatile bool stop_requested;
    bool ap_up;
    bool dhcps_up;
    bool client_events_ready;
    bool ap_clients_known;
    uint32_t ap_clients;
    bool credential_active;
    bool transaction_busy;
    bool offline_takeover;
    bool offline_closing;

    char ap_ssid[NETCFG_PORTAL_SSID_MAX + 1];

    netcfg_portal_ap_t aps[NETCFG_PORTAL_SCAN_MAX];
    int ap_count;
    bool ap_cache_valid;
    bool scan_busy;

    /* 手机提交的凭据，只在内存里存活到连接结束。 */
    char req_ssid[NETCFG_PORTAL_SSID_MAX + 1];
    char req_pwd[NETCFG_PORTAL_PWD_MAX + 1];

    SemaphoreHandle_t lock;
    SemaphoreHandle_t submitted;
    TaskHandle_t task;
    app_netcfg_portal_reason_t reason;
    uintptr_t start_caller;
} portal_ctx_t;

static portal_ctx_t s_ctx;

/* 普通 memset 可能被优化器删除；凭据清理必须产生真实写操作。 */
static void secure_zero(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;

    while (len-- > 0) {
        *p++ = 0;
    }
}

static void lock(void)
{
    if (s_ctx.lock != NULL) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_ctx.lock != NULL) {
        xSemaphoreGive(s_ctx.lock);
    }
}

static void portal_mark_inactive(void)
{
    lock();
    secure_zero(s_ctx.req_ssid, sizeof(s_ctx.req_ssid));
    secure_zero(s_ctx.req_pwd, sizeof(s_ctx.req_pwd));
    s_ctx.active = false;
    s_ctx.stop_requested = false;
    s_ctx.task = NULL;
    s_ctx.credential_active = false;
    s_ctx.transaction_busy = false;
    s_ctx.offline_takeover = false;
    s_ctx.offline_closing = false;
    unlock();
    sys_network_diag_mark("portal", "inactive", 0, "worker_finished");
}

/* Register before starting any portal AP. Track associations, not HTTP requests:
 * a phone obtaining DHCP must also prevent voice from tearing down its AP. */
static int portal_wifi_event(void *arg, event_module_t module, int event_id, void *data)
{
    (void)arg;
    (void)module;
    lock();
    if (event_id == EVENT_WIFI_AP_STARTED) {
        s_ctx.ap_clients = 0U;
        s_ctx.ap_clients_known = true;
    } else if (event_id == EVENT_WIFI_AP_STOPPED) {
        s_ctx.ap_clients = 0U;
        s_ctx.ap_clients_known = false;
    } else if (event_id == EVENT_WIFI_AP_STA_ADD || event_id == EVENT_WIFI_AP_STA_DEL) {
        unsigned idx = data == NULL ? 32U :
            (event_id == EVENT_WIFI_AP_STA_ADD ?
             ((event_ap_sta_add_param_t *)data)->sta_idx :
             ((event_ap_sta_del_param_t *)data)->sta_idx);
        if (idx >= 32U) {
            s_ctx.ap_clients_known = false; /* Unknown client state fails closed. */
        } else if (event_id == EVENT_WIFI_AP_STA_ADD) {
            s_ctx.ap_clients |= (uint32_t)1U << idx;
        } else {
            s_ctx.ap_clients &= ~((uint32_t)1U << idx);
        }
        if (s_ctx.offline_takeover && !s_ctx.offline_closing &&
            (!s_ctx.ap_clients_known || s_ctx.ap_clients != 0U)) {
            s_ctx.stop_requested = false;
            s_ctx.offline_takeover = false;
        }
    }
    unlock();
    return 0;
}

/* Association and an open page do not own the action engine. Only short
 * credential submission and VIF transitions block normal motion. */
static bool portal_offline_blocked_locked(void)
{
    return s_ctx.active &&
        (s_ctx.transaction_busy || s_ctx.stop_requested || (!s_ctx.ap_up && !s_ctx.credential_active));
}

bool app_netcfg_portal_offline_blocked(void)
{
    lock();
    bool blocked = portal_offline_blocked_locked();
    unlock();
    return blocked;
}

int app_netcfg_portal_offline_takeover(void)
{
    /* Kept for callers built against the previous interface. No AP teardown. */
    return app_netcfg_portal_offline_blocked() ? -EBUSY : 0;
}

static void portal_retry_trace(const char *reason, int result)
{
    static const char *last_reason;
    static unsigned last_tick;
    unsigned now = (unsigned)xTaskGetTickCount();
    if (reason == last_reason && (unsigned)(now - last_tick) < pdMS_TO_TICKS(10000U)) return;
    last_reason = reason;
    last_tick = now;
    sys_network_diag_mark("portal", "saved_retry", result, reason);
}

static int portal_retry_saved(bool explicit_request, bool wake_request)
{
    lock();
    if (!s_ctx.active) {
        unlock();
        return 0;
    }
    if (!explicit_request && s_ctx.reason == APP_NETCFG_PORTAL_REASON_USER_REQUEST) {
        portal_retry_trace("manual_portal", -EBUSY);
        unlock();
        return -EBUSY;
    }
    if (s_ctx.stop_requested) {
        portal_retry_trace("closing", -EINPROGRESS);
        unlock();
        return -EINPROGRESS;
    }
    if (s_ctx.credential_active || s_ctx.transaction_busy || s_ctx.scan_busy || !s_ctx.ap_up) {
        portal_retry_trace(s_ctx.credential_active ? "credentials" : "radio_transaction", -EBUSY);
        unlock();
        return -EBUSY;
    }
    if (!explicit_request && !wake_request && (!s_ctx.ap_clients_known || s_ctx.ap_clients != 0U)) {
        portal_retry_trace(s_ctx.ap_clients_known ? "clients_attached" : "clients_unknown", -EBUSY);
        unlock();
        return -EBUSY;
    }
    if (s_ctx.active) {
        /* A wake may reclaim automatic setup; explicit online may also reclaim
         * manual setup. Credentials and radio work always retain ownership. */
        s_ctx.offline_takeover = false;
        s_ctx.stop_requested = true; /* Claim before a concurrent credential submission. */
        portal_retry_trace(explicit_request ? "explicit_online" : wake_request ? "user_wakeup" : "idle_recovery",
                           -EINPROGRESS);
    }
    unlock();
    xSemaphoreGive(s_ctx.submitted);
    return -EINPROGRESS; /* Portal worker owns teardown; never wait in voice worker. */
}

int app_netcfg_portal_retry_saved(void)
{
    return portal_retry_saved(true, false);
}

int app_netcfg_portal_retry_saved_on_wake(void)
{
    return portal_retry_saved(false, true);
}

int app_netcfg_portal_retry_saved_background(void)
{
    return portal_retry_saved(false, false);
}

static void play_tone(int tone_id)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_network_prompt_allowed() || service_v2_voice_standby_background_audio_suppressed()) {
        return;
    }
#endif
    const char *url = app_tone_get_url(tone_id);

    if (url != NULL) {
        voice_player_play_tone_url(url);
    }
}

static void led_provisioning(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_v2_rgb_effect_set_provisioning(
        SERVICE_V2_RGB_PROVISIONING_ACTIVE);
#else
    service_led_blink(300, 700);
#endif
}

static void led_success(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_v2_rgb_effect_set_provisioning(
        SERVICE_V2_RGB_PROVISIONING_SUCCESS);
#else
    service_led_on();
#endif
}

static void led_off(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_v2_rgb_effect_set_provisioning(
        SERVICE_V2_RGB_PROVISIONING_INACTIVE);
#else
    service_led_off();
#endif
}

/**
 * 生成 ARCS-XXXXXX 形式的热点名。
 *
 * 后缀取 AP MAC 后三字节，同一批板子各不相同，客户在 WiFi 列表里一眼能认出，
 * 也不需要贴纸或语音播报来告诉他名字。
 */
static void build_ssid(void)
{
    uint8_t mac[6] = {0};

    if (wifi_get_ap_mac(mac) != LS_OK) {
        LISA_LOGW(TAG, "read AP MAC failed, fall back to a fixed suffix");
        snprintf(s_ctx.ap_ssid, sizeof(s_ctx.ap_ssid), "MHZW-SETUP");
        return;
    }

    snprintf(s_ctx.ap_ssid, sizeof(s_ctx.ap_ssid), "MHZW-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static bool encryption_is_open(wifi_mgr_wifi_encryption_mode_t mode)
{
    return mode == WIFI_MGR_WIFI_AUTH_OPEN;
}

/**
 * 把 vif 切回 STA 并等它真的切过去。
 *
 * wifi_mgr_sta_is_enable() 读的是 wifi_get_mode()，上一轮配网结束后这个模式还
 * 停在 AP，所以进配网、连接、退出三处都得先过这一道。切换是异步的，固定 sleep
 * 要么不够要么白等，这里轮询到位就返回。
 */
static bool sta_up(void)
{
    if (wifi_mgr_sta_is_enable()) {
        return true;
    }

    LISA_LOGI(TAG, "switching vif back to STA");
    wifi_mgr_sta_enable();

    for (int waited = 0; waited < 3000; waited += 100) {
        if (wifi_mgr_sta_is_enable()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    LISA_LOGW(TAG, "STA mode did not come up in 3s");
    return false;
}

/**
 * 在 STA 模式下扫一次并缓存。
 *
 * 必须发生在起 AP 之前：VIF_MAX == 1，AP 起来之后同一个 vif 上扫不到东西。
 */
static int scan_and_cache(bool prepare_sta)
{
    static wifi_mgr_scan_info_t found[NETCFG_PORTAL_SCAN_MAX];
    int n;

    if (prepare_sta && !sta_up()) {
        return -EAGAIN;
    }

    memset(found, 0, sizeof(found));

    /*
     * 刚切回 STA 时底层 vif 未必立刻能扫（wifi_scan_start 会直接返回 -1），
     * 重试几次比一次性放弃靠谱，代价只是最多多等 2 秒。
     */
    for (int attempt = 0; attempt < (prepare_sta ? 4 : 1); attempt++) {
        n = wifi_mgr_scan_ap(found, NETCFG_PORTAL_SCAN_MAX, false);
        if (n > 0 || (!prepare_sta && n == 0)) {
            break;
        }
        LISA_LOGW(TAG, "scan attempt %d returned %d, retrying", attempt + 1, n);
        if (prepare_sta) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    if (n < 0) {
        return n; /* Retain the last successful cache on a driver failure. */
    }

    lock();
    s_ctx.ap_count = 0;

    for (int i = 0; i < n && s_ctx.ap_count < NETCFG_PORTAL_SCAN_MAX; i++) {
        if (found[i].ssid[0] == '\0' || found[i].channel < 1 || found[i].channel > 14) {
            continue;
        }

        /* 同名多 AP 只留信号最强的那个，列表里出现重复项很难选。 */
        bool dup = false;

        for (int j = 0; j < s_ctx.ap_count; j++) {
            if (strncmp(s_ctx.aps[j].ssid, found[i].ssid, NETCFG_PORTAL_SSID_MAX) == 0) {
                if (found[i].rssi > s_ctx.aps[j].rssi) {
                    s_ctx.aps[j].rssi = found[i].rssi;
                    s_ctx.aps[j].secured = !encryption_is_open(found[i].encryption_mode);
                    strncpy(s_ctx.aps[j].bssid, found[i].bssid, sizeof(s_ctx.aps[j].bssid) - 1);
                    s_ctx.aps[j].bssid[sizeof(s_ctx.aps[j].bssid) - 1] = '\0';
                }
                dup = true;
                break;
            }
        }

        if (dup) {
            continue;
        }

        netcfg_portal_ap_t *slot = &s_ctx.aps[s_ctx.ap_count++];

        memcpy(slot->ssid, found[i].ssid, NETCFG_PORTAL_SSID_MAX);
        slot->ssid[NETCFG_PORTAL_SSID_MAX] = '\0';
        strncpy(slot->bssid, found[i].bssid, sizeof(slot->bssid) - 1);
        slot->bssid[sizeof(slot->bssid) - 1] = '\0';
        slot->rssi = found[i].rssi;
        slot->secured = !encryption_is_open(found[i].encryption_mode);
    }

    /* 按信号强度降序，用户最可能选的排最前。 */
    for (int i = 1; i < s_ctx.ap_count; i++) {
        netcfg_portal_ap_t key = s_ctx.aps[i];
        int j = i - 1;

        while (j >= 0 && s_ctx.aps[j].rssi < key.rssi) {
            s_ctx.aps[j + 1] = s_ctx.aps[j];
            j--;
        }
        s_ctx.aps[j + 1] = key;
    }

    int cached = s_ctx.ap_count;
    s_ctx.ap_cache_valid = true;

    unlock();

    LISA_LOGI(TAG, "scan cached %d AP(s)", cached);
    return 0;
}

int app_netcfg_portal_wifi_list(bool refresh, app_netcfg_wifi_ap_t *out, int max, int *count)
{
    int status = APP_NETCFG_WIFI_CACHED;
    bool scan = false;
    if (out == NULL || count == NULL || max <= 0) {
        return APP_NETCFG_WIFI_FAILED;
    }
    *count = 0;
    lock();
    if (!s_ctx.inited) {
        status = APP_NETCFG_WIFI_FAILED;
    } else if (sys_wifi_radio_suspended()) {
        status = APP_NETCFG_WIFI_PAUSED;
    } else if (s_ctx.active) {
        status = s_ctx.ap_cache_valid ? APP_NETCFG_WIFI_CACHED : APP_NETCFG_WIFI_BUSY;
    } else if (refresh) {
        /* sys_wifi owns background reconnect/scanning while started. Never
         * interrupt an association or normal connected audio traffic. */
        if (s_ctx.scan_busy || sys_wifi_is_started() || !wifi_mgr_sta_is_enable()) {
            status = APP_NETCFG_WIFI_BUSY;
        } else {
            s_ctx.scan_busy = true;
            scan = true;
        }
    } else if (!s_ctx.ap_cache_valid) {
        status = APP_NETCFG_WIFI_BUSY;
    }
    unlock();
    if (scan) {
        status = scan_and_cache(false) == 0 ? APP_NETCFG_WIFI_FRESH : APP_NETCFG_WIFI_FAILED;
    }
    lock();
    if (scan) {
        s_ctx.scan_busy = false;
    }
    *count = s_ctx.ap_count < max ? s_ctx.ap_count : max;
    for (int i = 0; i < *count; i++) {
        memcpy(out[i].ssid, s_ctx.aps[i].ssid, sizeof(out[i].ssid));
        out[i].rssi = s_ctx.aps[i].rssi;
        out[i].secured = s_ctx.aps[i].secured;
    }
    unlock();
    return status;
}

static int softap_up(void)
{
    wifi_ap_cfg_params_t cfg = {0};
    ip_addr_t dns_server;
    net_if_t *nif;

    /* 开放热点：客户不需要被告知密码，页面本身也不做认证之外的事。 */
    strncpy((char *)cfg.ssid, s_ctx.ap_ssid, sizeof(cfg.ssid) - 1);
    cfg.pwd[0] = '\0';
    cfg.sec = WIFI_SEC_OPEN;
    cfg.channel = 6;
    /* 不使用固件对 0 的隐式默认值，保证手机能留在热点上完成配网。 */
    /* Provisioning is explicitly user-controlled.  Do not evict the phone
     * before it has supplied usable credentials. */
    cfg.ap_max_inactivity = 0;

    if (wifi_ap_start(&cfg) != LS_OK) {
        LISA_LOGE(TAG, "wifi_ap_start failed");
        return -EIO;
    }
    lock();
    s_ctx.ap_up = true;
    unlock();

    nif = net_if_get(WIFI_VIF_AP_IDX);
    if (nif == NULL) {
        LISA_LOGE(TAG, "no netif for AP vif");
        return -ENODEV;
    }

    /*
     * 这里不要自己 net_if_set_ip()：紧接着的 dhcps_start() 会用 GW_ADDR*
     * 覆盖掉整组地址（见 netcfg_portal_priv.h 的说明），设了也是白设，
     * 还会让人误以为门户在自己配的那个网段上。
     */
    net_if_up(nif);
    net_if_set_default(nif);

    /*
     * 手机依赖 DHCP option 6 解析联网探测域名。SDK 默认虽然
     * 会发送该选项，值却是 0.0.0.0；必须明确指向门户 DNS。
     */
    IP4_ADDR(&dns_server, NETCFG_PORTAL_IP_A, NETCFG_PORTAL_IP_B, NETCFG_PORTAL_IP_C, NETCFG_PORTAL_IP_D);
    dhcps_set_dns_server(&dns_server);

    if (ls_dhcps_start(WIFI_VIF_AP_IDX) != LS_OK) {
        LISA_LOGE(TAG, "dhcp server start failed");
        dhcps_set_dns_server(NULL);
        return -EIO;
    }
    s_ctx.dhcps_up = true;

    LISA_LOGI(TAG, "SoftAP \"%s\" up at " NETCFG_PORTAL_IP_STR, s_ctx.ap_ssid);
    return 0;
}

static void softap_down(void)
{
    if (s_ctx.dhcps_up) {
        ls_dhcps_stop();
        s_ctx.dhcps_up = false;
    }
    dhcps_set_dns_server(NULL);

    if (s_ctx.ap_up) {
        wifi_ap_stop();
        lock();
        s_ctx.ap_up = false;
        unlock();
    }
}

int netcfg_portal_aps_snapshot(netcfg_portal_ap_t *out, int max)
{
    int n;

    if (out == NULL || max <= 0) {
        return 0;
    }

    lock();
    n = s_ctx.ap_count < max ? s_ctx.ap_count : max;
    if (n > 0) {
        memcpy(out, s_ctx.aps, (size_t)n * sizeof(*out));
    }
    unlock();

    return n;
}

void netcfg_portal_ssid_get(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    lock();
    strncpy(buf, s_ctx.ap_ssid, len - 1);
    buf[len - 1] = '\0';
    unlock();
}

bool netcfg_portal_submit(const char *ssid, const char *pwd)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    if (strlen(ssid) > NETCFG_PORTAL_SSID_MAX || (pwd != NULL && strlen(pwd) > NETCFG_PORTAL_PWD_MAX)) {
        return false;
    }

    lock();
    if (!s_ctx.active || s_ctx.stop_requested || s_ctx.credential_active) {
        unlock();
        return false;
    }
    s_ctx.credential_active = true;
    s_ctx.transaction_busy = true;
    strncpy(s_ctx.req_ssid, ssid, sizeof(s_ctx.req_ssid) - 1);
    s_ctx.req_ssid[sizeof(s_ctx.req_ssid) - 1] = '\0';
    if (pwd != NULL) {
        strncpy(s_ctx.req_pwd, pwd, sizeof(s_ctx.req_pwd) - 1);
        s_ctx.req_pwd[sizeof(s_ctx.req_pwd) - 1] = '\0';
    } else {
        s_ctx.req_pwd[0] = '\0';
    }
    unlock();

    xSemaphoreGive(s_ctx.submitted);
    return true;
}

/**
 * 用手机给的凭据去连。
 *
 * 到这一步 AP 已经关了，手机必然掉线，所以结果只能靠提示音和灯反馈。
 * 用户主动进入配网时保留旧凭据；连接成功后保存或更新本轮热点。
 */
static bool try_connect(void)
{
    char ssid[NETCFG_PORTAL_SSID_MAX + 1];
    char pwd[NETCFG_PORTAL_PWD_MAX + 1];
    bool ok = false;

    lock();
    memcpy(ssid, s_ctx.req_ssid, sizeof(ssid));
    memcpy(pwd, s_ctx.req_pwd, sizeof(pwd));
    unlock();

    softap_down();
    netcfg_portal_http_stop();

    if (!sta_up()) {
        LISA_LOGE(TAG, "cannot switch back to STA, connect would fail");
        goto out;
    }

    /* Register/start the station only after the single VIF has actually left
     * AP mode. Starting while still in AP mode registers the wrong netdev and
     * makes the following connect fail intermittently. */
    if (sys_wifi_start(false) != 0) {
        LISA_LOGE(TAG, "restart STA failed");
        goto out;
    }
    /* Portal credentials own the station until all bounded retries finish.
     * Keep a previously saved AP from racing the AP being provisioned. */
    sys_wifi_reconnect_saved_once_stop();

    /*
     * 混合 WPA/WPA2 AP 偶尔会在第一次四次握手超时；开机自动重试
     * 能连上，门户也应该在恢复热点前给同一份凭据几次机会。
     */
    for (int attempt = 1; attempt <= PORTAL_CONNECT_ATTEMPTS && !ok && !s_ctx.stop_requested; attempt++) {
        LISA_LOGI(TAG, "connecting to \"%s\" (attempt %d/%d)", ssid, attempt, PORTAL_CONNECT_ATTEMPTS);

        /* 复用既有网络管理入口，凭据保存和云端联动都在里面，不另开一套。 */
        sys_wifi_reset_connect_result();
        lock();
        s_ctx.transaction_busy = true;
        unlock();
        /* Do not pin the cached scan BSSID. The AP was scanned before portal
         * mode and a mesh/dual-band AP may select another radio meanwhile. */
        int connect_ret = sys_network_connect_wifi(ssid, pwd, NULL);
        /* Synchronous wifi_mgr_sta_connect resumes its saved-AP scanner before
         * returning on both success and failure. Cancel it immediately; on a
         * successful link the normal connection callback/DHCP path continues. */
        sys_wifi_reconnect_saved_once_stop();
        lock();
        s_ctx.transaction_busy = false;
        unlock();
        if (connect_ret != 0) {
            LISA_LOGW(TAG, "connect request for \"%s\" rejected on attempt %d", ssid, attempt);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
            for (int waited = 1000; waited < PORTAL_CONNECT_TIMEOUT_MS; waited += 200) {
                if (s_ctx.stop_requested) {
                    break;
                }
                if (sys_wifi_is_connected()) {
                    ok = true;
                    break;
                }

                sys_wifi_connect_result_t r = sys_wifi_get_connect_result();

                if (r == SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD || r == SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP ||
                    r == SYS_WIFI_CONNECT_RESULT_FAIL_IP || r == SYS_WIFI_CONNECT_RESULT_FAIL_OTHER) {
                    LISA_LOGW(TAG, "connect to \"%s\" failed on attempt %d, result %d", ssid, attempt, (int)r);
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }

        if (!ok && attempt < PORTAL_CONNECT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(PORTAL_CONNECT_RETRY_MS));
        }
    }

out:
    /* 密码用完立刻从栈和上下文里抹掉。 */
    secure_zero(pwd, sizeof(pwd));
    lock();
    secure_zero(s_ctx.req_pwd, sizeof(s_ctx.req_pwd));
    s_ctx.transaction_busy = false;
    unlock();

    return ok;
}

static void portal_task(void *arg)
{
    (void)arg;

    /* The button path plays its acknowledgement synchronously with the click.
     * Automatic boot/failure entry still owns its prompt here. */
    if (s_ctx.reason != APP_NETCFG_PORTAL_REASON_USER_REQUEST) {
        play_tone(TONE_ID_70); /* 070_enter_config_net_mode */
    }
    led_provisioning();

    scan_and_cache(true);

    /* The saved AP may recover while the fallback scan is in flight. Never
     * tear down that healthy STA for a now-obsolete automatic failure. */
    lock();
    bool recovered = s_ctx.reason == APP_NETCFG_PORTAL_REASON_CONNECT_FAILED && sys_wifi_is_connected();
    if (recovered) s_ctx.offline_closing = true;
    unlock();
    if (recovered) {
        sys_network_diag_mark("portal", "entry_cancelled", 0, "station_recovered_after_scan");
        led_off();
        portal_mark_inactive();
        vTaskDelete(NULL);
        return;
    }

    /* 单 vif：必须先彻底停 STA，AP 才起得来。 */
    sys_wifi_stop();

    if (softap_up() != 0 || netcfg_portal_http_start() != 0) {
        softap_down();
        netcfg_portal_http_stop();
        led_off();
        play_tone(TONE_ID_65); /* 065_net_error */
        LISA_LOGE(TAG, "portal failed to come up, restoring STA");
        sta_up();
        sys_wifi_start(!s_ctx.stop_requested);
        portal_mark_inactive();
        vTaskDelete(NULL);
        return;
    }

    bool connected = false;
    bool portal_failed = false;
portal_wait:
    while (!s_ctx.stop_requested) {
        if (xSemaphoreTake(s_ctx.submitted, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (s_ctx.stop_requested) {
                break; /* stop() also wakes this semaphore; it is not a credential. */
            }
            lock();
            bool credential_active = s_ctx.credential_active;
            unlock();
            if (!credential_active) continue; /* A cancelled takeover also wakes us. */
            /* 让 HTTP 把 {"ok":true} 发完，再拆掉手机脚下的热点。 */
            vTaskDelay(pdMS_TO_TICKS(600));

            connected = try_connect();
            lock();
            s_ctx.credential_active = false;
            unlock();

            if (connected || s_ctx.stop_requested) {
                break;
            }

            /* 连不上就把热点重新发出来，客户不需要重启板子再试一次。 */
            play_tone(TONE_ID_60); /* 060_network_fail */
            lock();
            s_ctx.transaction_busy = true;
            unlock();
            sys_wifi_stop();

            if (softap_up() != 0 || netcfg_portal_http_start() != 0) {
                LISA_LOGE(TAG, "cannot re-open portal after a failed attempt");
                portal_failed = true;
                break;
            }

            led_provisioning();
            lock();
            s_ctx.transaction_busy = false;
            unlock();
            continue;
        }
    }

    /* Last check under the same lock used by association/manual entry. Once
     * closing is committed, a newly arriving client is after the takeover. */
    lock();
    if (!connected && !portal_failed && !s_ctx.stop_requested && !s_ctx.offline_closing) {
        unlock();
        goto portal_wait;
    }
    s_ctx.offline_closing = s_ctx.offline_takeover;
    bool silent_exit = s_ctx.offline_takeover;
    unlock();

    softap_down();
    netcfg_portal_http_stop();

    if (connected) {
        /*
         * 上面的 softap_down() 成功路径也会走，而关 AP 不保证 vif 留在 STA。
         * 连上之后同样要显式确认 STA 在位，否则 LED 和提示音报成功，实际网络
         * 已经断了，自动重连也拿不到可用的 vif。
         */
        sta_up();
        if (!sys_wifi_is_started()) {
            sys_wifi_start(!s_ctx.stop_requested);
        }

        led_success();
        play_tone(TONE_ID_72); /* 072_config_net_success */
        app_ble_netcfg_complete();
        LISA_LOGI(TAG, "provisioning done");
    } else {
        led_off();
        /* 071 is the binding prompt, not a portal-exit prompt; keep exit silent. */
        /*
         * 没配成也要把 STA 恢复回自动重连，旧凭据仍然有效。
         * 关掉 AP 并不会把 vif 模式切回 STA，这里必须显式切，否则退出配网之后
         * 自动重连拿到的还是 AP 模式的 vif，板子再也连不上网。
         */
        sta_up();
        if (!sys_wifi_is_started()) {
            /* Voice takeover is not radio shutdown: saved hotspots keep
             * reconnecting in the background without stealing offline owner. */
            sys_wifi_start(!s_ctx.stop_requested || silent_exit);
        }
    }

    portal_mark_inactive();
    vTaskDelete(NULL);
}

int app_netcfg_portal_init(void)
{
    if (s_ctx.inited) {
        return 0;
    }

    s_ctx.lock = xSemaphoreCreateMutex();
    s_ctx.submitted = xSemaphoreCreateBinary();

    if (s_ctx.lock == NULL || s_ctx.submitted == NULL) {
        LISA_LOGE(TAG, "alloc sync objects failed");
        if (s_ctx.lock != NULL) {
            vSemaphoreDelete(s_ctx.lock);
            s_ctx.lock = NULL;
        }
        if (s_ctx.submitted != NULL) {
            vSemaphoreDelete(s_ctx.submitted);
            s_ctx.submitted = NULL;
        }
        return -ENOMEM;
    }

    build_ssid();
    int event_ret = ls_event_register_cb(EVENT_WIFI, EVENT_ID_ALL, portal_wifi_event, NULL);
    s_ctx.client_events_ready = event_ret == 0;
    if (event_ret != 0) {
        LISA_LOGW(TAG, "AP client monitor unavailable (%d), offline takeover disabled", event_ret);
    }
    s_ctx.inited = true;

    LISA_LOGI(TAG, "netcfg portal ready, hotspot name \"%s\"", s_ctx.ap_ssid);
    return 0;
}

int app_netcfg_portal_start(app_netcfg_portal_reason_t reason)
{
    uintptr_t caller = (uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0));
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (service_v2_voice_standby_background_audio_suppressed()) {
        return -EAGAIN;
    }
#endif
    if (sys_wifi_radio_suspended()) {
        return -EAGAIN;
    }
    if (!s_ctx.inited) {
        return -EPERM;
    }
    if (reason == APP_NETCFG_PORTAL_REASON_CONNECT_FAILED && sys_wifi_is_connected()) {
        sys_network_diag_mark("portal", "entry_cancelled", -EAGAIN, "station_already_connected");
        return -EAGAIN;
    }

    lock();
    if (s_ctx.scan_busy) {
        unlock();
        return -EBUSY;
    }
    if (s_ctx.active) {
        if (reason == APP_NETCFG_PORTAL_REASON_USER_REQUEST) {
            if (s_ctx.stop_requested && !s_ctx.offline_takeover) {
                unlock();
                return -EINPROGRESS; /* Caller retries once committed teardown finishes. */
            }
            if (s_ctx.offline_closing) {
                unlock();
                return -EINPROGRESS;
            }
            s_ctx.reason = reason; /* A repeated triple click makes an automatic portal manual. */
            s_ctx.start_caller = caller;
            if (s_ctx.offline_takeover && !s_ctx.offline_closing) {
                s_ctx.offline_takeover = false;
                s_ctx.stop_requested = false;
            }
        }
        unlock();
        return -EALREADY;
    }

    s_ctx.active = true;
    s_ctx.stop_requested = false;
    s_ctx.offline_takeover = false;
    s_ctx.offline_closing = false;
    s_ctx.credential_active = false;
    s_ctx.ap_clients = 0U;
    s_ctx.ap_clients_known = false;
    s_ctx.reason = reason;
    s_ctx.start_caller = caller;
    secure_zero(s_ctx.req_ssid, sizeof(s_ctx.req_ssid));
    secure_zero(s_ctx.req_pwd, sizeof(s_ctx.req_pwd));
    unlock();
    xSemaphoreTake(s_ctx.submitted, 0);

    if (xTaskCreate(portal_task, "netcfg_portal", PORTAL_TASK_STACK / sizeof(StackType_t), NULL, PORTAL_TASK_PRIO,
                    &s_ctx.task) != pdPASS) {
        portal_mark_inactive();
        LISA_LOGE(TAG, "create portal task failed");
        return -ENOMEM;
    }

    LISA_LOGI(TAG, "entering netcfg portal, reason %d", (int)reason);
    sys_network_diag_mark("portal", "enter", (int)reason, "start_requested");
    return 0;
}

int app_netcfg_portal_stop(void)
{
    lock();
    if (!s_ctx.active) {
        unlock();
        return 0;
    }

    /* An explicit stop (sleep/BLE/user) is not an opportunistic voice takeover;
     * a late association must not cancel it. */
    s_ctx.offline_takeover = false;
    s_ctx.stop_requested = true;
    unlock();

    xSemaphoreGive(s_ctx.submitted);
    for (int i = 0; i < 100 && app_netcfg_portal_is_active(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return app_netcfg_portal_is_active() ? -EBUSY : 0;
}

void app_netcfg_portal_get_status(app_netcfg_portal_status_t *status)
{
    if (!status) return;
    lock();
    *status = (app_netcfg_portal_status_t){
        .reason = s_ctx.reason, .active = s_ctx.active, .ap_up = s_ctx.ap_up,
        .clients_known = s_ctx.ap_clients_known, .clients = s_ctx.ap_clients,
        .credential_active = s_ctx.credential_active, .transaction_busy = s_ctx.transaction_busy,
        .scan_busy = s_ctx.scan_busy, .closing = s_ctx.stop_requested || s_ctx.offline_closing,
        .start_caller = s_ctx.start_caller,
    };
    unlock();
}

bool app_netcfg_portal_is_active(void)
{
    bool active;

    lock();
    active = s_ctx.active;
    unlock();

    return active;
}

const char *app_netcfg_portal_ip(void)
{
    return NETCFG_PORTAL_IP_STR;
}

bool app_netcfg_portal_ssid_copy(char *buf, size_t len)
{
    if (buf == NULL || len == 0 || !s_ctx.inited) {
        return false;
    }

    netcfg_portal_ssid_get(buf, len);
    return true;
}
