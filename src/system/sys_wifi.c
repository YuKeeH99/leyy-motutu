#include <stdio.h>
#include <string.h>
#include "ls_event.h"
#include "ls_wifi_type.h"
#include "net_al.h"
#include "net_def.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"

#include "lisa_wifi.h"
#include "lisa_thread.h"
#include "ls_misc.h"
#include "mac_manager.h"
#include "mac_manager_ops.h"
#include "ipc/mrpc_wifi_api_client.h"
#include "sys_network_diag.h"
#include "sys_wifi.h"
#include "sys_wifi_reconnect_policy.h"
#include "sys_wifi_radio_power.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_v2_voice_standby.h"
#endif
#include <errno.h>

static sys_wifi_radio_power_t s_radio_power;
#include "wifi_manager/wifi_manager.h"

#include "voice_msg.h"

#if CONFIG_SAL_USING_POSIX
#include "netdev.h"
#endif

#define TAG "user_wifi"
#include "lisa_log.h"

typedef struct {
    bool stack_inited;
    bool ready;
    bool started;
    bool connected;
    bool autoconnect_requested;
    bool autoconnect_enabled;
    bool saved_scan_absent;
    bool netdev_registered;
    bool dhcp_cb_registered;
    bool dhcp_pending;
    uint32_t dhcp_started_ms;
    bool realtime_mode_requested;
    bool realtime_mode_applied;
    uint32_t ip_addr;
    sys_wifi_connect_result_t connect_result;
} sys_wifi_status_t;

static sys_wifi_status_t s_wifi = {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    .realtime_mode_requested = true,
#endif
};

static mac_manager_t *m_mac_manager = NULL;

static void wifi_mgr_connection_cb(wifi_mgr_connection_info_t *connection_info, void *arg);
static void wifi_mgr_scan_done_cb(wifi_mgr_scan_info_t *aps_info, int ap_num, void *arg);
static int sys_wifi_register_netdev(void);
static int sys_wifi_start_autoconnect(void);
static void sys_wifi_stop_autoconnect(void);
static int sys_wifi_apply_power_policy(const char *reason);
static int sys_wifi_init_sta_config(wifi_mgr_sta_config_t *sta_config,
                                    const char *ssid,
                                    const char *pwd,
                                    const char *bssid);
bool sys_wifi_has_ap(void);

#define SYS_WIFI_STALE_LINK_RELEASE_TIMEOUT_MS 1000U
#define SYS_WIFI_STALE_LINK_POLL_MS              20U
#define SYS_WIFI_DHCP_TIMEOUT_MS              15000U

static bool sys_wifi_is_password_error_code(int code)
{
    return code == WIFI_ERROR_STA_AUTH_FAIL ||
           code == WIFI_ERROR_WPA3_PWD_OR_AUTH_FAIL ||
           code == WIFI_ERROR_FOUND_SSID_BUT_KEY_MISMATCH ||
           code == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT;
}

static bool sys_wifi_is_password_error(const wifi_mgr_connection_info_t *connection_info)
{
    return sys_wifi_is_password_error_code(connection_info->error_code) ||
           sys_wifi_is_password_error_code(connection_info->status_code) ||
           sys_wifi_is_password_error_code(connection_info->reason);
}

static bool sys_wifi_is_no_ap_error_code(int code)
{
    return code == WIFI_ERROR_STA_CONNECT_NO_TARGET_AP;
}

static bool sys_wifi_is_no_ap_error(const wifi_mgr_connection_info_t *connection_info)
{
    return sys_wifi_is_no_ap_error_code(connection_info->error_code) ||
           sys_wifi_is_no_ap_error_code(connection_info->status_code) ||
           sys_wifi_is_no_ap_error_code(connection_info->reason);
}

void sys_wifi_reset_connect_result(void)
{
    s_wifi.connect_result = SYS_WIFI_CONNECT_RESULT_SUCCESS;
}

static void sys_wifi_clear_ip_addr(void)
{
    s_wifi.ip_addr = 0;
    s_wifi.dhcp_pending = false;
}

static void sys_wifi_set_ip_addr(uint32_t ip_addr)
{
    s_wifi.ip_addr = ip_addr;
}

static void sys_wifi_set_connect_result(sys_wifi_connect_result_t result)
{
    s_wifi.connect_result = result;
}

static int sys_wifi_apply_power_policy(const char *reason)
{
    wifi_ps_mode_e mode;
    int ret;

    if (!s_wifi.ready || sys_wifi_radio_suspended()) {
        return 0;
    }

    mode = s_wifi.realtime_mode_requested ? WIFI_PS_MODE_OFF : WIFI_PS_MODE_DTIM;
    ret = wifi_ps_mode_set(mode);
    if (ret == 0) {
        s_wifi.realtime_mode_applied = s_wifi.realtime_mode_requested;
    }

    sys_network_diag_mark("wifi", "power_policy", ret,
                          s_wifi.realtime_mode_requested ? "realtime_off" : "idle_dtim");
    LISA_LOGI(TAG, "WiFi power policy=%s ret=%d reason=%s",
              s_wifi.realtime_mode_requested ? "realtime(power-save off)" : "idle(DTIM)",
              ret, reason ? reason : "");
    return ret;
}

