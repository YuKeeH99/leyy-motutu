#define TAG "lisa-ws"

#include "lisa_websocket.h"
#include "lisa_websocket_pending_policy.h"

#include "lisa_log.h"
#include "lisa_mem.h"
#include "lisa_mutex.h"
#include "lisa_queue.h"
#include "lisa_thread.h"
#include "lisa_typedef.h"
#include "lisa_time.h"
#include "lisa_semaphore.h"
#include "task.h"

#include <nopoll.h>
#include <nopoll_private.h>

#define WS_SEND_RETRY_DELAY           (CONFIG_LISA_WEBSOCKET_SEND_RETRY_DELAY_NS)
#define SEND_STACK_SIZE               (CONFIG_LISA_WEBSOCKET_THREAD_STACK_SIZE)
#define WS_QUEUE_COUNT                (CONFIG_LISA_WEBSOCKET_QUEUE_COUNT)
#define WS_PINGPONG_TIMEOUT_MS        (CONFIG_LISA_WEBSOCKET_PING_PONG_TIMEOUT_MS)
#define WS_PINGPONG_RETRY_CNT         (CONFIG_LISA_WEBSOCKET_PING_PONG_RETRY_CNT)
#define WS_FRAGMENT_RECV_TIMEOUT_MS   (CONFIG_LISA_WEBSOCKET_FRAGMENT_RECV_TIMEOUT_MS)
#define WS_PING_PONG                  (CONFIG_LISA_WEBSOCKET_PING_PONG_MSG)
#define WS_THREAD_PRIO                (CONFIG_LISA_WEBSOCKET_THREAD_PRIO)

struct lisa_ws {
    url_info_t u_info;
    uint32_t timeout;
    bool ws_conn;
    bool ws_stop;
    bool worker_busy; /* Reserved before queuing connect; cleared after all callbacks/resources retire. */
    bool cleanup_requested;
    int32_t connect_cancel; /* noPoll *_with_ctrl: zero runs, nonzero cancels. */
    void *user;
    const char *extra_header;
    lisa_queue_t *tx_msg_queue;
    void (*inter_on_event)(lisa_ws_event_t *event);
    void (*inter_on_data)(lisa_ws_data_t *data);
    lisa_semaphore_t *exiting_sem;
    lisa_semaphore_t *exited_sem;
    lisa_semaphore_t *pong_sem;
    lisa_semaphore_t *connect_sem;
    uint32_t pong_max_time_ms;
    uint32_t last_ping_sent_ms; /* 硬速率限制：距上次 PING 至少隔 pong_max_time_ms */
    uint8_t ping_pong_lost_cnt;
    lisa_ws_pending_tracker_t pending_write;
    lisa_thread_t *thread;
    noPollCtx *nopoll_ctx;
    noPollConn *nopoll_conn;
    char *rx_msg_buf;
    uint32_t rx_msg_len;
    uint32_t rx_msg_cap;
    lisa_ws_data_type_e rx_msg_type;
    bool rx_msg_active;
    lisa_ws_transport_stats_t transport;
    bool pending_payload_frame;
};

