#define TAG "netcfg-portal-http"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/sockets.h"

#include "lisa_log.h"
#include "netcfg_portal_priv.h"

/* 一次请求读多少。表单只有 ssid + pwd，1.5K 足够，超出的直接丢。 */
#define REQ_BUF_SZ         1536
#define DNS_BUF_SZ         512
#define APS_JSON_BUF_SZ    (NETCFG_PORTAL_SCAN_MAX * (NETCFG_PORTAL_SSID_MAX * 2 + 48) + 32)
#define FORM_BODY_MAX      512
#define REQUEST_TIMEOUT_MS 5000
/* lwIP recv/send + HTTP 处理实测峰值约 4.7K，4K 会溢出并破坏 accept 任务。 */
#define HTTP_TASK_STACK    8192
#define DNS_TASK_STACK     4096
#define HTTP_TASK_PRIO     5

static TaskHandle_t s_task;
static volatile bool s_run;
static volatile bool s_exited;
static int s_listen_fd = -1;
static int s_dns_fd = -1;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_exited;

static const char HDR_HTML[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                               "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %u\r\n\r\n";
static const char HDR_JSON[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                               "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %u\r\n\r\n";
static const char HDR_JSON_BAD_REQUEST[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                                           "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %u\r\n\r\n";
static const char HDR_JSON_TOO_LARGE[] = "HTTP/1.1 413 Content Too Large\r\nContent-Type: application/json\r\n"
                                         "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: %u\r\n\r\n";
/* 302 是让 Android / iOS 弹出"登录网络"的关键，不能改成 200。 */
static const char REDIRECT[] = "HTTP/1.1 302 Found\r\nLocation: http://" NETCFG_PORTAL_IP_STR "/\r\n"
                               "Cache-Control: no-store\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

/* 普通 memset 可能被优化器删除；HTTP 请求体包含密码，必须可靠清理。 */
static void secure_zero(void *buf, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;

    while (len-- > 0) {
        *p++ = 0;
    }
}

static bool send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        int n = lwip_send(fd, buf + off, len - off, 0);

        if (n <= 0) {
            LISA_LOGW(TAG, "send fd=%d failed at %u/%u, errno=%d", fd, (unsigned)off, (unsigned)len, errno);
            return false;
        }
        off += (size_t)n;
    }

    return true;
}

static void send_body(int fd, const char *hdr_fmt, const char *body)
{
    char hdr[128];
    size_t body_len = strlen(body);
    int hdr_len = snprintf(hdr, sizeof(hdr), hdr_fmt, (unsigned)body_len);

    if (hdr_len > 0 && (size_t)hdr_len < sizeof(hdr)) {
        if (send_all(fd, hdr, (size_t)hdr_len)) {
            send_all(fd, body, body_len);
        }
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

enum {
    FORM_INVALID = -1,
    FORM_MISSING = 0,
    FORM_OK = 1,
};

/**
 * 边解码边写入，限制的是 WiFi 真实字节长度，不是 %XX 编码后的长度。
 */
static int form_value_decode(const char *val, char *out, size_t out_sz)
{
    size_t w = 0;

    while (*val != '\0' && *val != '&') {
        unsigned char decoded;

        if (*val == '+') {
            decoded = ' ';
            val++;
        } else if (*val == '%' && val[1] != '\0' && val[2] != '\0') {
            int hi = hex_value(val[1]);
            int lo = hex_value(val[2]);

            if (hi >= 0 && lo >= 0) {
                decoded = (unsigned char)((hi << 4) | lo);
                val += 3;
            } else {
                decoded = (unsigned char)*val++;
            }
        } else {
            decoded = (unsigned char)*val++;
        }

        if (decoded == '\0' || w + 1 >= out_sz) {
            out[0] = '\0';
            return FORM_INVALID;
        }
        out[w++] = (char)decoded;
    }

    out[w] = '\0';
    return FORM_OK;
}

/**
 * 从 application/x-www-form-urlencoded 里取一个字段。
 *
 * 找的是 "key=" 且它必须位于串首或 '&' 之后，否则 "pwd" 会先匹配到 "xpwd"。
 */
static int form_field(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t key_len = strlen(key);
    const char *p = body;

    out[0] = '\0';

    while ((p = strstr(p, key)) != NULL) {
        bool at_start = (p == body) || (p[-1] == '&');

        if (at_start && p[key_len] == '=') {
            const char *val = p + key_len + 1;

            return form_value_decode(val, out, out_sz);
        }
        p += key_len;
    }
    return FORM_MISSING;
}

static bool request_is(const char *request, const char *method, const char *path)
{
    size_t method_len = strlen(method);
    size_t path_len = strlen(path);

    return strncmp(request, method, method_len) == 0 && request[method_len] == ' ' &&
           strncmp(request + method_len + 1, path, path_len) == 0 && request[method_len + path_len + 1] == ' ';
}

static bool ascii_equal_nocase(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];

        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }

    return true;
}

/**
 * 严格解析唯一一个 Content-Length。重复字段和超出表单上限都拒绝，避免
 * 截断请求或 HTTP request smuggling 把错误凭据交给 Wi-Fi 状态机。
 */
static int content_length_get(const char *request, const char *headers_end, size_t *length)
{
    static const char name[] = "Content-Length";
    const char *line = strstr(request, "\r\n");
    bool found = false;
    size_t value = 0;

    if (line == NULL || line >= headers_end || length == NULL) {
        return -EINVAL;
    }

    line += 2;
    while (line < headers_end) {
        const char *eol = strstr(line, "\r\n");
        const char *colon;

        if (eol == NULL || eol > headers_end) {
            return -EINVAL;
        }

        colon = memchr(line, ':', (size_t)(eol - line));
        if (colon != NULL && (size_t)(colon - line) == sizeof(name) - 1 &&
            ascii_equal_nocase(line, name, sizeof(name) - 1)) {
            const char *p = colon + 1;
            bool have_digit = false;

            if (found) {
                return -EINVAL;
            }
            found = true;
            value = 0;

            while (p < eol && (*p == ' ' || *p == '\t')) {
                p++;
            }
            while (p < eol && *p >= '0' && *p <= '9') {
                unsigned int digit = (unsigned int)(*p - '0');

                have_digit = true;
                if (value > (FORM_BODY_MAX - digit) / 10U) {
                    return -E2BIG;
                }
                value = value * 10U + digit;
                p++;
            }
            while (p < eol && (*p == ' ' || *p == '\t')) {
                p++;
            }
            if (!have_digit || p != eol) {
                return -EINVAL;
            }
        }

        line = eol + 2;
    }

    if (!found) {
        return -ENOENT;
    }

    *length = value;
    return 0;
}

static void handle_aps(int fd)
{
    static netcfg_portal_ap_t aps[NETCFG_PORTAL_SCAN_MAX];
    /* HTTP 任务单线程串行服务，放到 BSS 避免扫描列表占用任务栈。 */
    static char json[APS_JSON_BUF_SZ];
    int n = netcfg_portal_aps_snapshot(aps, NETCFG_PORTAL_SCAN_MAX);
    int off = snprintf(json, sizeof(json), "{\"aps\":[");

    for (int i = 0; i < n && off > 0 && (size_t)off < sizeof(json); i++) {
        char ssid[NETCFG_PORTAL_SSID_MAX * 2 + 1];
        size_t w = 0;

        /* SSID 进 JSON 前转义引号和反斜杠，控制字符直接丢。 */
        for (const char *c = aps[i].ssid; *c && w + 2 < sizeof(ssid); c++) {
            if (*c == '"' || *c == '\\') {
                ssid[w++] = '\\';
            } else if ((unsigned char)*c < 0x20) {
                continue;
            }
            ssid[w++] = *c;
        }
        ssid[w] = '\0';

        off += snprintf(json + off, sizeof(json) - (size_t)off, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":%d}",
                        i ? "," : "", ssid, aps[i].rssi, aps[i].secured ? 1 : 0);
    }

    if (off > 0 && (size_t)off < sizeof(json)) {
        snprintf(json + off, sizeof(json) - (size_t)off, "]}");
        send_body(fd, HDR_JSON, json);
    }
}

static void handle_info(int fd)
{
    char ssid[NETCFG_PORTAL_SSID_MAX + 1];
    char json[NETCFG_PORTAL_SSID_MAX + 32];

    netcfg_portal_ssid_get(ssid, sizeof(ssid));
    snprintf(json, sizeof(json), "{\"ssid\":\"%s\"}", ssid);
    send_body(fd, HDR_JSON, json);
}

static void handle_connect(int fd, const char *body)
{
    char ssid[NETCFG_PORTAL_SSID_MAX + 1] = {0};
    char pwd[NETCFG_PORTAL_PWD_MAX + 1] = {0};
    int ssid_result = body ? form_field(body, "ssid", ssid, sizeof(ssid)) : FORM_MISSING;

    if (ssid_result == FORM_INVALID) {
        send_body(fd, HDR_JSON, "{\"ok\":false,\"err\":\"网络名称过长或格式无效\"}");
        goto out;
    }
    if (ssid_result == FORM_MISSING || ssid[0] == '\0') {
        send_body(fd, HDR_JSON, "{\"ok\":false,\"err\":\"网络名称为空\"}");
        goto out;
    }

    /* 开放网络没有 pwd 字段，缺字段不是错误。 */
    int pwd_result = form_field(body, "pwd", pwd, sizeof(pwd));

    if (pwd_result == FORM_INVALID) {
        send_body(fd, HDR_JSON, "{\"ok\":false,\"err\":\"密码过长或格式无效\"}");
        goto out;
    }
    if (pwd_result == FORM_MISSING) {
        pwd[0] = '\0';
    }

    /* 只记 SSID。密码不进日志、不进串口。 */
    LISA_LOGI(TAG, "credentials received for \"%s\"", ssid);

    if (!netcfg_portal_submit(ssid, pwd)) {
        send_body(fd, HDR_JSON, "{\"ok\":false,\"err\":\"设备未处于配网状态\"}");
        goto out;
    }

    /* 先把响应发完再让上层关 AP，否则手机看到的是提交失败。 */
    send_body(fd, HDR_JSON, "{\"ok\":true}");

out:
    secure_zero(pwd, sizeof(pwd));
}

static void serve(int fd)
{
    char buf[REQ_BUF_SZ] = {0};
    int len = 0;
    bool too_large = false;
    TickType_t started = xTaskGetTickCount();
    char *headers_end = NULL;
    size_t content_length = 0;
    size_t expected_length = 0;

    while (len < (int)sizeof(buf) - 1 &&
           (TickType_t)(xTaskGetTickCount() - started) < pdMS_TO_TICKS(REQUEST_TIMEOUT_MS)) {
        int n = lwip_recv(fd, buf + len, sizeof(buf) - 1 - (size_t)len, 0);

        if (n <= 0) {
            if (n < 0 && len == 0) {
                LISA_LOGW(TAG, "recv fd=%d failed, errno=%d", fd, errno);
            }
            break;
        }
        len += n;
        buf[len] = '\0';

        if (memchr(buf, '\0', (size_t)len) != NULL) {
            break;
        }

        headers_end = strstr(buf, "\r\n\r\n");
        if (headers_end != NULL) {
            if (request_is(buf, "POST", "/api/connect")) {
                int cl_ret = content_length_get(buf, headers_end, &content_length);
                size_t body_offset = (size_t)(headers_end + 4 - buf);

                if (cl_ret == -E2BIG || body_offset > sizeof(buf) - 1 - content_length) {
                    too_large = true;
                    break;
                }
                if (cl_ret != 0) {
                    break;
                }

                expected_length = body_offset + content_length;
                if ((size_t)len >= expected_length) {
                    buf[expected_length] = '\0';
                    break;
                }
            } else {
                break;
            }
        }
    }

    if (len <= 0) {
        goto out;
    }
    buf[len] = '\0';

    if (too_large) {
        send_body(fd, HDR_JSON_TOO_LARGE, "{\"ok\":false,\"err\":\"请求过大\"}");
        goto out;
    }

    headers_end = strstr(buf, "\r\n\r\n");
    if (headers_end == NULL) {
        send_body(fd, HDR_JSON_BAD_REQUEST, "{\"ok\":false,\"err\":\"请求格式无效\"}");
        goto out;
    }

    if (request_is(buf, "POST", "/api/connect")) {
        char *body = headers_end + 4;

        if (expected_length == 0 || (size_t)len < expected_length) {
            send_body(fd, HDR_JSON_BAD_REQUEST, "{\"ok\":false,\"err\":\"请求体不完整\"}");
            goto out;
        }
        LISA_LOGI(TAG, "HTTP POST /api/connect (%d bytes)", len);
        handle_connect(fd, body);
    } else if (request_is(buf, "GET", "/api/aps")) {
        LISA_LOGI(TAG, "HTTP GET /api/aps (%d bytes)", len);
        handle_aps(fd);
    } else if (request_is(buf, "GET", "/api/info")) {
        LISA_LOGI(TAG, "HTTP GET /api/info (%d bytes)", len);
        handle_info(fd);
    } else if (request_is(buf, "GET", "/") || request_is(buf, "GET", "/index.html")) {
        LISA_LOGI(TAG, "HTTP GET portal page (%d bytes)", len);
        send_body(fd, HDR_HTML, netcfg_portal_page_html);
    } else {
        /* 系统探测 URL 和其它任何路径都跳回首页，这就是自动弹窗的来源。 */
        LISA_LOGI(TAG, "HTTP captive probe redirected (%d bytes)", len);
        send_all(fd, REDIRECT, sizeof(REDIRECT) - 1);
    }

out:
    secure_zero(buf, sizeof(buf));
}

/**
 * 把任何域名都答成板子的地址。
 *
 * 只处理单问题的 A 查询，其它类型原样返回空回答，够触发门户弹窗。
 */
static void serve_dns(int fd)
{
    uint8_t buf[DNS_BUF_SZ];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = lwip_recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);

    if (n < 12 || (buf[2] & 0x80) != 0) {
        return;
    }

    uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);

    if (qd != 1) {
        return;
    }

    /* 跳过 QNAME 的 label 链，再读取 QTYPE + QCLASS。 */
    int p = 12;

    while (p < n && buf[p] != 0) {
        uint8_t label_len = buf[p];

        if ((label_len & 0xC0) != 0 || label_len > 63 || p + 1 + label_len >= n) {
            return;
        }
        p += label_len + 1;
    }

    if (p >= n || p + 5 > n) {
        return;
    }

    uint16_t qtype = (uint16_t)((buf[p + 1] << 8) | buf[p + 2]);
    uint16_t qclass = (uint16_t)((buf[p + 3] << 8) | buf[p + 4]);
    p += 5;

    /* 权威应答但不宣称递归能力；保留请求里的 RD 位。 */
    buf[2] = (uint8_t)(0x84 | (buf[2] & 0x01));
    buf[3] = 0x00;
    buf[6] = 0;
    buf[7] = 0; /* ANCOUNT，A/IN 时再置 1 */
    buf[8] = 0;
    buf[9] = 0; /* NSCOUNT */
    buf[10] = 0;
    buf[11] = 0; /* ARCOUNT */

    /* AAAA 等其它问题返回 NOERROR/NODATA，不能伪装成类型不匹配的 A 记录。 */
    if (qtype != 1 || qclass != 1) {
        int sent = lwip_sendto(fd, buf, p, 0, (struct sockaddr *)&from, from_len);

        if (sent != p) {
            LISA_LOGW(TAG, "DNS empty reply fd=%d failed, ret=%d errno=%d", fd, sent, errno);
        }
        return;
    }

    if (p + 16 > (int)sizeof(buf)) {
        return;
    }

    buf[7] = 1;

    uint8_t *a = buf + p;

    a[0] = 0xC0;
    a[1] = 0x0C; /* 指回问题里的域名 */
    a[2] = 0x00;
    a[3] = 0x01; /* TYPE A */
    a[4] = 0x00;
    a[5] = 0x01; /* CLASS IN */
    a[6] = 0;
    a[7] = 0;
    a[8] = 0;
    a[9] = 30; /* TTL 30s */
    a[10] = 0x00;
    a[11] = 0x04; /* RDLENGTH */
    a[12] = NETCFG_PORTAL_IP_A;
    a[13] = NETCFG_PORTAL_IP_B;
    a[14] = NETCFG_PORTAL_IP_C;
    a[15] = NETCFG_PORTAL_IP_D;

    int sent = lwip_sendto(fd, buf, p + 16, 0, (struct sockaddr *)&from, from_len);

    if (sent != p + 16) {
        LISA_LOGW(TAG, "DNS reply fd=%d failed, ret=%d errno=%d", fd, sent, errno);
    }
}

