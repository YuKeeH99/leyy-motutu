#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/sockets.h"
#include "sysheap.h"

#define TAG "motutu-web"
#include "lisa_log.h"

#include "motutu_motion.h"
#include "service_motutu_web.h"
#include "sys_wifi.h"

#ifdef CONFIG_APPLICATION_UI
#include "../../../apps-ui/apps/llm/lisa_ui_nav_scr_ids.h"
#include "../../../apps-ui/apps/llm/models/model_qrcode.h"
#include "../../../apps-ui/lisa_ui_nav_scr.h"
#include "../../../apps-ui/portable/arcs/lisa_ui_invoke.h"
#endif

#define MOTUTU_WEB_PORT            8080
#define MOTUTU_WEB_STACK_SIZE      4096
#define MOTUTU_WEB_TASK_PRIORITY   5
#define MOTUTU_WEB_BACKLOG         2
#define MOTUTU_WEB_REQ_BUF_SIZE    24576
#define MOTUTU_WEB_BODY_MAX        16384

static TaskHandle_t s_motutu_web_task = NULL;
static bool s_motutu_web_inited = false;

static bool motutu_web_get_ip_text(char *buf, size_t len)
{
    return sys_wifi_get_ip_string(buf, len);
}

bool service_motutu_web_get_control_url(char *buf, size_t len)
{
    char ip_buf[24];
    int ret = 0;

    if (buf == NULL || len == 0U || !motutu_web_get_ip_text(ip_buf, sizeof(ip_buf))) {
        return false;
    }

    ret = snprintf(buf, len, "http://%s:%u", ip_buf, (unsigned int)MOTUTU_WEB_PORT);
    return ret > 0 && (size_t)ret < len;
}

int service_motutu_web_show_control_page(void)
{
    char ip_buf[24];
    char url_buf[48];

    if (!motutu_web_get_ip_text(ip_buf, sizeof(ip_buf))) {
        LOGW("motutu web show page failed: wifi ip unavailable");
        return -1;
    }

    if (!service_motutu_web_get_control_url(url_buf, sizeof(url_buf))) {
        LOGW("motutu web show page failed: control url unavailable");
        return -1;
    }

#ifndef CONFIG_APPLICATION_UI
    LOGI("motutu control page: %s", url_buf);
    return 0;
#else
    struct motutu_web_info_payload {
        qrcode_status_t status;
        char top_text[64];
        char bottom_text[192];
    } payload;
    struct motutu_web_info_payload *payload_ptr = &payload;

    memset(&payload, 0, sizeof(payload));
    payload.status = QR_STATUS_CUSTOM_INFO;
    snprintf(payload.top_text, sizeof(payload.top_text), "Motutu 控制页");
    snprintf(payload.bottom_text,
             sizeof(payload.bottom_text),
             "IP: %s\n%s",
             ip_buf,
             url_buf);

    LISA_UI_INVOKE_UI_ARG_PTR(payload_ptr, sizeof(payload), {
        model_qrcode_set_text(_invoke_payload_ptr->top_text, _invoke_payload_ptr->bottom_text);
        model_qrcode_set_status(_invoke_payload_ptr->status);

        if (lisa_ui_nav_scr_get_top_id() == LISA_UI_NAV_SCR_ID_INFO) {
            lisa_ui_nav_scr_nav_back();
        }
        lisa_ui_nav_scr_nav_to(LISA_UI_NAV_SCR_ID_INFO);
    });

    return 0;
#endif
}

static const char s_motutu_index_html[] =
#include "service_motutu_web_page.inc"
;

static void motutu_web_send_all(int sock, const char *buf, size_t len)
{
    while (len > 0U) {
        int sent = send(sock, buf, (int)len, 0);

        if (sent <= 0) {
            return;
        }
        buf += sent;
        len -= (size_t)sent;
    }
}

