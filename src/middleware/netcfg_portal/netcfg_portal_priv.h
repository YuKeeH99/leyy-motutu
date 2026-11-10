#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 手机侧列表放不下更多，缓存也没必要更大。 */
#define NETCFG_PORTAL_SCAN_MAX 24

#define NETCFG_PORTAL_SSID_MAX 32
#define NETCFG_PORTAL_PWD_MAX  64

#define NETCFG_PORTAL_HTTP_PORT 80
#define NETCFG_PORTAL_DNS_PORT  53

/*
 * 门户地址不是我们能挑的。lwip 的 dhcps_start() 会无条件把 AP netif 地址改成
 * GW_ADDR0..3（10.23.149.5），它那句 "use user config" 判断的是一个刚清零的局部
 * 变量，永远为真，所以之前 net_if_set_ip(192.168.4.1) 全被冲掉了，DHCP 也照
 * 10.23.149.100~200 发地址。这里跟着 SDK 走，避免网关和地址池不同网段。
 * 改这几个值必须同时改 lwip/apps/dhcps.h 里的 GW_ADDR*，否则又会对不上。
 */
#define NETCFG_PORTAL_IP_A   10
#define NETCFG_PORTAL_IP_B   23
#define NETCFG_PORTAL_IP_C   149
#define NETCFG_PORTAL_IP_D   5
#define NETCFG_PORTAL_IP_STR "10.23.149.5"

typedef struct {
    char ssid[NETCFG_PORTAL_SSID_MAX + 1];
    char bssid[18];
    int rssi;
    bool secured;
} netcfg_portal_ap_t;

/*
 * app_netcfg_portal.c 提供，HTTP 任务调用。
 */

/** 拷一份扫描缓存出来，返回条数。缓存在切 AP 之前就已经填好。 */
int netcfg_portal_aps_snapshot(netcfg_portal_ap_t *out, int max);

/**
 * 收下手机提交的凭据并唤醒工作任务去连接。
 *
 * pwd 允许为空串（开放网络）。密码只在内存和 KV 里流转，任何路径都不写日志。
 *
 * @return true 已受理，false 参数非法或当前不在配网状态。
 */
bool netcfg_portal_submit(const char *ssid, const char *pwd);

/** 本机热点名，用于页面标题。 */
void netcfg_portal_ssid_get(char *buf, size_t len);

/*
 * netcfg_portal_http.c 提供，工作任务调用。
 */

/** 建 80 端口 HTTP 和 53 端口 DNS 劫持，起服务任务。 */
int netcfg_portal_http_start(void);

/** 关监听并等服务任务退出，返回后不留任何打开的端口。 */
void netcfg_portal_http_stop(void);

/** 内嵌的配网页面，netcfg_portal_page.c 提供。 */
extern const char netcfg_portal_page_html[];

#ifdef __cplusplus
}
#endif
