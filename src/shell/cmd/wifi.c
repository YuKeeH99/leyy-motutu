#include "stdint.h"
#include "stdio.h"
#include "shell.h"
#include "cmd.h"
#include "stddef.h"
#include "string.h"
#include "sys_network_manager.h"
#include "sys_network_diag.h"
#include "sys_wifi.h"
// #include "listen_wifi.h"
#if CONFIG_WIFI_MANAGER
#include "ls_err.h"
#include "ls_wifi_type.h"
#include "ipc/mrpc_wifi_api_client.h"
#include "wifi_manager/wifi_manager.h"
#include "wifi_manager/wifi_manager_storage.h"
#endif

static int wifi_cmd_help(int argc, char **argv);
static int wifi_list(int argc, char **argv);
static int wifi_scan(int argc, char **argv);
static int wifi_status(int argc, char **argv);
static int wifi_diag(int argc, char **argv);

#if CONFIG_WIFI_MANAGER
static int wifi_save_ap_storage_only(wifi_mgr_sta_config_t *sta_cfg)
{
    int ret = wifi_mgr_storage_save_ap(sta_cfg);
    wifi_mgr_ops_t *ops;
    wifi_storage_ctx_t storage_ctx = {0};
    void *mutex;

    if (ret == 0) {
        return 0;
    }

    ops = wifi_mgr_ops_get();
    if (ops == NULL || ops->mem_ops == NULL || ops->os_ops == NULL) {
        return ret;
    }

    mutex = ops->os_ops->mutex_create();
    if (mutex == NULL) {
        return ret;
    }

    wifi_storage_ops_t storage_ops = {
        .malloc = ops->mem_ops->malloc,
        .calloc = ops->mem_ops->calloc,
        .free = ops->mem_ops->free,
        .mutex_lock = ops->os_ops->mutex_lock,
        .mutex_unlock = ops->os_ops->mutex_unlock,
    };

    if (wifi_storage_init(&storage_ctx, &storage_ops, mutex) == 0) {
        ret = wifi_storage_save_ap(&storage_ctx, sta_cfg);
        (void)wifi_storage_deinit(&storage_ctx);
    }

    ops->os_ops->mutex_delete(mutex);
    return ret;
}
#endif

static int wifi_connect(int argc, char **argv)
{
    if (argc < 1 || argc > 2) {
        printf("Usage: wifi connect ssid [password]\n");
        return -1;
    }
    char *ssid = argv[0];
    char *pwd = (argc >= 2) ? argv[1] : NULL;

    if (ssid == NULL || strlen(ssid) == 0) {
        printf("wifi ssid param error\n");
        return -1;
    }
    
    // 检查密码参数（如果提供了密码）
    if (pwd != NULL && strlen(pwd) == 0) {
        printf("wifi password param error\n");
        return -1;
    }

#if CONFIG_WIFI_MANAGER
    wifi_mgr_sta_config_t sta_cfg = {0};
    sys_network_status_t network_status;


    snprintf(sta_cfg.ssid, sizeof(sta_cfg.ssid), "%s", ssid);

    // 如果提供了密码，则设置密码
    if (pwd != NULL) {
        snprintf(sta_cfg.pwd, sizeof(sta_cfg.pwd), "%s", pwd);
    }

    if (sys_network_get_status(&network_status) == 0 &&
        network_status.active_bearer == SYS_NETWORK_BEARER_MODEM) {
        printf("4G network active, save wifi ap only\n");
        int ret = wifi_save_ap_storage_only(&sta_cfg);
        if (ret != 0) {
            printf("wifi save ap error %d", ret);
            return -1;
        }
        return 0;
    }

    int ret = wifi_mgr_sta_connect(&sta_cfg, true);
    if (ret != 0) {
        printf("wifi connect error %d", ret);
        return -1;
    }
#else
    // 对于非wifi_manager版本，如果没有密码则传递空字符串
    int ret = ls_wifi_connect(ssid, pwd ? pwd : "");
    if (ret != 0) {
        printf("wifi connect error %d", ret);
        return -1;
    }
#endif

    return 0;
}
#if CONFIG_WIFI_MANAGER
static int wifi_disconnect(int argc, char **argv)
{
    return wifi_mgr_sta_disconnect(true);
}

static int wifi_auto_connect(int argc, char **argv)
{
    if (argc < 1) {
        printf("invalid param index %d\n", argc);
        return -1;
    }
    uint32_t interval_ms = atoi(argv[0]);

    if (interval_ms <= 0) {
        printf("stop auto connect\n");
        wifi_mgr_auto_connect_stop();
        return 0;
    }

    wifi_mgr_autoconn_config_t cnn_cfg = {
        .interval_ms = interval_ms,
    };
    wifi_mgr_auto_connect_start(&cnn_cfg);

    return 0;
}