static void motutu_web_send_response(int sock,
                                     const char *status,
                                     const char *content_type,
                                     const char *body)
{
    char header[256];
    int header_len;
    size_t body_len = body ? strlen(body) : 0U;

    header_len = snprintf(header,
                          sizeof(header),
                          "HTTP/1.1 %s\r\n"
                          "Content-Type: %s; charset=utf-8\r\n"
                          "Content-Length: %u\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          status,
                          content_type,
                          (unsigned int)body_len);
    if (header_len < 0) {
        return;
    }

    motutu_web_send_all(sock, header, (size_t)header_len);
    if (body_len > 0U) {
        motutu_web_send_all(sock, body, body_len);
    }
}

static void motutu_web_send_json(int sock, const char *status, const char *json)
{
    motutu_web_send_response(sock, status, "application/json", json);
}

static bool motutu_web_append_json(char *buf, size_t len, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int written;

    if (buf == NULL || used == NULL || *used >= len) {
        return false;
    }

    va_start(ap, fmt);
    written = vsnprintf(buf + *used, len - *used, fmt, ap);
    va_end(ap);

    if (written < 0 || (size_t)written >= len - *used) {
        return false;
    }

    *used += (size_t)written;
    return true;
}

static void motutu_web_send_text(int sock, const char *status, const char *text)
{
    motutu_web_send_response(sock, status, "text/plain", text);
}

static const char *motutu_web_find_header_end(const char *buf)
{
    const char *pos = strstr(buf, "\r\n\r\n");

    if (pos != NULL) {
        return pos + 4;
    }
    return NULL;
}

static int motutu_web_parse_content_length(const char *buf)
{
    const char *p = strstr(buf, "Content-Length:");

    if (p == NULL) {
        p = strstr(buf, "content-length:");
    }
    if (p == NULL) {
        return 0;
    }

    p = strchr(p, ':');
    if (p == NULL) {
        return 0;
    }
    ++p;
    while (*p == ' ') {
        ++p;
    }
    return atoi(p);
}

static int motutu_web_recv_request(int client, char *buf, size_t buf_size, int *out_body_len)
{
    size_t used = 0;
    int content_length = 0;
    const char *body = NULL;

    if (buf == NULL || out_body_len == NULL || buf_size < 8U) {
        return -1;
    }

    *out_body_len = 0;
    while (used + 1U < buf_size) {
        int ret = recv(client, &buf[used], (int)(buf_size - used - 1U), 0);

        if (ret <= 0) {
            return -1;
        }
        used += (size_t)ret;
        buf[used] = '\0';

        body = motutu_web_find_header_end(buf);
        if (body == NULL) {
            continue;
        }

        content_length = motutu_web_parse_content_length(buf);
        if (content_length < 0 || content_length > MOTUTU_WEB_BODY_MAX) {
            return -1;
        }

        if ((size_t)(body - buf) + (size_t)content_length <= used) {
            *out_body_len = content_length;
            return 0;
        }
    }

    return -1;
}

static bool motutu_web_parse_request_line(const char *buf,
                                          char *method,
                                          size_t method_size,
                                          char *path,
                                          size_t path_size)
{
    const char *line_end;
    char line[128];
    int ret;

    if (buf == NULL || method == NULL || path == NULL) {
        return false;
    }

    line_end = strstr(buf, "\r\n");
    if (line_end == NULL || (size_t)(line_end - buf) >= sizeof(line)) {
        return false;
    }

    memcpy(line, buf, (size_t)(line_end - buf));
    line[line_end - buf] = '\0';
    ret = sscanf(line, "%15s %63s", method, path);
    return ret == 2;
}

