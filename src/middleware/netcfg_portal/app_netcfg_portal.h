#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SoftAP 配网门户。
 *
 * 板子自己发一个开放热点（SSID 形如 ARCS-A1B2C3），手机连上后被自动拉起的
 * 网页可以从缓存的扫描列表里选网络填密码，也可以手动输入 SSID 和密码。
 *
 * 关键约束：`ls_wifi_type.h` 里 `VIF_MAX == 1`，AP 和 STA 共用同一个 vif，
 * 不能共存，AP 起来之后也扫不到网。所以顺序被硬性固定成：
 *
 *   STA 下扫描并缓存 -> 关 STA -> 起 AP + DHCP + HTTP -> 手机提交凭据
 *   -> 关 AP -> 起 STA 连接 -> 结果用提示音和 LED 播报（手机此时已掉线）
 *
 * 用户主动进入配网时保留旧凭据；成功后保存或更新本轮提交的热点。
 * 自动进入配网（首次开机/连接失败）不额外清除凭据。
 */

typedef enum {
    /** 开机时没有任何已保存的 AP，直接进配网。 */
    APP_NETCFG_PORTAL_REASON_NO_CREDENTIAL = 0,
    /** 有凭据但连不上，退回配网。 */
    APP_NETCFG_PORTAL_REASON_CONNECT_FAILED,
    /** 用户主动触发（电源键多击等）。 */
    APP_NETCFG_PORTAL_REASON_USER_REQUEST,
} app_netcfg_portal_reason_t;

/**
 * 分配资源并生成本机 SSID，不发射热点。
 *
 * 需要在 sys_wifi_init() 之后调用，因为 SSID 后缀取自 AP MAC。
 *
 * @return 0 成功，负值为 errno 风格错误码。
 */
int app_netcfg_portal_init(void);

/**
 * 进入配网模式：扫描缓存、切 AP、起 DHCP 和 HTTP 服务。
 *
 * 异步执行，函数在工作任务跑起来后就返回。重复调用在已激活时返回 -EALREADY。
 */
int app_netcfg_portal_start(app_netcfg_portal_reason_t reason);

/**
 * 退出配网模式：关闭 HTTP 监听和 DHCP，停 AP，恢复 STA 自动重连。
 *
 * 空闲时调用是安全的。退出后不会有任何常驻的对外监听端口。
 */
int app_netcfg_portal_stop(void);

bool app_netcfg_portal_is_active(void);

typedef struct {
    app_netcfg_portal_reason_t reason;
    bool active, ap_up, clients_known, credential_active, transaction_busy, scan_busy, closing;
    uint32_t clients;
    uintptr_t start_caller; /* Accepted entry/promotion return address for addr2line. */
} app_netcfg_portal_status_t;
/* RAM-only diagnostic snapshot; contains no submitted credentials. */
void app_netcfg_portal_get_status(app_netcfg_portal_status_t *status);

/* Non-mutating gate for short credential/VIF transactions and startup/teardown.
 * An open portal, a phone, and association/DHCP waits do not block voice. */
bool app_netcfg_portal_offline_blocked(void);
/* Compatibility query only: 0 when coexistence is allowed, -EBUSY during a
 * transaction. Never closes the portal or changes the radio. */
int app_netcfg_portal_offline_takeover(void);
/* Explicit enter-online: release an idle AP, including a manually opened one.
 * Submitted credentials and radio transactions return -EBUSY.
 * Nonblocking: -EINPROGRESS while the portal worker closes, 0 once inactive. */
int app_netcfg_portal_retry_saved(void);
/* A natural wake may reclaim an automatic portal even when client tracking is
 * unavailable. Manual setup, submitted credentials and transactions are kept. */
int app_netcfg_portal_retry_saved_on_wake(void);
/* Background saved-AP retries must never evict a manually opened portal. */
int app_netcfg_portal_retry_saved_background(void);

#define APP_NETCFG_WIFI_AP_MAX 24
typedef struct {
    char ssid[33];
    int rssi;
    bool secured;
} app_netcfg_wifi_ap_t;

typedef enum {
    APP_NETCFG_WIFI_FRESH = 0,
    APP_NETCFG_WIFI_CACHED,
    APP_NETCFG_WIFI_BUSY,
    APP_NETCFG_WIFI_FAILED,
    APP_NETCFG_WIFI_PAUSED,
} app_netcfg_wifi_list_status_t;

/* Worker-only. Reuses the portal cache while SoftAP is active. Refresh scans
 * only an already enabled, idle STA with sys_wifi stopped; never changes mode
 * or enables a radio. Busy/failed replies may still contain a previous cache. */
int app_netcfg_portal_wifi_list(bool refresh, app_netcfg_wifi_ap_t *out, int max, int *count);

/** 取本机热点名，供 shell 或 UI 显示。缓冲区建议不小于 33 字节。 */
bool app_netcfg_portal_ssid_copy(char *buf, size_t len);

/**
 * 门户页面的地址，形如 "10.23.149.5"。
 *
 * 这个值由 lwip 的 dhcps 决定而不是我们挑的，所以要显示给人看的地方都从这里取，
 * 别在别处再写一遍字面量。
 */
const char *app_netcfg_portal_ip(void);

#ifdef __cplusplus
}
#endif