/* Diagnostic A/B switch only: keep the boot default and saved AP untouched. */
static int wifi_power_save(int argc, char **argv)
{
    wifi_ps_mode_e mode;
    Shell *shell = shellGetCurrent();
    if (argc != 1 || (strcmp(argv[0], "off") != 0 && strcmp(argv[0], "dtim") != 0)) {
        shellPrint(shell, "Usage: wifi power-save off|dtim (RAM only)\n");
        return -1;
    }
    if (!sys_wifi_is_ready()) {
        shellPrint(shell, "wifi power_save unavailable: stack not ready\n");
        return -1;
    }
    mode = strcmp(argv[0], "off") == 0 ? WIFI_PS_MODE_OFF : WIFI_PS_MODE_DTIM;
    int ret = wifi_ps_mode_set(mode);
    shellPrint(shell, "wifi power_save requested=%s ret=%d (RAM only; reboot restores boot policy)\n",
               argv[0], ret);
    return ret;
}
#endif


static const struct listen_cmd_t g_wifi_cmds[] = {
    {"connect", wifi_connect, "connect wifi, ex: wifi connect ssid [password]"},
#if CONFIG_WIFI_MANAGER
    {"disconnect", wifi_disconnect, "disconnect wifi, ex: wifi disconnect"},
    {"auto", wifi_auto_connect, "auto connect wifi, interval_ms <=0 stop, ex: wifi auto interval_ms"},
    {"power-save", wifi_power_save, "RAM-only WiFi power-save A/B: off|dtim"},
    {"scan", wifi_scan, "scan and list available wifi networks"},
    {"list", wifi_list, "show saved wifi networks"},
#endif
    {"status", wifi_status, "show wifi ready/connected/ip/rssi"},
    {"diag", wifi_diag, "latest 3 network events; diag [before_seq] pages back (RAM only)"},
    {"help", wifi_cmd_help, "show help"},
};

static int wifi_cmd_help(int argc, char **argv)
{
    int cmd_len = sizeof(g_wifi_cmds) / sizeof(g_wifi_cmds[0]);
    for (int i = 0; i < cmd_len; i++) {
        if (g_wifi_cmds[i].help != NULL) {
            printf("%-17s\t:\t%s\n", g_wifi_cmds[i].name, g_wifi_cmds[i].help);
        }
    }

    return 0;
}

static int wifi_diag(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    if (argc < 0 || argc > 1 || (argc == 1 && (argv == NULL || argv[0] == NULL || argv[0][0] == '\0'))) {
        shellPrint(shell, "Usage: wifi diag [before_seq]\n");
        return -1;
    }

    uint32_t before_seq = 0U;
    if (argc == 1) {
        for (const char *p = argv[0]; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9' || before_seq > (UINT32_MAX - (uint32_t)(*p - '0')) / 10U) {
                shellPrint(shell, "Usage: wifi diag [before_seq] (uint32 decimal)\n");
                return -1;
            }
            before_seq = before_seq * 10U + (uint32_t)(*p - '0');
        }
    }

    uint32_t first_seq;
    uint32_t count = sys_network_diag_bounds(&first_seq);
    uint32_t available = argc == 1 ? before_seq - first_seq : count;
    if (available > count) {
        shellPrint(shell, "network history cursor expired/out of range; restart with wifi diag\n");
        return -1;
    }

    /* ADB's 1024-byte TX stream drains in this same task only after the
     * command returns; delays here cannot drain it. Three bounded records
     * plus this header leave room for echo/prompt without changing the SDK. */
    uint32_t page_count = available > 3U ? 3U : available;
    uint32_t page_first = first_seq + available - page_count;
    shellPrint(shell, "network history count=%u page=%u more=%u next_before=%u (wifi diag %u)\n",
               (unsigned)count, (unsigned)page_count, available > page_count ? 1U : 0U,
               (unsigned)page_first, (unsigned)page_first);
    for (uint32_t i = 0U; i < page_count; ++i) {
        sys_network_diag_entry_t entry;
        if (!sys_network_diag_read(page_first + i, &entry)) continue;
        shellPrint(shell, "[NET-HD] seq=%u uptime=%ums domain=%s event=%s value=%d detail=%s\n",
                   (unsigned)entry.seq, (unsigned)entry.uptime_ms,
                   entry.domain, entry.event, entry.value, entry.detail);
    }
    return 0;
}