int sys_wifi_set_realtime_mode(bool enabled)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* Conversation exit is not device sleep. Keep the hotspot association
     * awake through idle gaps; the standby coordinator owns radio shutdown. */
    int standby = service_v2_voice_standby_get_state();
    if (standby != SERVICE_V2_VOICE_STANDBY_ENTERING &&
        standby != SERVICE_V2_VOICE_STANDBY_ASLEEP) enabled = true;
#endif
    if (s_wifi.realtime_mode_requested == enabled &&
        (!s_wifi.ready || s_wifi.realtime_mode_applied == enabled)) {
        return 0;
    }

    s_wifi.realtime_mode_requested = enabled;
    return sys_wifi_apply_power_policy(enabled ? "cloud_audio_start" : "cloud_audio_end");
}

static bool sys_wifi_format_ip_string(uint32_t ip_addr, char *buf, size_t len)
{
    ip4_addr_t addr;

    if (buf == NULL || len == 0U || ip_addr == 0U) {
        return false;
    }

    ip4_addr_set_u32(&addr, ip_addr);
    return ip4addr_ntoa_r(&addr, buf, (int)len) != NULL;
}

/* extern 声明 SDK nv_config.c 中的 MAC eFuse 接口 */
extern int8_t nv_efuse_read_mac(uint8_t *mac_addr);
#ifdef CONFIG_VENDOR_STORAGE_ENABLE
#include "vendor_ops.h"

#define ARCS_MAC_HEADER_0 0x26
#define ARCS_MAC_HEADER_1 0x48

static int vendor_storage_random(uint8_t *mac, size_t *mac_len)
{
    unsigned int curTickCount = (unsigned int)xTaskGetTickCount();

    LISA_LOGI(TAG, "vendor_storage_random curTickCount:%d\n", curTickCount);
    srand(curTickCount);

    mac[0] = 0x26;
    mac[1] = 0x48;
    for (size_t i = 2; i < *mac_len; i++) {
        mac[i] = rand() % 256;
    }

    return 0;
}

static int vendor_storage_get(uint8_t *mac_buf, size_t *mac_buf_len)
{
    if (!mac_buf || !mac_buf_len || *mac_buf_len < 6) {
        return -1;
    }

    if (vendor_storage_read(VENDOR_WIFI_MAC_ID, mac_buf, *mac_buf_len) == 0) {
        return 0;
    }

    efuse_init();
    uint64_t uuid = efuse_read_uuid();
    if (uuid == 0) {
        LISA_LOGE(TAG, "efuse read empty uuid");
        return -1;
    }
    mac_buf[0] = ARCS_MAC_HEADER_0;
    mac_buf[1] = ARCS_MAC_HEADER_1;
    mac_buf[2] = (uuid & 0xFF);
    mac_buf[3] = (uuid >> 32 >> 16) & 0xFF;
    mac_buf[4] = (uuid >> 32 >> 8) & 0xFF;
    mac_buf[5] = (uuid >> 32) & 0xFF;

    *mac_buf_len = 6;
    return 0;
}

static int vendor_storage_set(const uint8_t *mac, size_t mac_len)
{
    if (!mac || mac_len != 6) {
        return -1;
    }

    return vendor_storage_write(VENDOR_WIFI_MAC_ID, (void *)mac, mac_len);
}

static int vendor_storage_del(void)
{
    return -1;
}

static mac_manager_content_ops_t mac_manager_venor_ops = {
    .random = vendor_storage_random,
    .get    = vendor_storage_get,
    .set    = vendor_storage_set,
    .del    = vendor_storage_del,
};
#endif

#ifdef CONFIG_MAC_EFUSE_ENABLE
#include "Driver_EFUSE.h"

#define ARCS_MAC_HEADER_0 0x26
#define ARCS_MAC_HEADER_1 0x48

/**
 * @brief 从 eFuse 读取 MAC 地址，读取失败时使用 UUID 生成确定性 MAC
 * @param mac_buf 输出 MAC 地址缓冲区
 * @param mac_buf_len MAC 地址长度（应为 6）
 * @return 0=成功, -1=失败
 */
static int efuse_mac_get(uint8_t *mac_buf, size_t *mac_buf_len)
{
    if (!mac_buf || !mac_buf_len || *mac_buf_len < 6) {
        return -1;
    }

    /* 调用 SDK 底层接口读取 eFuse MAC */
    int8_t ret = nv_efuse_read_mac(mac_buf);
    if (ret == 0) {
        *mac_buf_len = 6;
        LISA_LOGI(TAG, "Read MAC from eFuse: %02X:%02X:%02X:%02X:%02X:%02X",
                  mac_buf[0], mac_buf[1], mac_buf[2],
                  mac_buf[3], mac_buf[4], mac_buf[5]);
        return 0;
    }

    /* eFuse MAC 读取失败，使用 UUID 生成确定性 MAC */
    LISA_LOGW(TAG, "eFuse MAC not found, generating MAC from UUID");
    efuse_init();
    uint64_t uuid = efuse_read_uuid();
    if (uuid == 0) {
        LISA_LOGE(TAG, "efuse read empty uuid");
        return -1;
    }
    mac_buf[0] = ARCS_MAC_HEADER_0;
    mac_buf[1] = ARCS_MAC_HEADER_1;
    mac_buf[2] = (uuid & 0xFF);
    mac_buf[3] = (uuid >> 32 >> 16) & 0xFF;
    mac_buf[4] = (uuid >> 32 >> 8) & 0xFF;
    mac_buf[5] = (uuid >> 32) & 0xFF;

    *mac_buf_len = 6;
    return 0;
}