/*
 * 门户故意直接调用 lwip_*，不经过 SAL：sys_wifi_stop() 会注销 STA 使用的 wifi0
 * SAL netdev，但 SoftAP 的 lwIP netif 仍然有效。这样只保留一套描述符，也不需要为了
 * HTTP/DNS 把已经停掉的 STA 抽象层重新注册回来。
 *
 * HTTP 和 DNS 各用一个阻塞任务；SO_RCVTIMEO 只负责让 stop 时能定期检查 s_run。
 */
static int set_rx_timeout(int fd, int sec)
{
    struct timeval tv = {.tv_sec = sec, .tv_usec = 0};
    int ret = lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (ret != 0) {
        LISA_LOGE(TAG, "set SO_RCVTIMEO fd=%d failed, errno=%d", fd, errno);
    }

    return ret;
}

static void http_task(void *arg)
{
    (void)arg;
    LISA_LOGI(TAG, "HTTP task started, listen fd=%d stack=%u bytes", s_listen_fd, HTTP_TASK_STACK);

    while (s_run) {
        int fd = lwip_accept(s_listen_fd, NULL, NULL);

        if (fd < 0) {
            int err = errno;

            /* EAGAIN/EWOULDBLOCK 是 SO_RCVTIMEO 每秒唤醒以检查 stop 的正常路径。 */
            if (err != EAGAIN && err != EWOULDBLOCK) {
                LISA_LOGW(TAG, "HTTP accept failed, errno=%d", err);
            }
            continue; /* 多数是 accept 超时，回头再看 s_run */
        }

        LISA_LOGI(TAG, "HTTP client accepted, fd=%d", fd);
        if (set_rx_timeout(fd, 3) == 0) {
            serve(fd);
        }
        lwip_close(fd);
    }

    LISA_LOGI(TAG, "HTTP task exiting, stack_free=%u",
              (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    s_exited = true;
    vTaskDelete(NULL);
}

static void dns_task(void *arg)
{
    (void)arg;
    LISA_LOGI(TAG, "DNS task started, fd=%d stack=%u bytes", s_dns_fd, DNS_TASK_STACK);

    while (s_run) {
        serve_dns(s_dns_fd);
    }

    s_dns_exited = true;
    vTaskDelete(NULL);
}

static int open_listen(void)
{
    struct sockaddr_in addr = {0};
    int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    if (fd < 0) {
        LISA_LOGE(TAG, "create HTTP socket failed, errno=%d", errno);
        return -1;
    }

    if (lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        LISA_LOGW(TAG, "set HTTP SO_REUSEADDR failed, errno=%d", errno);
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(NETCFG_PORTAL_HTTP_PORT);

    if (lwip_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LISA_LOGE(TAG, "bind HTTP port %d failed, errno=%d", NETCFG_PORTAL_HTTP_PORT, errno);
        lwip_close(fd);
        return -1;
    }
    if (lwip_listen(fd, 4) != 0) {
        LISA_LOGE(TAG, "listen HTTP port %d failed, errno=%d", NETCFG_PORTAL_HTTP_PORT, errno);
        lwip_close(fd);
        return -1;
    }

    /* accept 也受 SO_RCVTIMEO 约束，停门户时才不会卡在这儿。 */
    if (set_rx_timeout(fd, 1) != 0) {
        lwip_close(fd);
        return -1;
    }
    return fd;
}

static int open_dns(void)
{
    struct sockaddr_in addr = {0};
    int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1;

    if (fd < 0) {
        LISA_LOGE(TAG, "create DNS socket failed, errno=%d", errno);
        return -1;
    }

    if (lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        LISA_LOGW(TAG, "set DNS SO_REUSEADDR failed, errno=%d", errno);
    }

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(NETCFG_PORTAL_DNS_PORT);

    if (lwip_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LISA_LOGE(TAG, "bind DNS port %d failed, errno=%d", NETCFG_PORTAL_DNS_PORT, errno);
        lwip_close(fd);
        return -1;
    }

    if (set_rx_timeout(fd, 1) != 0) {
        lwip_close(fd);
        return -1;
    }
    return fd;
}

int netcfg_portal_http_start(void)
{
    if (s_task != NULL) {
        return -1;
    }

    s_listen_fd = open_listen();
    if (s_listen_fd < 0) {
        LISA_LOGE(TAG, "open HTTP listener failed");
        return -1;
    }

    /* DNS 劫持只影响自动弹窗，失败也让门户地址能手输访问。 */
    s_dns_fd = open_dns();
    if (s_dns_fd < 0) {
        LISA_LOGW(TAG, "DNS hijack unavailable, manual " NETCFG_PORTAL_IP_STR " still works");
    }

    s_run = true;
    s_exited = false;
    s_dns_exited = true; /* 没起 DNS 任务时，stop 不该等它 */

    if (xTaskCreate(http_task, "netcfg_http", HTTP_TASK_STACK / sizeof(StackType_t), NULL, HTTP_TASK_PRIO, &s_task) !=
        pdPASS) {
        s_run = false;
        lwip_close(s_listen_fd);
        s_listen_fd = -1;
        if (s_dns_fd >= 0) {
            lwip_close(s_dns_fd);
            s_dns_fd = -1;
        }
        s_task = NULL;
        LISA_LOGE(TAG, "create http task failed");
        return -1;
    }

    if (s_dns_fd >= 0) {
        s_dns_exited = false;
        if (xTaskCreate(dns_task, "netcfg_dns", DNS_TASK_STACK / sizeof(StackType_t), NULL, HTTP_TASK_PRIO,
                        &s_dns_task) != pdPASS) {
            /* 丢了自动弹窗而已，门户本身照常服务，不算启动失败。 */
            s_dns_exited = true;
            s_dns_task = NULL;
            LISA_LOGW(TAG, "create dns task failed, auto-popup unavailable");
        }
    }

    LISA_LOGI(TAG, "portal listening on http://" NETCFG_PORTAL_IP_STR "/ (lwip http_fd=%d dns_fd=%d)", s_listen_fd,
              s_dns_fd);
    return 0;
}

void netcfg_portal_http_stop(void)
{
    if (s_task == NULL) {
        return;
    }

    s_run = false;

    /* 收发都是 1s 超时，给足退出时间再回收 fd，避免任务摸到已关的句柄。 */
    for (int i = 0; i < 30 && (!s_exited || !s_dns_exited); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!s_exited || !s_dns_exited) {
        LISA_LOGW(TAG, "portal task did not exit in time");
    }

    if (s_listen_fd >= 0) {
        lwip_close(s_listen_fd);
        s_listen_fd = -1;
    }
    if (s_dns_fd >= 0) {
        lwip_close(s_dns_fd);
        s_dns_fd = -1;
    }

    s_task = NULL;
    s_dns_task = NULL;
    LISA_LOGI(TAG, "portal stopped, no listener left");
}
