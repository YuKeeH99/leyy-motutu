#include "lingxin_http.h"

#define TAG "lingxin.http"

#include <stdio.h>
#include <string.h>

#include "lisa_http.h"
#include "lisa_log.h"
#include "lingxin_server_config.h"

typedef struct {
    RequestCallback callback;
    void *user_data;
} lingxin_http_ctx_t;

static void lingxin_http_on_data(lisa_http_data_t *data)
{
    lingxin_http_ctx_t *ctx = (lingxin_http_ctx_t *)data->user;
    if (ctx && ctx->callback && data->buf && data->len > 0) {
        ctx->callback((void *)data->buf, (size_t)data->len, ctx->user_data);
    }
}

static void lingxin_http_build_headers(const HttpHeader *headers, char *buf, size_t len)
{
    if (!headers || !buf || len == 0) {
        return;
    }

    snprintf(buf, len,
             "Content-Type: application/json\r\n"
             "signature: %s\r\n"
             "sn: %s\r\n"
             "app_id: %s\r\n"
             "timestamp: %s\r\n",
             headers->signature ? headers->signature : "",
             headers->sn ? headers->sn : "",
             headers->app_id ? headers->app_id : "",
             headers->timestamp ? headers->timestamp : "");
}

int http_post(HttpConfig *config, RequestCallback userCallback, void *userData)
{
    if (!config || !config->protocol || !config->host || !config->path) {
        LOGE("invalid http config");
        return 0;
    }

    char url[URL_MAX_LENGTH] = {0};
    char headers[HEADER_MAX_LENGTH] = {0};
    lingxin_http_ctx_t ctx = {
        .callback = userCallback,
        .user_data = userData,
    };

    const char *host = lingxin_server_config_cloud_host();
    int port = lingxin_server_config_cloud_port();

    snprintf(url, sizeof(url), "%s://%s:%d/%s",
             config->protocol,
             (host && host[0] != '\0') ? host : config->host,
             port > 0 ? port : config->port,
             config->path[0] == '/' ? config->path + 1 : config->path);
    lingxin_http_build_headers(config->headers, headers, sizeof(headers));
    LOGI("http post begin, url=%s body_len=%d", url,
         config->post_data ? (int)strlen(config->post_data) : 0);

    lisa_http_request_t req = {
        .method = LISA_HTTP_POST,
        .url = (uint8_t *)url,
        .headers = (uint8_t *)headers,
        .body = (void *)config->post_data,
        .body_len = config->post_data ? (int32_t)strlen(config->post_data) : 0,
        .timeout = 10000,
        .user = &ctx,
        .on_data = lingxin_http_on_data,
    };

    lisa_http_t *http = lisa_http_init(&req);
    if (!http) {
        LOGE("lisa_http_init failed");
        return 0;
    }

    lisa_http_err_e ret = lisa_http_perform(http);
    LOGI("http post end, url=%s ret=%d", url, ret);
    lisa_http_cleanup(http);
    return ret == LISA_HTTP_OK ? 1 : 0;
}