/**
 * @brief eFuse MAC 不支持运行时写入（仅产测固件支持）
 */
static int efuse_mac_set(const uint8_t *mac, size_t mac_len)
{
    return -1;
}

/**
 * @brief eFuse MAC 不支持删除
 */
static int efuse_mac_del(void)
{
    return -1;
}

/**
 * @brief eFuse MAC 不支持随机生成（get 已通过 UUID 回退处理）
 */
static int efuse_mac_random(uint8_t *mac, size_t *mac_len)
{
    return -1;
}

static mac_manager_content_ops_t mac_manager_efuse_ops = {
    .random = efuse_mac_random,
    .get    = efuse_mac_get,
    .set    = efuse_mac_set,
    .del    = efuse_mac_del,
};
#endif

static void user_mac_manager_init(void)
{
    mac_manager_config_t config = {
        .random_mac_if_mac_invalid = false,
    };


    m_mac_manager = mac_manager_init(
        mac_manager_ops_get()->mem_ops,
#ifdef CONFIG_MAC_EFUSE_ENABLE
        &mac_manager_efuse_ops,  /* 优先使用 eFuse MAC */
#elif defined(CONFIG_VENDOR_STORAGE_ENABLE)
        &mac_manager_venor_ops,  /* 备选方案：vendor_storage */
#else
        mac_manager_ops_get()->content_ops,  /* 默认方案 */
#endif
        &config);
  
    if (m_mac_manager == NULL) {
        LISA_LOGE(TAG,"[user_wifi]mac_manager_init failed\n");
        assert(0);
    }
}

static void dhcp_status_callback(int vif_idx, bool success, uint32_t ip_addr, uint32_t netmask, uint32_t gateway, void *arg)
{
    if (sys_wifi_radio_suspended() || !s_wifi.started ||
        wifi_mgr_sta_get_status() != WIFI_MGR_STA_CONNECTED) {
        return; /* Late completion from the pre-sleep connection. */
    }
    s_wifi.dhcp_pending = false;
    if (success) {
        char ip_buf[24] = {0};
        char mask_buf[24] = {0};
        char gw_buf[24] = {0};

        s_wifi.connected = true;
        sys_wifi_set_ip_addr(ip_addr);
        sys_wifi_set_connect_result(SYS_WIFI_CONNECT_RESULT_SUCCESS);
        voice_msg_pub(VOICE_MSG_WIFI_IP_GOT, NULL, 0);
        sys_wifi_format_ip_string(ip_addr, ip_buf, sizeof(ip_buf));
        sys_wifi_format_ip_string(netmask, mask_buf, sizeof(mask_buf));
        sys_wifi_format_ip_string(gateway, gw_buf, sizeof(gw_buf));
        sys_network_diag_mark("wifi", "dhcp_success", vif_idx, ip_buf);
        LISA_LOGI(TAG, "DHCP Success on VIF-%d: IP=%s, Mask=%s, GW=%s",
                  vif_idx, ip_buf, mask_buf, gw_buf);
    } else {
        s_wifi.connected = false;
        sys_wifi_clear_ip_addr();
        sys_wifi_set_connect_result(SYS_WIFI_CONNECT_RESULT_FAIL_IP);
        sys_network_diag_mark("wifi", "dhcp_failed", vif_idx, "no_ip");
        LISA_LOGW(TAG,"DHCP Failed on VIF-%d", vif_idx);
        voice_msg_pub(VOICE_MSG_WIFI_DISCONNECTED, NULL, 0);
    }
}

static int sys_wifi_register_netdev(void)
{
#if CONFIG_SAL_USING_POSIX
    if (s_wifi.netdev_registered) {
        return 0;
    }

    if (app_netdev_register("wifi0", 200) != 0) {
        LISA_LOGW(TAG, "Failed to register WiFi network device");
        return -1;
    }

    s_wifi.netdev_registered = true;
#endif

    return 0;
}

static void sys_wifi_unregister_netdev(void)
{
#if CONFIG_SAL_USING_POSIX
    if (!s_wifi.netdev_registered) {
        return;
    }

    netdev_unregister_by_name("wifi0");
    s_wifi.netdev_registered = false;
#endif
}

static int sys_wifi_start_autoconnect(void)
{
    if (sys_wifi_radio_suspended()) {
        return -EAGAIN;
    }
    if (!s_wifi.ready || s_wifi.autoconnect_enabled) {
        return s_wifi.ready ? 0 : -1;
    }

    wifi_mgr_autoconn_config_t cnn_cfg = {
        .interval_ms = 1000,
        .max_interval_ms = 5000,
    };

    int ret = wifi_mgr_auto_connect_start(&cnn_cfg);
    if (ret != 0) {
        sys_network_diag_mark("wifi", "autoconnect_start_failed", ret, NULL);
        LISA_LOGW(TAG, "WiFi manager auto connect start failed: %d", ret);
        return ret;
    }
    s_wifi.autoconnect_enabled = true;
    sys_network_diag_mark("wifi", "autoconnect_start", (int)cnn_cfg.interval_ms, "max_interval_ms=5000");

    LISA_LOGI(TAG, "WiFi manager auto connect started with interval %d ms, max %d ms",
              cnn_cfg.interval_ms, cnn_cfg.max_interval_ms);
    return 0;
}