static void motutu_web_handle_status(int client)
{
    motutu_pose_t pose = {0};
    uint16_t abs_deg[4] = {0};
    char ip_buf[24] = {0};
    char url_buf[48] = {0};
    char *json_buf;
    size_t used = 0U;
    bool has_ip;
    bool has_url;
    int ready;

    json_buf = (char *)psram_malloc(512U);
    if (json_buf == NULL) {
        motutu_web_send_json(client, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"oom\"}");
        return;
    }

    ready = motutu_motion_is_ready() ? 1 : 0;
    has_ip = motutu_web_get_ip_text(ip_buf, sizeof(ip_buf));
    has_url = service_motutu_web_get_control_url(url_buf, sizeof(url_buf));
    (void)motutu_get_current_pose(&pose);
    (void)motutu_get_current_abs_pose(abs_deg);

    if (!motutu_web_append_json(json_buf, 512U, &used, "{\"ready\":%s,\"port\":%u",
                                ready ? "true" : "false",
                                (unsigned int)MOTUTU_WEB_PORT) ||
        (has_ip && !motutu_web_append_json(json_buf, 512U, &used, ",\"ip\":\"%s\"", ip_buf)) ||
        (has_url && !motutu_web_append_json(json_buf, 512U, &used, ",\"url\":\"%s\"", url_buf)) ||
        !motutu_web_append_json(json_buf, 512U, &used,
                                ",\"current\":{\"logical\":{\"lf\":%d,\"rf\":%d,\"lr\":%d,\"rr\":%d,\"hold_ms\":%u},"
                                "\"abs\":{\"lf\":%u,\"rf\":%u,\"lr\":%u,\"rr\":%u}}}",
                                pose.lf_deg,
                                pose.rf_deg,
                                pose.lr_deg,
                                pose.rr_deg,
                                (unsigned int)pose.hold_ms,
                                (unsigned int)abs_deg[0],
                                (unsigned int)abs_deg[1],
                                (unsigned int)abs_deg[2],
                                (unsigned int)abs_deg[3])) {
        psram_free(json_buf);
        motutu_web_send_json(client, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"json build failed\"}");
        return;
    }

    motutu_web_send_json(client, "200 OK", json_buf);
    psram_free(json_buf);
}