static void lisa_ws_note_max(uint32_t *target, uint32_t value)
{
    uint32_t previous = __atomic_load_n(target, __ATOMIC_RELAXED);
    while (value > previous && !__atomic_compare_exchange_n(target, &previous, value,
            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

typedef struct {
    lisa_ws_data_type_e m_type; // 0 txt 1 audio
    char *m_msg;
    int m_size;
    uint32_t flash_index;
} lisa_ws_msg_t;

static void lisa_websocket_thread(void *param);

static void lisa_websocket_rx_msg_reset(lisa_ws_t *ins)
{
    if (ins == NULL) {
        return;
    }

    if (ins->rx_msg_buf != NULL) {
        lisa_mem_free(ins->rx_msg_buf);
        ins->rx_msg_buf = NULL;
    }

    ins->rx_msg_len = 0;
    ins->rx_msg_cap = 0;
    ins->rx_msg_type = LISA_WS_TEXT;
    ins->rx_msg_active = false;
}

static int lisa_websocket_rx_msg_reserve(lisa_ws_t *ins, uint32_t need)
{
    char *buf;
    uint32_t new_cap;

    if (need <= ins->rx_msg_cap) {
        return 0;
    }

    new_cap = ins->rx_msg_cap ? ins->rx_msg_cap : 256;
    while (new_cap < need) {
        if (new_cap > UINT32_MAX / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }

    buf = ins->rx_msg_buf ? lisa_mem_realloc(ins->rx_msg_buf, new_cap) : lisa_mem_alloc(new_cap);
    if (buf == NULL) {
        LISA_NLOGE("rx msg buf alloc failed, need:%u", need);
        return -1;
    }

    ins->rx_msg_buf = buf;
    ins->rx_msg_cap = new_cap;
    return 0;
}

static int lisa_websocket_rx_msg_append(lisa_ws_t *ins, const void *data, uint32_t len, lisa_ws_data_type_e type, bool start)
{
    if (start) {
        if (ins->rx_msg_active) {
            LISA_NLOGW("drop unfinished websocket message, len:%u", ins->rx_msg_len);
            lisa_websocket_rx_msg_reset(ins);
        }
        ins->rx_msg_type = type;
        ins->rx_msg_active = true;
    } else if (!ins->rx_msg_active) {
        LISA_NLOGE("unexpected websocket continuation frame");
        return -1;
    }

    if (lisa_websocket_rx_msg_reserve(ins, ins->rx_msg_len + len + 1) != 0) {
        lisa_websocket_rx_msg_reset(ins);
        return -1;
    }

    if (len > 0) {
        memcpy(ins->rx_msg_buf + ins->rx_msg_len, data, len);
        ins->rx_msg_len += len;
    }
    ins->rx_msg_buf[ins->rx_msg_len] = 0;
    return 0;
}

static void lisa_websocket_rx_msg_dispatch(lisa_ws_t *ins, const void *msg, uint32_t len, lisa_ws_data_type_e type)
{
    lisa_ws_data_t ws_data;

    ws_data.type = type;
    ws_data.user = ins->user;
    ws_data.buf = msg;
    ws_data.len = len;
    ins->inter_on_data(&ws_data);
}

static void _callback_ws_disconnect(lisa_ws_t *handle)
{
    if (handle) {
        lisa_websocket_rx_msg_reset(handle);
        handle->ws_conn = false;
        lisa_ws_event_t ws_event;
        ws_event.what = LISA_WS_ON_DISCONNECTED;
        ws_event.user = handle->user;
        handle->inter_on_event(&ws_event);
    }
}

static void _callback_ws_connected(lisa_ws_t *handle)
{
    if (handle) {
        handle->ws_conn = true;
        lisa_ws_event_t ws_event;
        ws_event.what = LISA_WS_ON_CONNECTED;
        ws_event.user = handle->user;
        handle->inter_on_event(&ws_event);
    }
}

static void nopoll_conn_on_close(noPollCtx *ctx, noPollConn *conn, noPollPtr user_data)
{
    LISA_NLOGI("no poll conn closed, status:%d, reason:%s", conn->peer_close_status, conn->peer_close_reason);
}

lisa_ws_t *lisa_ws_init(lisa_ws_request_t *req)
{
    lisa_ws_t *handle = (lisa_ws_t *)lisa_mem_calloc(1, sizeof(lisa_ws_t));
    if (handle == NULL) {
        LISA_NLOGE("handle create failed. ");
        return NULL;
    }
    memset(handle, 0, sizeof(lisa_ws_t));

    if (req != NULL) {
        LISA_NLOGD("port %s", req->port);
        LISA_NLOGD("scheme %s", req->scheme);
        LISA_NLOGD("host %s", req->host);
        LISA_NLOGD("path %s", req->path);

        strcpy((char *)handle->u_info.scheme, req->scheme);
        strcpy((char *)handle->u_info.host, req->host);
        strcpy((char *)handle->u_info.path, req->path);
        strcpy((char *)handle->u_info.port, req->port);

        handle->user = req->user;
        handle->inter_on_event = req->on_event;
        handle->timeout = req->timeout;
        handle->extra_header = req->extra_header;
        handle->inter_on_data = req->on_data;
    }
    handle->pong_max_time_ms = WS_PINGPONG_TIMEOUT_MS;
    handle->last_ping_sent_ms = 0;

    handle->tx_msg_queue = lisa_queue_create(WS_QUEUE_COUNT, "ws_tx_msg", sizeof(lisa_ws_msg_t));
    if (handle->tx_msg_queue == NULL) {
        LISA_NLOGE("create tx queue failed!");
        goto err_exit;
    }

    handle->exiting_sem = lisa_semaphore_create(1);
    if (handle->exiting_sem == NULL) {
        LISA_NLOGE("exiting_sem create failed!");
        goto err_exit;
    }
    handle->exited_sem = lisa_semaphore_create(1);
    if (handle->exited_sem == NULL) {
        LISA_NLOGE("exited_sem create failed!");
        goto err_exit;
    }
    handle->pong_sem = lisa_semaphore_create(1);
    if (handle->pong_sem == NULL) {
        LISA_NLOGE("pong_sem create failed!");
        goto err_exit;
    }
    handle->connect_sem = lisa_semaphore_create(1);
    if (handle->connect_sem == NULL) {
        LISA_NLOGE("connect_sem create failed!");
        goto err_exit;
    }

    lisa_thread_attr_t thread_attr;
    thread_attr.name = "ws_send";
    thread_attr.stack_size = SEND_STACK_SIZE;
    thread_attr.priority = WS_THREAD_PRIO;
    handle->thread = lisa_thread_create(&thread_attr, lisa_websocket_thread, (void *)handle);

    if (handle->thread == NULL) {
        LISA_NLOGE("thread create failed");
        goto err_exit;
    }

    return handle;

err_exit:
    if (handle->tx_msg_queue) {
        lisa_queue_delete(handle->tx_msg_queue);
    }

    if (handle->exiting_sem) {
        lisa_semaphore_delete(handle->exiting_sem);
    }

    if (handle->exited_sem) {
        lisa_semaphore_delete(handle->exited_sem);
    }
    if (handle->connect_sem) {
        lisa_semaphore_delete(handle->connect_sem);
    }
    if (handle->pong_sem) {
        lisa_semaphore_delete(handle->pong_sem);
    }
    if (handle->thread) {
        lisa_thread_delete(handle->thread);
    }

    lisa_mem_free(handle);

    return NULL;
}

static int lisa_websocket_msg_fragment_remain_recv(lisa_ws_t *ins, noPollConn *nopoll_conn, char *buf, uint32_t len)
{
    int recv_len = 0;
    uint32_t start_time = lisa_os_get_tick_ms();

    while ((recv_len < len) && (lisa_os_get_tick_ms() - start_time) < WS_FRAGMENT_RECV_TIMEOUT_MS) {
        noPollMsg *nopoll_msg;
        nopoll_msg = nopoll_conn_get_msg(nopoll_conn);

        if (nopoll_msg == NULL) {
            lisa_thread_mdelay(20);
            continue;
        }

        if (nopoll_msg->op_code == NOPOLL_PONG_FRAME) {
            lisa_semaphore_give(ins->pong_sem);
            nopoll_msg_unref(nopoll_msg);
            continue;
        }

        const unsigned char *msg = nopoll_msg_get_payload(nopoll_msg);
        LISA_NLOGD("msg fragment continue, payload size:%ld, payload:%s", nopoll_msg->payload_size, msg);

        if (nopoll_msg->is_fragment && nopoll_msg->payload_size <= (len - recv_len)) {
            memcpy(buf, msg, nopoll_msg->payload_size);
            recv_len += nopoll_msg->payload_size;
            buf += nopoll_msg->payload_size;
        } else {
            LISA_NLOGE("un expected msg received, %s, opcode:%d", msg, nopoll_msg->op_code);
        }
        nopoll_msg_unref(nopoll_msg);
    }

    if (recv_len != len) {
        LISA_NLOGE("msg fragment recv error, recv len:%d, len:%d", recv_len, len);
        return -1;
    }

    return 0;
}

static int lisa_websocket_msg_recv_proc(lisa_ws_t *ins, noPollConn *nopoll_conn)
{
    noPollMsg *nopoll_msg;
    noPollOpCode opcode;
    bool final_frame;
    bool partial_frame = false;
    lisa_ws_data_type_e data_type;

    nopoll_msg = nopoll_conn_get_msg(nopoll_conn);
    if (nopoll_msg == NULL) {
        return -1;
    }

    /* websocket pong frame */
    if (nopoll_msg->op_code == NOPOLL_PONG_FRAME) {
        lisa_semaphore_give(ins->pong_sem);
        nopoll_msg_unref(nopoll_msg);
        return 0;
    }

    const unsigned char *msg = nopoll_msg_get_payload(nopoll_msg);
    int len = nopoll_msg_get_payload_size(nopoll_msg);

    if (msg == NULL || len <= 0) {
        nopoll_msg_unref(nopoll_msg);
        return 0;
    }

    opcode = nopoll_msg_opcode(nopoll_msg);
    final_frame = nopoll_msg->has_fin != 0;

    /* Finish reading the current frame before websocket message reassembly. */
    if (nopoll_msg->remain_bytes > 0) {
        uint32_t total_size = nopoll_msg->remain_bytes + nopoll_msg->payload_size;
        LISA_NLOGD("msg is a fragment, remain:%d, payload size:%ld, total:%d, payload: %s", nopoll_msg->remain_bytes,
                   nopoll_msg->payload_size, total_size, msg);
        char *buf = lisa_mem_alloc(total_size + 1);
        if (buf == NULL) {
            LISA_NLOGE("fragment msg buf alloc failed");
            nopoll_msg_unref(nopoll_msg);
            return -1;
        }
        memcpy(buf, msg, nopoll_msg->payload_size);
        int err = lisa_websocket_msg_fragment_remain_recv(ins, nopoll_conn, buf + nopoll_msg->payload_size,
                                                          nopoll_msg->remain_bytes);
        if (err) {
            lisa_mem_free((void *)buf);
            return err;
        }

        buf[total_size] = 0;
        msg = buf;
        len = total_size;
        partial_frame = true;
    }

    if (opcode == NOPOLL_TEXT_FRAME) {
        data_type = LISA_WS_TEXT;
    } else if (opcode == NOPOLL_BINARY_FRAME) {
        data_type = LISA_WS_BIN;
    } else if (opcode == NOPOLL_CONTINUATION_FRAME) {
        data_type = ins->rx_msg_type;
    } else {
        data_type = LISA_WS_BIN;
    }

    if (opcode == NOPOLL_CONTINUATION_FRAME) {
        if (lisa_websocket_rx_msg_append(ins, msg, len, data_type, false) == 0 && final_frame) {
            lisa_websocket_rx_msg_dispatch(ins, ins->rx_msg_buf, ins->rx_msg_len, ins->rx_msg_type);
            lisa_websocket_rx_msg_reset(ins);
        }
    } else if (!final_frame) {
        (void)lisa_websocket_rx_msg_append(ins, msg, len, data_type, true);
    } else {
        if (ins->rx_msg_active) {
            LISA_NLOGW("drop unfinished websocket message before direct dispatch, len:%u", ins->rx_msg_len);
            lisa_websocket_rx_msg_reset(ins);
        }
        lisa_websocket_rx_msg_dispatch(ins, msg, len, data_type);
    }

    /* release msg */
    nopoll_msg_unref(nopoll_msg);
    if (partial_frame) {
        lisa_mem_free((void *)msg);
    }

    return 0;
}

static int lisa_websocket_msg_normal_send(lisa_ws_t *ins, noPollConn *nopoll_conn, lisa_ws_msg_t *msg)
{
    int err = -1;

    if (msg->m_type == LISA_WS_TEXT) {
        err = nopoll_conn_send_text(nopoll_conn, msg->m_msg, msg->m_size);
    } else if (msg->m_type == LISA_WS_BIN) {
        err = nopoll_conn_send_binary(nopoll_conn, msg->m_msg, msg->m_size);
    }

    lisa_ws_send_outcome_t outcome =
        lisa_ws_classify_send_result(err, nopoll_conn_pending_write_bytes(nopoll_conn));
    if (err > 0) __atomic_add_fetch(&ins->transport.written_bytes, (uint32_t)err, __ATOMIC_RELAXED);
    if (outcome == LISA_WS_SEND_FAILED) {
        LISA_NLOGE("websocket msg send failed, err:%d pending:%d", err,
                   nopoll_conn_pending_write_bytes(nopoll_conn));
        return -1;
    }

    /* -2/EAGAIN means noPoll already owns a copy of this complete frame.
     * Never submit the same frame again; the worker drains that pending copy. */

    return 0;
}

static int lisa_websocket_msg_send(lisa_ws_t *ins, noPollConn *nopoll_conn, lisa_ws_msg_t *msg)
{
    if (nopoll_conn == NULL || msg == NULL) {
        return -1;
    }
    /* One accepted message owns one frame. TCP/TLS handles wire segmentation;
     * short writes must never become duplicate WebSocket continuation frames. */
    return lisa_websocket_msg_normal_send(ins, nopoll_conn, msg);
}

static int lisa_websocket_pending_write_proc(lisa_ws_t *ins, noPollConn *nopoll_conn)
{
    int pending_before = nopoll_conn_pending_write_bytes(nopoll_conn);
    __atomic_store_n(&ins->transport.pending_bytes, (uint32_t)pending_before, __ATOMIC_RELAXED);
    bool was_active = ins->pending_write.active;
    uint32_t since_ms = ins->pending_write.since_ms;
    uint32_t now_ms = lisa_os_get_tick_ms();
    lisa_ws_pending_decision_t decision =
        lisa_ws_pending_tracker_update(&ins->pending_write, pending_before, now_ms,
                                       ins->pong_max_time_ms);

    if (decision == LISA_WS_PENDING_READY) {
        return 0;
    }
    if (!was_active) {
        since_ms = ins->pending_write.since_ms;
        LISA_NLOGW("websocket send backpressure, pending:%d", pending_before);
    }
    if (decision == LISA_WS_PENDING_STALLED) {
        LISA_NLOGE("websocket pending write stalled for %u ms, remaining:%d",
                   (unsigned)(now_ms - since_ms), pending_before);
        return -1;
    }

    unsigned int pending_sequence = nopoll_conn_pending_write_sequence(nopoll_conn);
    int flushed = nopoll_conn_complete_pending_write(nopoll_conn);
    if (ins->pending_payload_frame && flushed > 0)
        __atomic_add_fetch(&ins->transport.written_bytes, (uint32_t)flushed, __ATOMIC_RELAXED);
    int pending_after = nopoll_conn_pending_write_bytes(nopoll_conn);
    __atomic_store_n(&ins->transport.pending_bytes, (uint32_t)pending_after, __ATOMIC_RELAXED);
    now_ms = lisa_os_get_tick_ms();
    if (pending_after > 0 &&
        pending_sequence != nopoll_conn_pending_write_sequence(nopoll_conn)) {
        /* noPoll completed the old frame and started its deferred PONG. The
         * new control frame owns a fresh deadline and no application bytes. */
        if (ins->pending_payload_frame) {
            __atomic_add_fetch(&ins->transport.completed_frames, 1U, __ATOMIC_RELAXED);
            ins->pending_payload_frame = false;
        }
        ins->pending_write.active = false;
        ins->pending_write.since_ms = 0U;
        since_ms = now_ms;
    }
    decision = lisa_ws_pending_tracker_update(&ins->pending_write, pending_after, now_ms,
                                              ins->pong_max_time_ms);
    if (decision == LISA_WS_PENDING_READY) {
        if (ins->pending_payload_frame) {
            __atomic_add_fetch(&ins->transport.completed_frames, 1U, __ATOMIC_RELAXED);
            ins->pending_payload_frame = false;
        }
        LISA_NLOGI("websocket send backpressure recovered in %u ms",
                   (unsigned)(now_ms - since_ms));
        return 0;
    }
    if (flushed < 0 && !nopoll_conn_is_ok(nopoll_conn)) {
        LISA_NLOGE("websocket pending write failed, err:%d remaining:%d", errno,
                   pending_after);
        return -1;
    }
    if (decision == LISA_WS_PENDING_STALLED) {
        LISA_NLOGE("websocket pending write stalled for %u ms, remaining:%d",
                   (unsigned)(now_ms - since_ms), pending_after);
        return -1;
    }

    /* Kconfig's legacy *_NS name is inaccurate; nopoll_sleep uses microseconds. */
    nopoll_sleep(WS_SEND_RETRY_DELAY);
    return 1;
}

static int lisa_websocket_msg_send_proc(lisa_ws_t *ins, noPollConn *nopoll_conn)
{
    lisa_ws_msg_t msg;
    int pending_state = lisa_websocket_pending_write_proc(ins, nopoll_conn);

    if (pending_state < 0) {
        return -1;
    }
    if (pending_state > 0) {
        /* Preserve frame ordering while noPoll owns a pending frame. */
        return 0;
    }

    int err = lisa_queue_pop(ins->tx_msg_queue, &msg, sizeof(lisa_ws_msg_t), 20);
    if (err == 0 && msg.m_msg != NULL) {
        err = lisa_websocket_msg_send(ins, nopoll_conn, &msg);
        lisa_mem_free(msg.m_msg);
        if (err < 0) {
            return err;
        }
        ins->pending_payload_frame = nopoll_conn_pending_write_bytes(nopoll_conn) > 0;
        if (!ins->pending_payload_frame)
            __atomic_add_fetch(&ins->transport.completed_frames, 1U, __ATOMIC_RELAXED);
        pending_state = lisa_websocket_pending_write_proc(ins, nopoll_conn);
        return pending_state < 0 ? -1 : 0;
    }

    return 0;
}

bool lisa_websocket_need_to_exit(lisa_ws_t *ws)
{
    return __atomic_load_n(&ws->ws_stop, __ATOMIC_ACQUIRE) ||
           lisa_semaphore_take(ws->exiting_sem, 0) == 0;
}

static int lisa_websocket_nopoll_conn(lisa_ws_t *ins)
{
    noPollCtx *nopoll_ctx = NULL;
    noPollConn *nopoll_conn = NULL;
    noPollConnOpts *nopoll_opts = NULL;

    if (__atomic_load_n(&ins->connect_cancel, __ATOMIC_ACQUIRE)) {
        return -1;
    }

    nopoll_ctx = nopoll_ctx_new();
    if (!nopoll_ctx) {
        LISA_NLOGE("nopoll_ctx_new failed");
        return -1;
    }

    nopoll_opts = nopoll_conn_opts_new();
    if (nopoll_opts == NULL) {
        LISA_NLOGE("nopoll_conn_opts_new failed");
        goto err_exit;
    }

    if (ins->extra_header) {
        nopoll_conn_opts_set_extra_headers(nopoll_opts, ins->extra_header);
    }

    if (strcmp(ins->u_info.scheme, "ws") == 0) {
        nopoll_conn = nopoll_conn_new_opts_with_ctrl(nopoll_ctx, nopoll_opts, ins->u_info.host, ins->u_info.port,
                                           ins->u_info.host, ins->u_info.path, NULL, NULL, &ins->connect_cancel);
        nopoll_opts =  NULL;

    } else if (strcmp(ins->u_info.scheme, "wss") == 0) {
        if (!nopoll_conn_opts_set_ssl_certs(nopoll_opts, NULL, 0, NULL, 0, NULL, 0, NULL, 0)) {
            LISA_NLOGE("set_ssl_certs err");
            goto err_exit;
        }
        nopoll_conn_opts_ssl_peer_verify(nopoll_opts, nopoll_false);
        nopoll_conn = nopoll_conn_tls_new_with_ctrl(nopoll_ctx, nopoll_opts, ins->u_info.host, ins->u_info.port, ins->u_info.host,
                                          ins->u_info.path, NULL, NULL, &ins->connect_cancel);
        nopoll_opts =  NULL;
    } else {
        LISA_NLOGE("invalid scheme:%s", ins->u_info.scheme);
        goto err_exit;
    }

    if (nopoll_conn == NULL) {
        LISA_NLOGE("nopoll conn failed");
        goto err_exit;
    }

    if (!nopoll_conn_wait_until_connection_ready_with_ctrl(nopoll_conn, ins->timeout, &ins->connect_cancel)) {
        LISA_NLOGE("wait conn ready timeout");
        goto err_exit;
    }
    nopoll_conn_set_on_close(nopoll_conn, nopoll_conn_on_close, ins);

    ins->nopoll_conn = nopoll_conn;
    ins->nopoll_ctx = nopoll_ctx;

    return 0;

err_exit:
    if (nopoll_conn != NULL) {
        nopoll_conn_close(nopoll_conn);
        nopoll_conn = NULL;
    }

    if (nopoll_ctx) {
        nopoll_ctx_unref(nopoll_ctx);
        nopoll_ctx = NULL;
    }

    if (nopoll_opts) {
        nopoll_conn_opts_unref(nopoll_opts);
    }

    ins->nopoll_conn = NULL;
    ins->nopoll_ctx = NULL;

    return -1;
}

static int lisa_websocket_nopoll_send_recv_proc(lisa_ws_t *ins)
{
    bool ping_outstanding = false;
    ins->last_ping_sent_ms = lisa_os_get_tick_ms();
    ins->ping_pong_lost_cnt = 0;
    ins->pending_write.active = false;
    ins->pending_write.since_ms = 0U;

    while (1) {
        int err;
        int send_cost_time = 0;
        if (lisa_websocket_need_to_exit(ins)) {
            LISA_NLOGE("received exit sem, exiting send recv process");
            break;
        }

        if (!nopoll_conn_is_ok(ins->nopoll_conn)) {
            /* maybe the conn closed by server */
            LISA_NLOGE("invalid nopoll conn");
            break;
        }
        send_cost_time = lisa_os_get_tick_ms();
        /* min block time: 20ms */
        err = lisa_websocket_msg_send_proc(ins, ins->nopoll_conn);
        send_cost_time = lisa_os_get_tick_ms() - send_cost_time;
        lisa_ws_note_max(&ins->transport.send_max_ms, (uint32_t)send_cost_time);
        if (err != 0) {
            LISA_NLOGE("websocket send path failed, disconnect for recovery");
            break;
        }
        if (lisa_websocket_need_to_exit(ins)) {
            break;
        }
        uint32_t receive_started = lisa_os_get_tick_ms();
        lisa_websocket_msg_recv_proc(ins, ins->nopoll_conn);
        lisa_ws_note_max(&ins->transport.receive_max_ms, lisa_os_get_tick_ms() - receive_started);

        /* nopoll_conn_get_msg() is non-blocking. Yield so Wi-Fi maintenance
         * and DHCP tasks can run while the socket has no traffic. */
        lisa_thread_mdelay(1);

#if CONFIG_NOPOLL_DELIVER_PONG_FRAME_ENABLED
        /* A ping owns one fixed deadline. Sending another ping must never
         * postpone a missing pong; only a received pong resets the retry count. */
        uint32_t now_ms = lisa_os_get_tick_ms();
        err = lisa_semaphore_take(ins->pong_sem, 0);
        if (err == 0) {
            ping_outstanding = false;
            ins->ping_pong_lost_cnt = 0;
        } else if (ping_outstanding &&
                   (uint32_t)(now_ms - ins->last_ping_sent_ms) >= ins->pong_max_time_ms) {
            LISA_NLOGE("recv websocket pong msg timeout, curr:%lu, ping:%lu, timeout:%lu, lost cnt:%d",
                       (unsigned long)now_ms, (unsigned long)ins->last_ping_sent_ms,
                       (unsigned long)ins->pong_max_time_ms, ins->ping_pong_lost_cnt);
            if (++ins->ping_pong_lost_cnt >= WS_PINGPONG_RETRY_CNT) {
                break;
            }
            ping_outstanding = false;
        }
        if (!ping_outstanding && nopoll_conn_pending_write_bytes(ins->nopoll_conn) == 0 &&
            (uint32_t)(now_ms - ins->last_ping_sent_ms) >= ins->pong_max_time_ms) {
            bool sent = nopoll_conn_send_ping(ins->nopoll_conn);
            if (!sent && nopoll_conn_pending_write_bytes(ins->nopoll_conn) == 0) {
                LISA_NLOGE("websocket ping send failed without a pending frame");
                break;
            }
            /* A partially written ping is already owned by noPoll. The normal
             * pending writer finishes it; never enqueue a duplicate ping. */
            ins->last_ping_sent_ms = lisa_os_get_tick_ms();
            ping_outstanding = true;
        }
#endif
    }

    LISA_NLOGI("websocket send recv process exiting...");
    __atomic_store_n(&ins->ws_stop, true, __ATOMIC_RELEASE);
    ins->pending_write.active = false;
    ins->pending_write.since_ms = 0U;

    /* drop all msg */
    lisa_ws_msg_t msg;
    while (lisa_queue_pop(ins->tx_msg_queue, &msg, sizeof(lisa_ws_msg_t), 0) == LISA_OK) {
        if (msg.m_msg != NULL) {
            lisa_mem_free(msg.m_msg);
        }
        memset(&msg, 0, sizeof(lisa_ws_msg_t));
    }

    /* release nopoll conn */
    if (ins->nopoll_conn != NULL) {
        nopoll_conn_close(ins->nopoll_conn);
        ins->nopoll_conn = NULL;
    }

    if (ins->nopoll_ctx) {
        nopoll_ctx_unref(ins->nopoll_ctx);
        ins->nopoll_ctx = NULL;
    }

    _callback_ws_disconnect(ins);

    return 0;
}

static void lisa_websocket_thread(void *param)
{
    lisa_ws_t *ins = (lisa_ws_t *)param;
    int r;

    while (1) {
        LISA_NLOGI("websocket thread waiting for connect sem");

        while (1) {
            /* wait connect or exit signal */
            r = lisa_semaphore_take(ins->connect_sem, 50);
            if (r == 0) {
                LISA_NLOGI("Got connect signal");
                break;
            }

            r = lisa_semaphore_take(ins->exiting_sem, 50);
            if (r == 0) {
                LISA_NLOGI("Got exit signal");
                lisa_semaphore_give(ins->exited_sem);
                LISA_NLOGI("websocket thread waiting for connect sem");
            }
        }

        if (!__atomic_load_n(&ins->connect_cancel, __ATOMIC_ACQUIRE)) {
            lisa_ws_event_t ws_event;
            ws_event.what = LISA_WS_ON_CONNECTING;
            ws_event.user = ins->user;
            ins->inter_on_event(&ws_event);

            if (lisa_websocket_nopoll_conn(ins) == 0) {
                if (!__atomic_load_n(&ins->connect_cancel, __ATOMIC_ACQUIRE)) {
                    _callback_ws_connected(ins);
                }
                /* Also releases a connection cancelled just after handshake. */
                lisa_websocket_nopoll_send_recv_proc(ins);
            } else {
                _callback_ws_disconnect(ins);
            }
        }
        /* This is the only completion point, including failed/cancelled
         * handshakes. Publish idle LAST: cleanup may delete this idle worker
         * as soon as it sees false; no owned noPoll resource/callback remains. */
        lisa_semaphore_give(ins->exited_sem);
        __atomic_store_n(&ins->worker_busy, false, __ATOMIC_RELEASE);
    }
}

lisa_ws_err_e lisa_ws_connect(lisa_ws_t *ins)
{
    if (!ins) {
        return LISA_WS_COMMON_ERR;
    }
    bool expected = false;
    if (__atomic_load_n(&ins->cleanup_requested, __ATOMIC_ACQUIRE) || ins->ws_conn ||
        !__atomic_compare_exchange_n(&ins->worker_busy, &expected, true, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        LISA_NLOGE("websocket already running");
        return LISA_WS_COMMON_ERR;
    }

    __atomic_store_n(&ins->ws_stop, false, __ATOMIC_RELEASE);
    __atomic_store_n(&ins->connect_cancel, 0, __ATOMIC_RELEASE);
    lisa_semaphore_reset(ins->exiting_sem);
    lisa_semaphore_reset(ins->exited_sem);
    lisa_semaphore_give(ins->connect_sem);

    return LISA_WS_OK;
}

lisa_ws_err_e lisa_ws_disconnect(lisa_ws_t *ins)
{
    if (ins == NULL) {
        return LISA_WS_COMMON_ERR;
    }

    __atomic_store_n(&ins->ws_stop, true, __ATOMIC_RELEASE);
    __atomic_store_n(&ins->connect_cancel, 1, __ATOMIC_RELEASE);
    lisa_semaphore_give(ins->exiting_sem);
    if (ins->thread && ins->thread->handle == (lisa_threadhandle_t)xTaskGetCurrentTaskHandle()) {
        /* A callback may request close, but cannot join its own stack. */
        return LISA_WS_BUSY;
    }
    uint32_t started = lisa_os_get_tick_ms();
    while (__atomic_load_n(&ins->worker_busy, __ATOMIC_ACQUIRE)) {
        if ((uint32_t)(lisa_os_get_tick_ms() - started) >= 10U * 1000U) {
            LISA_NLOGE("websocket close pending: worker still owns handshake/resources");
            return LISA_WS_BUSY;
        }
        (void)lisa_semaphore_take(ins->exited_sem, 10U);
    }

    LISA_NLOGI("websocket disconnect done.");

    return LISA_WS_OK;
}

static int lisa_websocket_msg_send_to_thread(lisa_ws_t *ins, int type, const char *msg, uint32_t len)
{
    if (ins == NULL || !ins->ws_conn || __atomic_load_n(&ins->ws_stop, __ATOMIC_ACQUIRE)) {
        return LISA_WS_DISCONNECT;
    }

    lisa_ws_msg_t chunk = {0};
    chunk.m_size = len;
    chunk.m_type = type;
    chunk.m_msg = lisa_mem_alloc(len + 1);
    if (chunk.m_msg == NULL) {
        LISA_NLOGE("send txt malloc failed!");
        return LISA_WS_COMMON_ERR;
    }

    memcpy(chunk.m_msg, msg, len);
    chunk.m_msg[len] = 0;

    lisa_err_t st = lisa_queue_push(ins->tx_msg_queue, &chunk, sizeof(lisa_ws_msg_t), 0);
    if (st != LISA_OK) {
        lisa_mem_free(chunk.m_msg);
        __atomic_add_fetch(&ins->transport.queue_full, 1U, __ATOMIC_RELAXED);
        return LISA_WS_WOULD_BLOCK;
    }

    __atomic_add_fetch(&ins->transport.enqueued_frames, 1U, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ins->transport.enqueued_bytes, len, __ATOMIC_RELAXED);
    lisa_ws_note_max(&ins->transport.queue_peak, lisa_queue_waiting(ins->tx_msg_queue));
    return LISA_WS_OK;
}

lisa_ws_err_e lisa_ws_send_text(lisa_ws_t *ins, const uint8_t *text)
{
    if (text == NULL) {
        return -1;
    }

    return lisa_websocket_msg_send_to_thread(ins, LISA_WS_TEXT, text, strlen(text));
}

lisa_ws_err_e lisa_ws_send_binary(lisa_ws_t *ins, const void *buf, uint32_t len)
{
    if (ins == NULL || !ins->ws_conn || __atomic_load_n(&ins->ws_stop, __ATOMIC_ACQUIRE)) {
        return LISA_WS_DISCONNECT;
    }

    if (!buf || !len) return LISA_WS_COMMON_ERR;

    if (lisa_queue_waiting(ins->tx_msg_queue) >= WS_QUEUE_COUNT - 5) {
        __atomic_add_fetch(&ins->transport.queue_full, 1U, __ATOMIC_RELAXED);
        return LISA_WS_WOULD_BLOCK;
    }

    return lisa_websocket_msg_send_to_thread(ins, LISA_WS_BIN, buf, len);
}

void lisa_ws_get_transport_stats(lisa_ws_t *ins, lisa_ws_transport_stats_t *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!ins) return;
#define WS_STAT(field) stats->field = __atomic_load_n(&ins->transport.field, __ATOMIC_RELAXED)
    WS_STAT(enqueued_frames); WS_STAT(enqueued_bytes); WS_STAT(written_bytes); WS_STAT(completed_frames);
    WS_STAT(queue_peak); WS_STAT(queue_full); WS_STAT(pending_bytes);
    WS_STAT(send_max_ms); WS_STAT(receive_max_ms);
#undef WS_STAT
    stats->queue_current = lisa_queue_waiting(ins->tx_msg_queue);
}

void lisa_ws_get_send_backlog_on_worker(lisa_ws_t *ins, unsigned int *queued_messages,
                                      unsigned int *pending_bytes)
{
    if (queued_messages) {
        *queued_messages = ins && ins->tx_msg_queue ? lisa_queue_waiting(ins->tx_msg_queue) : 0U;
    }
    if (pending_bytes) {
        int pending = ins && ins->nopoll_conn ? nopoll_conn_pending_write_bytes(ins->nopoll_conn) : 0;
        *pending_bytes = pending > 0 ? (unsigned int)pending : 0U;
    }
}

lisa_ws_err_e lisa_ws_cleanup(lisa_ws_t *ins)
{
    if (ins == NULL) {
        return LISA_WS_COMMON_ERR;
    }

    __atomic_store_n(&ins->cleanup_requested, true, __ATOMIC_RELEASE);
    lisa_ws_err_e result = lisa_ws_disconnect(ins);
    if (result != LISA_WS_OK) {
        /* Never delete a task inside DNS/TLS or free its callback owner.
         * The caller retains this handle and retries once the worker retires. */
        return result;
    }

    if (ins->thread) {
        lisa_thread_delete(ins->thread);
        ins->thread = NULL;
    }

    if (ins->tx_msg_queue) {
        /* drop all msg */
        lisa_ws_msg_t msg;
        while (lisa_queue_pop(ins->tx_msg_queue, &msg, sizeof(lisa_ws_msg_t), 0) == LISA_OK) {
            if (msg.m_msg != NULL) {
                lisa_mem_free(msg.m_msg);
            }
            memset(&msg, 0, sizeof(lisa_ws_msg_t));
        }
        lisa_queue_delete(ins->tx_msg_queue);
        ins->tx_msg_queue = NULL;
    }

    if (ins->exiting_sem) {
        lisa_semaphore_delete(ins->exiting_sem);
        ins->exiting_sem = NULL;
    }

    if (ins->exited_sem) {
        lisa_semaphore_delete(ins->exited_sem);
        ins->exited_sem = NULL;
    }

    if (ins->pong_sem) {
        lisa_semaphore_delete(ins->pong_sem);
        ins->pong_sem = NULL;
    }
    if (ins->connect_sem) {
        lisa_semaphore_delete(ins->connect_sem);
        ins->connect_sem = NULL;
    }

    lisa_websocket_rx_msg_reset(ins);
    lisa_mem_free(ins);

    return LISA_WS_OK;
}

lisa_ws_err_e lisa_ws_cfg_set(lisa_ws_t *ws, lisa_ws_request_t *req)
{
    if (ws == NULL || req == NULL || ws->ws_conn) {
        return -1;
    }

    strcpy((char *)ws->u_info.scheme, req->scheme);
    strcpy((char *)ws->u_info.host, req->host);
    strcpy((char *)ws->u_info.path, req->path);
    strcpy((char *)ws->u_info.port, req->port);

    ws->user = req->user;
    ws->inter_on_event = req->on_event;
    ws->timeout = req->timeout;
    ws->extra_header = req->extra_header;
    ws->inter_on_data = req->on_data;

    return 0;
}

lisa_ws_err_e lisa_ws_cfg_get(lisa_ws_t *ws, lisa_ws_request_t *req)
{
    if (ws == NULL || req == NULL) {
        return -1;
    }

    req->scheme = (char *)ws->u_info.scheme;
    req->host = (char *)ws->u_info.host;
    req->path = (char *)ws->u_info.path;
    req->port = (char *)ws->u_info.port;

    req->user = ws->user;
    req->on_event = ws->inter_on_event;
    req->timeout = ws->timeout;
    req->on_data = ws->inter_on_data;

    return 0;
}