static void sys_wifi_stop_autoconnect(void)
{
    if (!s_wifi.ready) {
        return;
    }

    /* wifi_mgr_sta_connect(..., false) resumes the SDK auto-connect worker
     * internally before it returns.  That state change is invisible to this
     * wrapper, so s_wifi.autoconnect_enabled can legitimately still be false.
     * Always issue the idempotent SDK stop to close that race. */
    (void)wifi_mgr_auto_connect_stop();
    s_wifi.autoconnect_enabled = false;
    sys_network_diag_mark("wifi", "autoconnect_stop", 0, NULL);
    LISA_LOGI(TAG, "WiFi manager auto connect stopped");
}

static void wifi_mgr_init_done_cb(void)
{
    LISA_LOGI(TAG, "lisa_wifi_init_done");
    wifi_mgr_init(wifi_mgr_ops_get());
    wifi_mgr_sta_enable();

    wifi_mgr_sta_add_connection_cb(wifi_mgr_connection_cb, NULL);
    wifi_mgr_add_scan_done_cb(wifi_mgr_scan_done_cb, NULL);
    s_wifi.ready = true;
    sys_network_diag_mark("wifi", "manager_ready", s_wifi.started ? 1 : 0,
                          s_wifi.autoconnect_requested ? "autoconnect_requested" : "manual_connect");
    if (s_wifi.realtime_mode_requested) {
        (void)sys_wifi_apply_power_policy("wifi_manager_ready");
    }

    if (s_wifi.started) {
        sys_wifi_register_netdev();
        if (s_wifi.autoconnect_requested) {
            sys_wifi_start_autoconnect();
        }
    }
}

