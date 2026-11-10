#include "lingxin_websocket.h"

#define TAG "lingxin.ws"

#include <stdio.h>
#include <string.h>

#include "lisa_log.h"
#include "lisa_mem.h"
#include "lisa_thread.h"
#include "lisa_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lisa_websocket.h"
#include "lingxin_server_config.h"
#include "lingxin_trace.h"
#include "sys_network_diag.h"
#include "sys_network_manager.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
#endif

#define LINGXIN_WS_SEND_RETRY_DELAY_MS 20U
/* Absorb a transient stall without hiding an indefinitely slow connection.
 * Capture has another four-second bounded queue; keep this deadline shorter. */
#define LINGXIN_WS_SEND_WAIT_MS 1500U
#define LINGXIN_WS_CLOSE_DRAIN_MS 2500U

typedef struct {
    lisa_ws_t *ws;
    WebsocketConfig *config;
    bool connected;
    bool closing;
    bool connect_failure_reported;
    uint32_t owner_network_epoch;
    char port[16];
    char path[1024];
    char scheme[8];
    char extra_header[512];
} lingxin_ws_ctx_t;

static void lingxin_ws_release(WebsocketClient *client)
{
    if (client != NULL) {
        (void)__atomic_sub_fetch(&client->active_operations, 1U, __ATOMIC_RELEASE);
    }
}

static lingxin_ws_ctx_t *lingxin_ws_acquire(WebsocketClient *client)
{
    if (client == NULL) {
        return NULL;
    }

    (void)__atomic_add_fetch(&client->active_operations, 1U, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&client->closing, __ATOMIC_ACQUIRE)) {
        lingxin_ws_release(client);
        return NULL;
    }

    lingxin_ws_ctx_t *ctx = (lingxin_ws_ctx_t *)__atomic_load_n(&client->clientHandler,
                                                                 __ATOMIC_ACQUIRE);
    if (ctx == NULL) {
        lingxin_ws_release(client);
        return NULL;
    }
    return ctx;
}

static uint32_t s_ws_binary_send_count;
static uint32_t s_ws_binary_send_bytes;
static uint32_t s_ws_binary_drop_count;
static uint32_t s_ws_wait_count, s_ws_wait_total_ms, s_ws_wait_max_ms;
static uint32_t s_ws_wait_timeout, s_ws_wait_cancel;
static WebsocketTransportStats s_last_transport;