static void motutu_web_handle_meta(int client)
{
    static const char *const joint_keys[4] = {"lf", "rf", "lr", "rr"};
    motutu_pose_t pose = {0};
    uint16_t abs_deg[4] = {0};
    char ip_buf[24] = {0};
    char url_buf[48] = {0};
    char *json_buf;
    size_t used = 0U;
    bool has_ip;
    bool has_url;
    int ready;
    uint8_t i = 0;

    json_buf = (char *)psram_malloc(1536U);
    if (json_buf == NULL) {
        motutu_web_send_json(client, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"oom\"}");
        return;
    }

    ready = motutu_motion_is_ready() ? 1 : 0;
    has_ip = motutu_web_get_ip_text(ip_buf, sizeof(ip_buf));
    has_url = service_motutu_web_get_control_url(url_buf, sizeof(url_buf));
    (void)motutu_get_current_pose(&pose);
    (void)motutu_get_current_abs_pose(abs_deg);

    if (!motutu_web_append_json(json_buf, 1536U, &used, "{\"ready\":%s,\"port\":%u,\"sequence_key\":\"a\",\"sequence_body_max\":%u",
                                ready ? "true" : "false",
                                (unsigned int)MOTUTU_WEB_PORT,
                                (unsigned int)MOTUTU_WEB_BODY_MAX) ||
        (has_ip && !motutu_web_append_json(json_buf, 1536U, &used, ",\"ip\":\"%s\"", ip_buf)) ||
        (has_url && !motutu_web_append_json(json_buf, 1536U, &used, ",\"url\":\"%s\"", url_buf)) ||
        !motutu_web_append_json(json_buf, 1536U, &used,
                                ",\"current\":{\"logical\":{\"lf\":%d,\"rf\":%d,\"lr\":%d,\"rr\":%d},"
                                "\"abs\":{\"lf\":%u,\"rf\":%u,\"lr\":%u,\"rr\":%u}},\"joints\":[",
                                pose.lf_deg,
                                pose.rf_deg,
                                pose.lr_deg,
                                pose.rr_deg,
                                (unsigned int)abs_deg[0],
                                (unsigned int)abs_deg[1],
                                (unsigned int)abs_deg[2],
                                (unsigned int)abs_deg[3])) {
        psram_free(json_buf);
        motutu_web_send_json(client, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"json build failed\"}");
        return;
    }

    for (i = 0; i < 4U; ++i) {
        motutu_joint_info_t info;
        int min_abs;
        int max_abs;

        if (motutu_get_joint_info(i, &info) != 0) {
            continue;
        }

        min_abs = (int)info.center_abs_deg + info.dir * info.angle_min_deg;
        max_abs = (int)info.center_abs_deg + info.dir * info.angle_max_deg;
        if (min_abs > max_abs) {
            int tmp = min_abs;
            min_abs = max_abs;
            max_abs = tmp;
        }
        if (min_abs < 0) {
            min_abs = 0;
        }
        if (max_abs > 270) {
            max_abs = 270;
        }

        if ((i > 0U && !motutu_web_append_json(json_buf, 1536U, &used, ",")) ||
            !motutu_web_append_json(json_buf, 1536U, &used,
                                    "{\"key\":\"%s\",\"index\":%u,\"id\":%u,\"dir\":%d,\"center_abs_deg\":%u,"
                                    "\"min_abs_deg\":%d,\"max_abs_deg\":%d,\"min_logical_deg\":%d,"
                                    "\"max_logical_deg\":%d,\"center_us\":%u,\"min_us\":%u,\"max_us\":%u}",
                                    joint_keys[i],
                                    (unsigned int)i,
                                    (unsigned int)info.id,
                                    info.dir,
                                    (unsigned int)info.center_abs_deg,
                                    min_abs,
                                    max_abs,
                                    info.angle_min_deg,
                                    info.angle_max_deg,
                                    (unsigned int)info.pulse_center_us,
                                    (unsigned int)info.pulse_min_us,
                                    (unsigned int)info.pulse_max_us)) {
            psram_free(json_buf);
            motutu_web_send_json(client, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"json build failed\"}");
            return;
        }
    }

    if (!motutu_web_append_json(json_buf, 1536U, &used,
                                "],\"templates\":[\"calibrate\",\"home\",\"stand\",\"down\",\"nod\",\"swing\",\"flip\",\"wave_left\",\"wave_right\",\"shake_left\",\"shake_right\",\"spin_left\",\"spin_right\",\"serve_left\",\"serve_right\",\"walk\",\"back\",\"left\",\"right\"],"
                                "\"formats\":[\"motutu-seq-v1\",\"motutu-pulse-v1\",\"rerobot-adapter-v1\"]}")) {
        psram_free(json_buf);
        motutu_web_send_json(client, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"json build failed\"}");
        return;
    }

    motutu_web_send_json(client, "200 OK", json_buf);
    psram_free(json_buf);
}

static void motutu_web_handle_sequence(int client, const char *body, int body_len)
{
    char *json_buf;
    char resp[128];
    int ret;

    if (body == NULL || body_len <= 0) {
        motutu_web_send_json(client, "400 Bad Request", "{\"ok\":false,\"msg\":\"empty body\"}");
        return;
    }

    if (body_len > MOTUTU_WEB_BODY_MAX) {
        snprintf(resp,
                 sizeof(resp),
                 "{\"ok\":false,\"msg\":\"body too large\",\"body_len\":%d,\"body_max\":%u}",
                 body_len,
                 (unsigned int)MOTUTU_WEB_BODY_MAX);
        motutu_web_send_json(client, "413 Payload Too Large", resp);
        return;
    }

    json_buf = (char *)psram_malloc((size_t)body_len + 1U);
    if (json_buf == NULL) {
        motutu_web_send_json(client, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"oom\"}");
        return;
    }

    memcpy(json_buf, body, (size_t)body_len);
    json_buf[body_len] = '\0';

    ret = motutu_execute_sequence_json(json_buf);
    psram_free(json_buf);
    snprintf(resp,
             sizeof(resp),
             "{\"ok\":%s,\"ret\":%d}",
             ret == 0 ? "true" : "false",
             ret);
    motutu_web_send_json(client, ret == 0 ? "200 OK" : "500 Internal Server Error", resp);
}