static int8_t custom_get_wifi_mac(uint8_t mac_addr[6])
{
    int8_t ret = 0;

    if (!mac_addr)
        return -1;

    ret = mac_manager_get(m_mac_manager, mac_addr, 6);
    LISA_LOGI(TAG,"custom_get_wifi_mac: %d, %02X:%02X:%02X:%02X:%02X:%02X", ret, mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    return ret;
}

static void wifi_mgr_scan_done_cb(wifi_mgr_scan_info_t *aps_info, int ap_num, void *arg)
{
    bool has_candidate = false;

    if (sys_wifi_radio_suspended()) {
        return;
    }

    (void)arg;

    if (!s_wifi.started ||
        s_wifi.connected) {
        return;
    }

    if (!(s_wifi.autoconnect_requested || s_wifi.autoconnect_enabled) || !sys_wifi_has_ap()) {
        return;
    }

    s_wifi.saved_scan_absent = false;
    if (ap_num < 0 || aps_info == NULL) return;
    if (ap_num > 0 && aps_info != NULL) {
        for (int idx = 0; idx < ap_num; idx++) {
            wifi_mgr_sta_config_t ap = {0};
            int count = wifi_mgr_storage_search_ap(&ap, 1, SEARCH_BY_BSSID, aps_info[idx].bssid);

            if (count < 0) return; /* Storage failure is not evidence of no signal. */
            if (count <= 0 || ap.ssid[0] == '\0') {
                count = wifi_mgr_storage_search_ap(&ap, 1, SEARCH_BY_SSID, aps_info[idx].ssid);
            }

            if (count < 0) return;
            if (count > 0 && ap.ssid[0] != '\0') {
                if (ap.bssid[0] != '\0' && strcmp(ap.bssid, aps_info[idx].bssid) != 0) {
                    continue;
                }

                has_candidate = true;
                break;
            }
        }
    }

    if (has_candidate) {
        return;
    }

    s_wifi.saved_scan_absent = true;
    LISA_LOGI(TAG, "WiFi scan done, no saved AP available");
    sys_network_diag_mark("wifi", "scan_no_saved_ap", ap_num, "publish_wifi_disconnected");
    sys_wifi_set_connect_result(SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP);
    voice_msg_pub(VOICE_MSG_WIFI_DISCONNECTED, NULL, 0);
}

static void wifi_mgr_connection_cb(wifi_mgr_connection_info_t *connection_info, void *arg)
{
    net_if_t *net_if;
    char diag_detail[96];

    if (sys_wifi_radio_suspended()) {
        /* Pause owns teardown; callbacks must not restore netif or DHCP. */
        return;
    }

    snprintf(diag_detail, sizeof(diag_detail), "error=%d assoc=%d reason=%d",
             connection_info->error_code, connection_info->status_code, connection_info->reason);
    sys_network_diag_mark("wifi", "manager_connection", connection_info->status, diag_detail);

    LISA_LOGI(TAG,"mgr connection: %d", connection_info->status);

    switch (connection_info->status)
    {
        case WIFI_MGR_STA_CONNECTED:
            /* An asynchronous manual connect makes wifi_manager enqueue an
             * auto-connect START before dispatching this callback.  Queue a
             * matching STOP now; otherwise the delayed worker can reassociate
             * an already-connected station and tear down an active dialog. */
            (void)wifi_mgr_auto_connect_stop();
            s_wifi.autoconnect_enabled = false;
            sys_network_diag_mark("wifi", "autoconnect_stop_on_link", 0,
                                  "cancel_sdk_async_resume");
            LISA_LOGI(TAG,"WiFi connected, starting network interface");
            net_if = net_if_get(WIFI_VIF_STA_IDX);
            net_if_up(net_if);
            if (!net_if->static_ip) {
                s_wifi.connected = false;
                sys_wifi_clear_ip_addr();
                s_wifi.dhcp_started_ms = (uint32_t)lisa_os_get_tick_ms();
                s_wifi.dhcp_pending = true;
                sys_wifi_reset_connect_result();
                ls_dhcpc_start(WIFI_VIF_STA_IDX);
            } else {
                uint32_t ip_addr = 0;

                s_wifi.connected = true;
                if (net_if_get_ip(net_if, &ip_addr, NULL, NULL) == 0) {
                    sys_wifi_set_ip_addr(ip_addr);
                }
                sys_wifi_set_connect_result(SYS_WIFI_CONNECT_RESULT_SUCCESS);
                voice_msg_pub(VOICE_MSG_WIFI_IP_GOT, NULL, 0);
            }
            
            voice_msg_pub(VOICE_MSG_WIFI_CONNECTED, NULL, 0);
            sys_network_diag_mark("wifi", "link_connected", connection_info->status, "dhcp_pending");
            /* The Wi-Fi firmware may reset its power-save state while
             * reassociating, so preserve an active real-time session. */
            if (s_wifi.realtime_mode_requested) {
                (void)sys_wifi_apply_power_policy("link_connected");
            }
            /*save wifi sta info*/
            memset(connection_info->sta_info->bssid,0,sizeof(connection_info->sta_info->bssid));
            wifi_mgr_storage_save_ap(connection_info->sta_info);
            break;

        case WIFI_MGR_STA_DISCONNECTED:
        case WIFI_MGR_STA_CONNECT_FAILED:
        {
            LISA_LOGI(TAG,"WiFi disconnected, stopping network interface");
            ls_dhcpc_stop(WIFI_VIF_STA_IDX);
            net_if_down(net_if_get(WIFI_VIF_STA_IDX));
            s_wifi.connected = false;
            sys_wifi_clear_ip_addr();
            if (sys_wifi_is_no_ap_error(connection_info)) {
                sys_wifi_set_connect_result(SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP);
            } else if (sys_wifi_is_password_error(connection_info)) {
                sys_wifi_set_connect_result(SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD);
            } else {
                sys_wifi_set_connect_result(SYS_WIFI_CONNECT_RESULT_FAIL_OTHER);
            }
            LISA_LOGW(TAG, "WiFi connect result status:%d error:%d assoc:%d reason:%d",
                      connection_info->status, connection_info->error_code,
                      connection_info->status_code, connection_info->reason);
            sys_network_diag_mark("wifi", "link_disconnected", connection_info->status, diag_detail);
            voice_msg_pub(VOICE_MSG_WIFI_DISCONNECTED, NULL, 0);
            break;
        }

        case WIFI_MGR_STA_CONNECTING:
            sys_network_diag_mark("wifi", "link_connecting", connection_info->status, diag_detail);
            LISA_LOGI(TAG,"WiFi connecting...");
            break;

        default:
            break;
    }
}

static int sys_wifi_init_sta_config(wifi_mgr_sta_config_t *sta_config,
                                    const char *ssid,
                                    const char *pwd,
                                    const char *bssid)
{
    if (sta_config == NULL || ssid == NULL || ssid[0] == '\0') {
        return -1;
    }

    memset(sta_config, 0, sizeof(*sta_config));

    strncpy(sta_config->ssid, ssid, sizeof(sta_config->ssid) - 1);
    sta_config->ssid[sizeof(sta_config->ssid) - 1] = '\0';

    if (pwd != NULL) {
        strncpy(sta_config->pwd, pwd, sizeof(sta_config->pwd) - 1);
        sta_config->pwd[sizeof(sta_config->pwd) - 1] = '\0';
    }

    if (bssid != NULL) {
        strncpy(sta_config->bssid, bssid, sizeof(sta_config->bssid) - 1);
        sta_config->bssid[sizeof(sta_config->bssid) - 1] = '\0';
    }

    sta_config->encryption_mode = WIFI_MGR_WIFI_AUTH_AUTO;
    return 0;
}

int sys_wifi_init(void)
{
    if (s_wifi.stack_inited) {
        return 0;
    }

    user_mac_manager_init();

    if (!s_wifi.dhcp_cb_registered) {
        net_dhcp_register_status_callback(dhcp_status_callback, NULL);
        s_wifi.dhcp_cb_registered = true;
    }

    lisa_wifi_ops_t ops = {
        .init_done = wifi_mgr_init_done_cb,
        .custom_mac = custom_get_wifi_mac,
    };
    lisa_wifi_init(&ops);
    s_wifi.stack_inited = true;

    return 0;
}

int sys_wifi_start(bool autoconnect)
{
    if (sys_wifi_radio_suspended()) {
        return -EAGAIN;
    }
    if (!s_wifi.stack_inited) {
        return -1;
    }

    s_wifi.started = true;
    s_wifi.autoconnect_requested = autoconnect;

    if (!s_wifi.ready) {
        return 0;
    }

    if (sys_wifi_register_netdev() != 0) {
        return -1;
    }

    if (autoconnect) {
        sys_wifi_start_autoconnect();
    } else {
        sys_wifi_stop_autoconnect();
    }

    return 0;
}

int sys_wifi_stop(void)
{
    s_wifi.started = false;
    s_wifi.autoconnect_requested = false;

    sys_wifi_unregister_netdev();

    if (!s_wifi.ready || sys_wifi_radio_suspended()) {
        s_wifi.connected = false;
        sys_wifi_clear_ip_addr();
        return 0;
    }

    sys_wifi_stop_autoconnect();
    /* Portal/bearer handoff is temporary. The manager's explicit disconnect
     * blacklists this SSID, making the saved hotspot unreachable afterwards. */
    (void)wifi_sta_disconnect();
    s_wifi.connected = false;
    sys_wifi_clear_ip_addr();

    return 0;
}

int sys_wifi_connect(const char *ssid, const char *pwd, const char *bssid)
{
    wifi_mgr_sta_config_t sta_config;

    if (!s_wifi.ready || !s_wifi.started || sys_wifi_radio_suspended()) {
        return -1;
    }

    if (sys_wifi_init_sta_config(&sta_config, ssid, pwd, bssid) != 0) {
        return -1;
    }

    sys_wifi_reset_connect_result();
    return wifi_mgr_sta_connect(&sta_config, false);
}

int sys_wifi_reconnect_saved_once_start(void)
{
    s_wifi.saved_scan_absent = false;
    if (sys_wifi_radio_suspended()) {
        return -EAGAIN;
    }
    bool has_saved_ap = sys_wifi_has_ap();
    sys_wifi_reconnect_decision_t decision = sys_wifi_reconnect_decide(
        s_wifi.ready, s_wifi.started, has_saved_ap,
        s_wifi.connected || wifi_mgr_sta_get_status() == WIFI_MGR_STA_CONNECTED);

    if (decision == SYS_WIFI_RECONNECT_UNAVAILABLE) {
        sys_network_diag_mark("wifi", "wakeup_reconnect_unavailable", -1,
                              !s_wifi.ready ? "manager_not_ready" :
                              (!s_wifi.started ? "station_not_started" : "no_saved_ap"));
        return -1;
    }

    sys_wifi_stop_autoconnect();

    if (decision == SYS_WIFI_RECONNECT_FORCE_REASSOCIATE) {
        wifi_mgr_sta_config_t stale_ap = {0};
        int info_ret = wifi_mgr_sta_get_connected_info(&stale_ap);
        int disconnect_ret;

        sys_network_diag_mark("wifi", "wakeup_reconnect_stale_link", info_ret,
                              "force_radio_reassociation");
        if (info_ret != 0 || stale_ap.ssid[0] == '\0') {
            LISA_LOGW(TAG, "stale WiFi link has no connected AP details: %d", info_ret);
            return -1;
        }

        /* Use the raw radio disconnect so wifi_manager does not blacklist a
         * valid saved AP merely because the cloud transport detected a stale
         * association.  Wait for its event before issuing the new connect. */
        disconnect_ret = wifi_sta_disconnect();
        if (disconnect_ret != 0) {
            sys_network_diag_mark("wifi", "wakeup_reconnect_disconnect_failed",
                                  disconnect_ret, "radio_disconnect_failed");
            return disconnect_ret;
        }

        uint32_t waited_ms = 0U;
        while (wifi_mgr_sta_get_status() == WIFI_MGR_STA_CONNECTED &&
               waited_ms < SYS_WIFI_STALE_LINK_RELEASE_TIMEOUT_MS) {
            lisa_thread_mdelay(SYS_WIFI_STALE_LINK_POLL_MS);
            waited_ms += SYS_WIFI_STALE_LINK_POLL_MS;
        }
        if (wifi_mgr_sta_get_status() == WIFI_MGR_STA_CONNECTED) {
            sys_network_diag_mark("wifi", "wakeup_reconnect_disconnect_timeout",
                                  (int)waited_ms, "radio_still_connected");
            return -1;
        }

        s_wifi.connected = false;
        sys_wifi_clear_ip_addr();
        sys_wifi_reset_connect_result();
        int ret = wifi_mgr_sta_connect(&stale_ap, true);
        sys_network_diag_mark("wifi", "wakeup_reconnect_forced", ret,
                              ret == 0 ? "saved_ap_reassociation_started" : "start_failed");
        return ret;
    }

    /* A wakeup is an explicit retry boundary.  Restart a stale/slow scan so
     * the SDK backoff returns to its one-second base interval. */
    int ret = sys_wifi_start_autoconnect();
    sys_network_diag_mark("wifi", "wakeup_reconnect_start", ret,
                          ret == 0 ? "saved_ap_scan_started" : "start_failed");
    return ret;
}

void sys_wifi_reconnect_saved_once_stop(void)
{
    sys_wifi_stop_autoconnect();
    sys_network_diag_mark("wifi", "wakeup_reconnect_stop", 0, "attempt_finished");
}

bool sys_wifi_reconnect_saved_exhausted(void)
{
    return s_wifi.ready && s_wifi.started && !sys_wifi_radio_suspended() &&
        !s_wifi.connected && wifi_mgr_sta_get_status() != WIFI_MGR_STA_CONNECTED &&
        wifi_mgr_auto_connect_exhausted();
}

/* Only a successful saved-AP scan plus a completed manager cycle qualifies.
 * Neither a scan/storage error nor failed authentication means no RF signal. */
bool sys_wifi_reconnect_saved_no_signal(void)
{
    return s_wifi.saved_scan_absent && sys_wifi_reconnect_saved_exhausted();
}

/* Used by the existing recovery worker; never tear down a healthy link or
 * interfere with a normal initial scan / an intentional radio pause. */
bool sys_wifi_ip_recovery_needed(void)
{
    if (!s_wifi.ready || !s_wifi.started || s_wifi.connected || sys_wifi_radio_suspended()) {
        return false;
    }
    bool expired = s_wifi.connect_result == SYS_WIFI_CONNECT_RESULT_FAIL_IP ||
        (s_wifi.dhcp_pending &&
         (uint32_t)((uint32_t)lisa_os_get_tick_ms() - s_wifi.dhcp_started_ms) >= SYS_WIFI_DHCP_TIMEOUT_MS);
    return expired && wifi_mgr_sta_get_status() == WIFI_MGR_STA_CONNECTED;
}

sys_wifi_connect_result_t sys_wifi_get_connect_result(void)
{
    return s_wifi.connect_result;
}

int sys_wifi_save_ap(const char *ssid, const char *pwd, const char *bssid)
{
    wifi_mgr_sta_config_t sta_config;
    int ret;

    if (!s_wifi.ready) {
        return -1;
    }

    if (sys_wifi_init_sta_config(&sta_config, ssid, pwd, bssid) != 0) {
        return -1;
    }

    ret = wifi_mgr_storage_save_ap(&sta_config);

    return ret;
}

int sys_wifi_clear_saved_aps(void)
{
    wifi_mgr_sta_config_t list[16] = {0};
    int batch_size = (int)(sizeof(list) / sizeof(list[0]));
    int count = 0;

    if (!s_wifi.ready) {
        s_wifi.connected = false;
        return 0;
    }

    (void)wifi_mgr_sta_disconnect(true);

    do {
        count = wifi_mgr_storage_search_ap(list, batch_size, SEARCH_ALL, NULL);
        if (count <= 0) {
            break;
        }

        for (int i = 0; i < count && i < batch_size; i++) {
            if (wifi_mgr_storage_delete_ap(&list[i]) != 0) {
                return -1;
            }
        }

        memset(list, 0, sizeof(list));
    } while (count >= batch_size);

    return 0;
}

bool sys_wifi_has_ap(void)
{
    wifi_mgr_sta_config_t ap = {0};
    int count = wifi_mgr_storage_search_ap(&ap, 1, SEARCH_ALL, NULL);
    return count > 0;
}

void sys_wifi_refresh_dnsserver(const char *const dns_srv)
{
    if (DNS_MAX_SERVERS >= 2) {
        ip_addr_t dns_ip_addr;
        if (ipaddr_aton(dns_srv, &dns_ip_addr)) {
            const ip_addr_t *current = dns_getserver(DNS_MAX_SERVERS - 1);
            if (current->addr != dns_ip_addr.addr) {
                LISA_LOGD(TAG, "%dth old dns server: %s", DNS_MAX_SERVERS, ipaddr_ntoa(current));
                dns_setserver(DNS_MAX_SERVERS - 1, &dns_ip_addr);
                current = dns_getserver(DNS_MAX_SERVERS - 1);
                LISA_LOGD(TAG, "%dth new dns server: %s", DNS_MAX_SERVERS, ipaddr_ntoa(current));
            }
        }
    }
}

bool sys_wifi_get_signal_quality(int *rssi)
{
    wifi_mgr_sta_config_t wifi_sta_info = {0};

    if (rssi == NULL || !s_wifi.ready || !s_wifi.connected) {
        return false;
    }

    if (wifi_mgr_sta_get_connected_info(&wifi_sta_info) != 0) {
        return false;
    }

    *rssi = wifi_sta_info.rssi;
    return true;
}

bool sys_wifi_get_ip_addr(uint32_t *ip_addr)
{
    if (ip_addr == NULL || !s_wifi.connected || s_wifi.ip_addr == 0) {
        return false;
    }

    *ip_addr = s_wifi.ip_addr;
    return true;
}

bool sys_wifi_get_ip_string(char *buf, size_t len)
{
    uint32_t ip_addr = 0;

    if (buf == NULL || len == 0U || !sys_wifi_get_ip_addr(&ip_addr)) {
        return false;
    }

    return sys_wifi_format_ip_string(ip_addr, buf, len);
}

bool sys_wifi_is_ready(void)
{
    return s_wifi.ready;
}

bool sys_wifi_is_started(void)
{
    return s_wifi.started;
}

bool sys_wifi_is_connected(void)
{
    return s_wifi.connected && !sys_wifi_radio_suspended();
}

bool sys_wifi_radio_suspended(void)
{
    return sys_wifi_radio_power_blocked(&s_radio_power);
}

bool sys_wifi_radio_is_off(void)
{
    return sys_wifi_radio_power_is_off(&s_radio_power);
}

static uint32_t s_radio_epoch;

uint32_t sys_wifi_radio_epoch(void)
{
    return __atomic_load_n(&s_radio_epoch, __ATOMIC_ACQUIRE);
}

static int radio_pause_io(void *arg)
{
    __atomic_store_n(&s_radio_epoch, sys_wifi_radio_epoch() + 1U, __ATOMIC_RELEASE);
    (void)arg;
    s_wifi.autoconnect_requested = false;
    s_wifi.autoconnect_enabled = false;
    /* Do not erase a working link until the pause barrier actually succeeds.
     * A timeout leaves the real STA connected; rollback must retain its IP. */
    return wifi_mgr_radio_pause(3000U);
}

static int radio_off(void *arg)
{
    (void)arg;
    s_wifi.started = false;
    s_wifi.connected = false;
    sys_wifi_clear_ip_addr();
    voice_msg_pub(VOICE_MSG_WIFI_DISCONNECTED, NULL, 0);
    ls_dhcpc_stop(WIFI_VIF_STA_IDX);
    net_if_t *net_if = net_if_get(WIFI_VIF_STA_IDX);
    if (net_if != NULL) {
        net_if_down(net_if);
    }
    sys_wifi_unregister_netdev();
    /* Explicit manager disconnect blacklists the current AP. Removing the
     * STA VIF releases the link without damaging the saved-AP retry policy. */
    int ret = wifi_mgr_sta_disable();
    return ret == 0 ? wifi_off() : ret;
}

static int radio_on(void *arg)
{
    (void)arg;
    return wifi_on();
}

static int radio_restore(void *arg)
{
    (void)arg;
    int ret = wifi_mgr_sta_is_enable() ? 0 : wifi_mgr_sta_enable();
    if (ret == 0) {
        ret = sys_wifi_register_netdev();
    }
    if (ret != 0) {
        return ret;
    }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    s_wifi.realtime_mode_requested = true;
    s_wifi.realtime_mode_applied = wifi_ps_mode_set(WIFI_PS_MODE_OFF) == 0;
#else
    s_wifi.realtime_mode_requested = false;
    s_wifi.realtime_mode_applied = false;
    (void)wifi_ps_mode_set(WIFI_PS_MODE_DTIM);
#endif
    s_wifi.started = true;
    sys_wifi_reset_connect_result();
    return 0;
}

static void radio_release_io(void *arg)
{
    (void)arg;
    wifi_mgr_radio_resume();
}

static const sys_wifi_radio_power_ops_t s_radio_ops = {
    .pause_io = radio_pause_io, .off = radio_off, .on = radio_on,
    .restore = radio_restore, .release_io = radio_release_io,
};

int sys_wifi_suspend_radio(void)
{
    if (!s_wifi.ready) {
        return -EAGAIN;
    }
    int ret = sys_wifi_radio_power_suspend(&s_radio_power, &s_radio_ops, NULL);
    LISA_LOGI(TAG, "standby radio suspend: ret=%d off=%d blocked=%d", ret,
              sys_wifi_radio_is_off(), sys_wifi_radio_suspended());
    return ret;
}

int sys_wifi_resume_radio(void)
{
    bool was_blocked = sys_wifi_radio_suspended();
    int ret = sys_wifi_radio_power_resume(&s_radio_power, &s_radio_ops, NULL);
    if (ret == 0 && was_blocked) {
        /* A pause/disable failure may leave an associated STA. Reconcile the
         * real link instead of waiting for an event that already occurred
         * while callbacks were gated. Also refresh the manager's cache. */
        if (wifi_mgr_sta_get_status() == WIFI_MGR_STA_CONNECTED) {
            net_if_t *net_if = net_if_get(WIFI_VIF_STA_IDX);
            uint32_t ip_addr = 0;
            if (net_if != NULL) {
                net_if_up(net_if);
                (void)net_if_get_ip(net_if, &ip_addr, NULL, NULL);
                s_wifi.connected = ip_addr != 0U;
                sys_wifi_set_ip_addr(ip_addr);
                if (!s_wifi.connected && !net_if->static_ip) {
                    ls_dhcpc_start(WIFI_VIF_STA_IDX);
                }
                if (s_wifi.connected) {
                    voice_msg_pub(VOICE_MSG_WIFI_IP_GOT, NULL, 0);
                }
            }
        } else {
            s_wifi.connected = false;
            sys_wifi_clear_ip_addr();
            voice_msg_pub(VOICE_MSG_WIFI_DISCONNECTED, NULL, 0);
        }
    }
    LISA_LOGI(TAG, "standby radio resume: ret=%d off=%d blocked=%d", ret,
              sys_wifi_radio_is_off(), sys_wifi_radio_suspended());
    return ret;
}