static int wifi_status(int argc, char **argv)
{
    Shell *shell = shellGetCurrent();
    char ip_buf[24] = {0};
    int rssi = 0;
    bool has_ip = sys_wifi_get_ip_string(ip_buf, sizeof(ip_buf));
    bool has_rssi = sys_wifi_get_signal_quality(&rssi);

    (void)argc;
    (void)argv;

    shellPrint(shell, "wifi ready=%d started=%d connected=%d\n",
           sys_wifi_is_ready() ? 1 : 0,
           sys_wifi_is_started() ? 1 : 0,
           sys_wifi_is_connected() ? 1 : 0);

    if (has_ip) {
        shellPrint(shell, "wifi ip=%s\n", ip_buf);
    } else {
        shellPrint(shell, "wifi ip=<unavailable>\n");
    }

    if (has_rssi) {
        shellPrint(shell, "wifi rssi=%d dBm\n", rssi);
    } else {
        shellPrint(shell, "wifi rssi=<unavailable>\n");
    }

    return 0;
}

#if CONFIG_WIFI_MANAGER
static int wifi_scan(int argc, char **argv)
{
    wifi_mgr_scan_info_t *ap_info = NULL;
    uint32_t size = 32; // Maximum number of APs to scan
    int ret;
    
    // Allocate memory for scan results
    ap_info = malloc(sizeof(wifi_mgr_scan_info_t) * size);
    if (ap_info == NULL) {
        printf("Failed to allocate memory for scan results\n");
        return -1;
    }
    
    // Perform WiFi scan (synchronous)
    printf("Scanning for WiFi networks...\n");
    ret = wifi_mgr_scan_ap(ap_info, size, false);
    
    if (ret < 0) {
        printf("WiFi scan failed with error: %d\n", ret);
        free(ap_info);
        return -1;
    }
    
    // Print scan results
    printf("\nFound %d WiFi networks:\n", ret);
    printf("%-4s %-32s %-18s %-8s %-8s\n", "No.", "SSID", "BSSID", "Channel", "RSSI");
    printf("----------------------------------------------------------------\n");
    
    for (int i = 0; i < ret; i++) {
        printf("%-4d %-32s %02x:%02x:%02x:%02x:%02x:%02x %-8d %-8d\n", 
               i + 1,
               ap_info[i].ssid,
               ap_info[i].bssid[0], ap_info[i].bssid[1], ap_info[i].bssid[2],
               ap_info[i].bssid[3], ap_info[i].bssid[4], ap_info[i].bssid[5],
               ap_info[i].channel,
               ap_info[i].rssi);
    }
    
    // Free allocated memory
    free(ap_info);
    return 0;
}

static int wifi_list(int argc, char **argv)
{
    wifi_mgr_sta_config_t *matched_list = NULL;

    matched_list = psram_malloc(sizeof(wifi_mgr_sta_config_t) * 64);
    if (matched_list == NULL) {
        return -1;
    }

    int ret;

    // 搜索所有已保存的WiFi网络
    ret = wifi_mgr_storage_search_ap(matched_list, 64, SEARCH_ALL, NULL);

    if (ret < 0) {
        printf("Failed to get saved WiFi networks, error: %d\n", ret);
        psram_free(matched_list);
        return -1;
    }
    
    if (ret == 0 || matched_list == NULL) {
        printf("No saved WiFi networks found\n");
        psram_free(matched_list);
        return 0;
    }
    
    // 打印已保存的WiFi网络
    printf("\nFound %d saved WiFi networks:\n", ret);
    printf("%-4s %-32s %-18s %-8s\n", "No.", "SSID", "BSSID", "Channel");
    printf("----------------------------------------------------------\n");
    
    for (int i = 0; i < ret; i++) {
        printf("%-4d %-32s %02x:%02x:%02x:%02x:%02x:%02x %-8d\n", 
               i + 1,
               matched_list[i].ssid,
               matched_list[i].bssid[0], matched_list[i].bssid[1], matched_list[i].bssid[2],
               matched_list[i].bssid[3], matched_list[i].bssid[4], matched_list[i].bssid[5],
               matched_list[i].channel);
    }

    psram_free(matched_list);

    return 0;
}
#endif

static int wifi_cmd_handler(int argc, char **argv)
{
    if (argc == 1) {
        wifi_cmd_help(argc, argv);
        return 0;
    }

    int i;

    for (i = 0; i < sizeof(g_wifi_cmds) / sizeof(g_wifi_cmds[0]); i++) {
        if (strcmp(g_wifi_cmds[i].name, argv[1]) == 0) {
            return g_wifi_cmds[i].exec(argc - 2, argv + 2);
        }
    }

    wifi_cmd_help(argc, argv);

    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, wifi,
                 wifi_cmd_handler, wifi cmd group);