static void lingxin_ws_note_wait(uint32_t elapsed)
{
    if (!elapsed) return;
    __atomic_add_fetch(&s_ws_wait_count, 1U, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_ws_wait_total_ms, elapsed, __ATOMIC_RELAXED);
    uint32_t previous = __atomic_load_n(&s_ws_wait_max_ms, __ATOMIC_RELAXED);
    while (elapsed > previous && !__atomic_compare_exchange_n(&s_ws_wait_max_ms, &previous,
            elapsed, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void lingxin_ws_on_event(lisa_ws_event_t *event)
{
    lingxin_ws_ctx_t *ctx = event ? (lingxin_ws_ctx_t *)event->user : NULL;
    if (!ctx || __atomic_load_n(&ctx->closing, __ATOMIC_ACQUIRE) ||
        !ctx->config || !ctx->config->listener) {
        return;
    }

    switch (event->what) {
    case LISA_WS_ON_CONNECTED:
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (!service_offline_action_network_current(ctx->owner_network_epoch)) {
            ctx->connect_failure_reported = true;
            ctx->config->listener(ON_WEBSOCKET_CONNECTION_FAIL, NULL, 0, 0, ctx->config->userContext);
            break;
        }
        service_offline_action_cloud_connected(service_offline_action_generation());
#endif
        ctx->connected = true;
        ctx->connect_failure_reported = false;
        sys_network_diag_mark("websocket", "connected", 0, "handshake_complete");
        lingxin_trace_set("ws:connected");
        ctx->config->listener(ON_WEBSOCKET_CONNECTION_SUCCESS, NULL, 0, 0, ctx->config->userContext);
        break;
    case LISA_WS_ON_DISCONNECTED:
        sys_network_diag_mark("websocket", "disconnected", ctx->connected ? 1 : 0,
                              ctx->connected ? "after_handshake" : "before_handshake");
        lingxin_trace_set("ws:disconnected");
        if (!ctx->connected) {
            /* nopoll reports a failed handshake as a disconnect.  Dropping it
             * leaves the SDK state machine in Upload_Transfer forever. */
            if (!ctx->connect_failure_reported) {
                ctx->connect_failure_reported = true;
                lingxin_trace_set("ws:connect_failed_before_connected");
                LOGW("[Lingxin] websocket disconnected before handshake completed");
                /* A replaced task may finish closing after a new mode was selected. */
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                if (service_offline_action_network_current(ctx->owner_network_epoch))
#endif
                    sys_network_report_transport_failure();
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                if (service_offline_action_network_current(ctx->owner_network_epoch))
                    service_offline_action_cloud_failed(service_offline_action_generation());
#endif
                ctx->config->listener(ON_WEBSOCKET_CONNECTION_FAIL,
                                      NULL, 0, 0, ctx->config->userContext);
            }
            break;
        }
        ctx->connected = false;
        /* Detach the callback before invoking destroy.  The callback may
         * schedule cleanup and release its user context; a later cleanup-side
         * disconnect must therefore be a no-op, not a second callback through
         * a dangling userContext pointer. */
        WebSocketEventListener listener = ctx->config->listener;
        void *user_context = ctx->config->userContext;
        ctx->config->listener = NULL;
        ctx->config->userContext = NULL;
        listener(ON_WEBSOCKET_DESTROY, NULL, 0, 0, user_context);
        break;
    case LISA_WS_ON_ERROR:
        sys_network_diag_mark("websocket", "error", ctx->connected ? 1 : 0,
                              ctx->connected ? "after_handshake" : "before_handshake");
        lingxin_trace_set("ws:error");
        if (!ctx->connected && !ctx->connect_failure_reported) {
            ctx->connect_failure_reported = true;
            lingxin_trace_set("ws:connect_failed_before_connected");
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
            if (service_offline_action_network_current(ctx->owner_network_epoch))
#endif
                sys_network_report_transport_failure();
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
            if (service_offline_action_network_current(ctx->owner_network_epoch))
                service_offline_action_cloud_failed(service_offline_action_generation());
#endif
            ctx->config->listener(ON_WEBSOCKET_CONNECTION_FAIL, NULL, 0, 0,
                                  ctx->config->userContext);
        } else {
            ctx->config->listener(ON_WEBSOCKET_ERROR, NULL, 0, 0,
                                  ctx->config->userContext);
        }
        break;
    case LISA_WS_ON_CONNECTING:
        sys_network_diag_mark("websocket", "connecting", 0, NULL);
        lingxin_trace_set("ws:connecting");
        break;
    default:
        break;
    }
}

static void lingxin_ws_on_data(lisa_ws_data_t *data)
{
    lingxin_ws_ctx_t *ctx = data ? (lingxin_ws_ctx_t *)data->user : NULL;
    if (!ctx || __atomic_load_n(&ctx->closing, __ATOMIC_ACQUIRE) ||
        !ctx->config || !ctx->config->listener || !data->buf) {
        return;
    }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* A websocket carries multiple protocol tasks. Ordinary wake generations
     * must not drop task_terminated/task_started on the reused connection.
     * The protocol manager filters per-task payloads by task_id/waitTerminate. */
    if (!service_offline_action_network_current(ctx->owner_network_epoch)) return;
#endif

    ctx->config->listener(ON_WEBSOCKET_DATA_RECEIVED,
                          (const char *)data->buf,
                          (size_t)data->len,
                          data->type == LISA_WS_BIN ? 1 : 0,
                          ctx->config->userContext);
}

static void lingxin_ws_build_headers(WebsocketConfig *config, char *buf, size_t len)
{
    if (!config || !buf || len == 0) {
        return;
    }

    snprintf(buf, len,
             "\r\nsignature: %s\r\n"
             "sn: %s\r\n"
             "app_id: %s\r\n"
             "timestamp: %s",
             config->header_signature ? config->header_signature : "",
             config->header_sn ? config->header_sn : "",
             config->header_app_id ? config->header_app_id : "",
             config->header_timestamp ? config->header_timestamp : "");
}

WebsocketClient *initWebsocket(WebsocketConfig *config)
{
    LOGI("[Lingxin] initWebsocket enter config=%p", config);
    lingxin_trace_set("ws:init_enter");
    if (!config || !config->protocol || !config->host || !config->path) {
        LOGE("invalid websocket config");
        lingxin_trace_set("ws:init_invalid_config");
        return NULL;
    }

    WebsocketClient *client = (WebsocketClient *)lisa_mem_calloc(1, sizeof(WebsocketClient));
    lingxin_ws_ctx_t *ctx = (lingxin_ws_ctx_t *)lisa_mem_calloc(1, sizeof(lingxin_ws_ctx_t));
    if (!client || !ctx) {
        if (client) {
            lisa_mem_free(client);
        }
        if (ctx) {
            lisa_mem_free(ctx);
        }
        return NULL;
    }

    const char *host = lingxin_server_config_cloud_host();
    int port = lingxin_server_config_cloud_port();

    ctx->config = config;
    snprintf(ctx->scheme, sizeof(ctx->scheme), "%s", "wss");
    snprintf(ctx->port, sizeof(ctx->port), "%d", port);
    /* Respect the endpoint selected by the protocol layer.  In particular,
     * Cloud-VAD half duplex uses /api/v2/agentChat; forcing the historical v1
     * path here silently turns the new state machine back into the old API. */
    if (config->path[0] == '\0') {
        snprintf(ctx->path, sizeof(ctx->path), "%s", "/");
    } else if (config->path[0] == '/') {
        snprintf(ctx->path, sizeof(ctx->path), "%s", config->path);
    } else {
        snprintf(ctx->path, sizeof(ctx->path), "/%s", config->path);
    }
    lingxin_ws_build_headers(config, ctx->extra_header, sizeof(ctx->extra_header));
    LOGI("[Lingxin] ws auth header prepared");

    lisa_ws_request_t req = {
        .scheme = (uint8_t *)ctx->scheme,
        .host = (uint8_t *)((host && host[0] != '\0') ? host : config->host),
        .path = (uint8_t *)ctx->path,
        .port = (uint8_t *)ctx->port,
        /* lisa_websocket passes this value to nopoll in seconds. */
        .timeout = 20,
        .extra_header = ctx->extra_header,
        .user = ctx,
        .on_event = lingxin_ws_on_event,
        .on_data = lingxin_ws_on_data,
    };

    char trace_line[128] = {0};
    snprintf(trace_line, sizeof(trace_line), "ws:req=%s:%s%s", ctx->scheme, ctx->port, ctx->path);
    lingxin_trace_set(trace_line);
    lingxin_trace_set("ws:path_from_protocol_config");
    LOGI("[Lingxin] websocket request %s://%s:%s%s",
         ctx->scheme,
         req.host ? (char *)req.host : "",
         ctx->port,
         ctx->path);

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    ctx->owner_network_epoch = service_offline_action_network_epoch();
#endif
    ctx->ws = lisa_ws_init(&req);
    if (!ctx->ws) {
        lisa_mem_free(ctx);
        lisa_mem_free(client);
        return NULL;
    }

    client->clientHandler = ctx;
    client->config = config;
    return client;
}

bool startWebsocket(WebsocketClient *client)
{
    lingxin_ws_ctx_t *ctx = lingxin_ws_acquire(client);
    LOGI("[Lingxin] startWebsocket enter client=%p ctx=%p ws=%p", client, ctx, ctx ? ctx->ws : NULL);
    lingxin_trace_set("ws:start_enter");
    if (!ctx || !ctx->ws) {
        lingxin_trace_set("ws:start_invalid_ctx");
        if (ctx != NULL) {
            lingxin_ws_release(client);
        }
        return false;
    }
    lingxin_trace_set("ws:start_call");
    s_ws_binary_send_count = 0;
    s_ws_binary_send_bytes = 0;
    s_ws_binary_drop_count = 0;
    lisa_ws_err_e ret = lisa_ws_connect(ctx->ws);
    sys_network_diag_mark("websocket", "connect_call_return", (int)ret,
                          ret == LISA_WS_OK ? "accepted" : "rejected");
    LOGI("[Lingxin] startWebsocket lisa_ws_connect ret=%d", ret);
    lingxin_trace_set(ret == LISA_WS_OK ? "ws:start_ret_ok" : "ws:start_ret_fail");
    lingxin_ws_release(client);
    return ret == LISA_WS_OK;
}

bool websocketSendText(WebsocketClient *client, const char *message)
{
    lingxin_ws_ctx_t *ctx = lingxin_ws_acquire(client);
    if (!ctx || !ctx->ws || !message || !ctx->connected) {
        if (ctx && !ctx->connected) {
            LOGW("[Lingxin] ws text dropped: connection is not ready");
        }
        if (ctx != NULL) {
            lingxin_ws_release(client);
        }
        return false;
    }
    LOGI("[Lingxin] ws send text: %.220s", message);
    bool sent = lisa_ws_send_text(ctx->ws, (const uint8_t *)message) == LISA_WS_OK;
    lingxin_ws_release(client);
    return sent;
}

int websocketSendBinaryGuarded(WebsocketClient *client, const char *audioData, size_t dataSize,
                               bool (*current)(void *), void *guard_arg)
{
    lingxin_ws_ctx_t *ctx = lingxin_ws_acquire(client);
    if (!ctx || !ctx->ws || !audioData || dataSize == 0) {
        if (ctx != NULL) {
            lingxin_ws_release(client);
        }
        return 0;
    }
    uint32_t started = lisa_os_get_tick_ms();
    bool waited = false;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    uint32_t owner = service_offline_action_generation();
#endif
    lisa_ws_err_e ret = LISA_WS_COMMON_ERR;
    for (;;) {
        if (__atomic_load_n(&client->closing, __ATOMIC_ACQUIRE) ||
            (current && !current(guard_arg))
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
            || (current && !service_offline_action_online_current(owner))
#endif
            ) {
            if (waited) lingxin_ws_note_wait(lisa_os_get_tick_ms() - started);
            __atomic_add_fetch(&s_ws_wait_cancel, 1U, __ATOMIC_RELAXED);
            lingxin_ws_release(client);
            return WEBSOCKET_SEND_CANCELLED;
        }
        if (!ctx->connected) {
            ret = LISA_WS_DISCONNECT;
            break;
        }
        ret = lisa_ws_send_binary(ctx->ws, audioData, (uint32_t)dataSize);
        if (ret != LISA_WS_WOULD_BLOCK) break;
        waited = true;
        uint32_t elapsed = lisa_os_get_tick_ms() - started;
        if (elapsed >= LINGXIN_WS_SEND_WAIT_MS) {
            __atomic_add_fetch(&s_ws_wait_timeout, 1U, __ATOMIC_RELAXED);
            break;
        }
        uint32_t remaining = LINGXIN_WS_SEND_WAIT_MS - elapsed;
        lisa_thread_mdelay(remaining < LINGXIN_WS_SEND_RETRY_DELAY_MS
                              ? remaining : LINGXIN_WS_SEND_RETRY_DELAY_MS);
    }
    if (waited) lingxin_ws_note_wait(lisa_os_get_tick_ms() - started);
    if (ret == LISA_WS_OK) {
        s_ws_binary_send_count++;
        s_ws_binary_send_bytes += (uint32_t)dataSize;
        if (s_ws_binary_send_count == 1U || (s_ws_binary_send_count % 100U) == 0U) {
            LOGI("[Lingxin] ws binary sent=%u, bytes=%u, last_len=%u",
                 s_ws_binary_send_count,
                 s_ws_binary_send_bytes,
                 (uint32_t)dataSize);
        }
        lingxin_ws_release(client);
        return (int)dataSize;
    }

    if (ret == LISA_WS_WOULD_BLOCK) {
        s_ws_binary_drop_count++;
        if (s_ws_binary_drop_count == 1U || (s_ws_binary_drop_count % 100U) == 0U) {
            sys_network_diag_mark("websocket", "audio_send_congested",
                                  (int)s_ws_binary_drop_count, "bounded_wait_expired");
            LOGW("[Lingxin] ws queue wait expired, failures=%u, last_len=%u",
                 s_ws_binary_drop_count,
                 (uint32_t)dataSize);
        }
        /* The protocol aborts this utterance; never skip a missing frame and
         * upload its suffix as though the sentence were complete. */
        lingxin_ws_release(client);
        return 0;
    }

    LOGE("[Lingxin] ws binary send failed ret=%d, len=%u", ret, (uint32_t)dataSize);
    sys_network_diag_mark("websocket", "audio_send_failed", (int)ret, "transport_rejected");
    lingxin_ws_release(client);
    return 0;
}

int websocketSendBinary(WebsocketClient *client, const char *audioData, size_t dataSize)
{
    return websocketSendBinaryGuarded(client, audioData, dataSize, NULL, NULL);
}

static void lingxin_ws_snapshot(lingxin_ws_ctx_t *ctx, WebsocketTransportStats *stats)
{
    lisa_ws_transport_stats_t local;
    lisa_ws_get_transport_stats(ctx->ws, &local);
#define COPY_STAT(field) stats->field = local.field
    COPY_STAT(enqueued_frames); COPY_STAT(enqueued_bytes); COPY_STAT(written_bytes); COPY_STAT(completed_frames);
    COPY_STAT(queue_current); COPY_STAT(queue_peak); COPY_STAT(queue_full); COPY_STAT(pending_bytes);
    COPY_STAT(send_max_ms); COPY_STAT(receive_max_ms);
#undef COPY_STAT
    stats->connected = ctx->connected;
}

void websocketGetTransportStats(WebsocketClient *client, WebsocketTransportStats *stats)
{
    if (!stats) return;
    taskENTER_CRITICAL();
    *stats = s_last_transport;
    taskEXIT_CRITICAL();
    lingxin_ws_ctx_t *ctx = lingxin_ws_acquire(client);
    if (ctx) {
        lingxin_ws_snapshot(ctx, stats);
        lingxin_ws_release(client);
    }
#define WAIT_STAT(field) stats->field = __atomic_load_n(&s_ws_##field, __ATOMIC_RELAXED)
    WAIT_STAT(wait_count); WAIT_STAT(wait_total_ms); WAIT_STAT(wait_max_ms);
    WAIT_STAT(wait_timeout); WAIT_STAT(wait_cancel);
#undef WAIT_STAT
}

void websocketGetSendBacklogOnReceive(WebsocketClient *client, unsigned int *queued_messages,
                                    unsigned int *pending_bytes)
{
    if (queued_messages) *queued_messages = 0U;
    if (pending_bytes) *pending_bytes = 0U;
    lingxin_ws_ctx_t *ctx = lingxin_ws_acquire(client);
    if (!ctx) return;
    lisa_ws_get_send_backlog_on_worker(ctx->ws, queued_messages, pending_bytes);
    lingxin_ws_release(client);
}

bool closeWebsocket(WebsocketClient *client)
{
    if (client == NULL) {
        return true;
    }

    bool expected = false;
    if (!__atomic_compare_exchange_n(&client->close_active, &expected, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;
    }
    __atomic_store_n(&client->closing, true, __ATOMIC_RELEASE);
    /* Refuse new API leases, but retain the handle until the worker has
     * retired DNS/TLS, noPoll resources and all in-flight callbacks. */
    lingxin_ws_ctx_t *ctx = (lingxin_ws_ctx_t *)__atomic_load_n(
        &client->clientHandler, __ATOMIC_ACQUIRE);
    if (ctx == NULL) {
        __atomic_store_n(&client->close_active, false, __ATOMIC_RELEASE);
        return true;
    }
    __atomic_store_n(&ctx->closing, true, __ATOMIC_RELEASE);
    uint32_t started = lisa_os_get_tick_ms();
    while (__atomic_load_n(&client->active_operations, __ATOMIC_ACQUIRE) != 0U) {
        if ((uint32_t)(lisa_os_get_tick_ms() - started) >= LINGXIN_WS_CLOSE_DRAIN_MS) {
            __atomic_store_n(&client->close_active, false, __ATOMIC_RELEASE);
            return false;
        }
        lisa_thread_mdelay(1U);
    }
    lisa_ws_t *ws = ctx->ws;
    WebsocketTransportStats final_stats = {0};
    lingxin_ws_snapshot(ctx, &final_stats);
    final_stats.connected = false;
    taskENTER_CRITICAL();
    s_last_transport = final_stats;
    taskEXIT_CRITICAL();
    sys_network_diag_mark("websocket", "cleanup", ctx && ctx->connected ? 1 : 0,
                          "client_close");
    if (ws != NULL) {
        if (lisa_ws_cleanup(ws) != LISA_WS_OK) {
            __atomic_store_n(&client->close_active, false, __ATOMIC_RELEASE);
            return false;
        }
    }
    ctx->ws = NULL;
    __atomic_store_n(&client->clientHandler, NULL, __ATOMIC_RELEASE);
    lisa_mem_free(ctx);
    __atomic_store_n(&client->close_active, false, __ATOMIC_RELEASE);
    return true;
}

void destroyWebsocketClient(WebsocketClient *client)
{
    if (client == NULL) {
        return;
    }
    if (__atomic_load_n(&client->clientHandler, __ATOMIC_ACQUIRE) != NULL ||
        __atomic_load_n(&client->active_operations, __ATOMIC_ACQUIRE) != 0U ||
        __atomic_load_n(&client->close_active, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&client->closing, __ATOMIC_ACQUIRE)) {
        LOGW("[Lingxin] preserve websocket client wrapper until transport is fully closed");
        return;
    }
    lisa_mem_free(client);
}