static void motutu_web_handle_show_page(int client)
{
    char resp[96];
    int ret = service_motutu_web_show_control_page();

    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"ret\":%d}", ret == 0 ? "true" : "false", ret);
    motutu_web_send_json(client, ret == 0 ? "200 OK" : "500 Internal Server Error", resp);
}

static void motutu_web_handle_client(int client)
{
    char *req_buf;
    char method[16] = {0};
    char path[64] = {0};
    const char *body;
    int body_len = 0;

    req_buf = (char *)psram_malloc(MOTUTU_WEB_REQ_BUF_SIZE);
    if (req_buf == NULL) {
        motutu_web_send_text(client, "500 Internal Server Error", "oom");
        return;
    }

    if (motutu_web_recv_request(client, req_buf, MOTUTU_WEB_REQ_BUF_SIZE, &body_len) != 0) {
        motutu_web_send_json(client, "400 Bad Request", "{\"ok\":false,\"msg\":\"bad request or body too large\"}");
        goto cleanup;
    }

    if (!motutu_web_parse_request_line(req_buf, method, sizeof(method), path, sizeof(path))) {
        motutu_web_send_text(client, "400 Bad Request", "bad request line");
        goto cleanup;
    }

    body = motutu_web_find_header_end(req_buf);
    if (body == NULL) {
        motutu_web_send_text(client, "400 Bad Request", "missing headers");
        goto cleanup;
    }

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        motutu_web_send_response(client, "200 OK", "text/html", s_motutu_index_html);
        goto cleanup;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/dog/status") == 0) {
        motutu_web_handle_status(client);
        goto cleanup;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/dog/meta") == 0) {
        motutu_web_handle_meta(client);
        goto cleanup;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/dog/sequence") == 0) {
        motutu_web_handle_sequence(client, body, body_len);
        goto cleanup;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/dog/show_page") == 0) {
        motutu_web_handle_show_page(client);
        goto cleanup;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0) {
        motutu_web_send_response(client, "204 No Content", "text/plain", "");
        goto cleanup;
    }

    motutu_web_send_text(client, "404 Not Found", "not found");
cleanup:
    psram_free(req_buf);
}

static void motutu_web_task(void *arg)
{
    (void)arg;

    LOGI("motutu web task start, stack=%u", (unsigned int)MOTUTU_WEB_STACK_SIZE);

    while (1) {
        int server_sock;
        struct sockaddr_in addr;

        server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_sock < 0) {
            LOGE("motutu web socket create failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        {
            int opt = 1;
            setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        }

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(MOTUTU_WEB_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            LOGE("motutu web bind failed");
            closesocket(server_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (listen(server_sock, MOTUTU_WEB_BACKLOG) != 0) {
            LOGE("motutu web listen failed");
            closesocket(server_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        LOGW("motutu web ready on port %u", (unsigned int)MOTUTU_WEB_PORT);

        while (1) {
            int client;
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            client = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
            if (client < 0) {
                LOGW("motutu web accept failed");
                break;
            }

            motutu_web_handle_client(client);
            closesocket(client);
        }

        closesocket(server_sock);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int service_motutu_web_init(void)
{
    if (s_motutu_web_inited) {
        return 0;
    }

    LOGI("motutu web init, stack=%u", (unsigned int)MOTUTU_WEB_STACK_SIZE);

    if (xTaskCreate(motutu_web_task,
                    "motutu_web",
                    MOTUTU_WEB_STACK_SIZE,
                    NULL,
                    MOTUTU_WEB_TASK_PRIORITY,
                    &s_motutu_web_task) != pdPASS) {
        LOGE("motutu web task create failed");
        return -1;
    }

    LOGI("motutu web init ok");
    s_motutu_web_inited = true;
    return 0;
}
