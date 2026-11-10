#ifndef LINGXIN_WEBSOCKET_H
#define LINGXIN_WEBSOCKET_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

  typedef enum
  {
    ON_WEBSOCKET_CONNECTION_SUCCESS, // 建联成功
    ON_WEBSOCKET_CONNECTION_FAIL, // 建联失败
    ON_WEBSOCKET_DATA_RECEIVED, // 收到数据
    ON_WEBSOCKET_DESTROY,  // 销毁完成
    ON_WEBSOCKET_ERROR  // 收到错误
  } WebSocketEventType;

  typedef void (*WebSocketEventListener)(WebSocketEventType event, const char *data, size_t data_len, const int isBinary, void *userContext);

  typedef struct
  {
    const char *protocol; // ws or wss
    const char *host;
    const char *path;
    const char *header_signature;
    const char *header_sn;
    const char *header_app_id;
    const char *header_timestamp;
    int port;
    void *userContext;
    WebSocketEventListener listener;
  } WebsocketConfig;

  typedef struct
  {
    void *clientHandler;
    WebsocketConfig *config;
    volatile unsigned int active_operations;
    volatile bool closing;
    volatile bool close_active;
    volatile bool close_retry_pending;
  } WebsocketClient;

  WebsocketClient *initWebsocket(WebsocketConfig *config);

  bool startWebsocket(WebsocketClient *client);

  bool websocketSendText(WebsocketClient *client, const char *message);

  int websocketSendBinary(WebsocketClient *client, const char *audioData, size_t dataSize);
  /* The guard is checked before each attempt, including after backpressure.
   * A cancelled frame is not an I/O failure and must not fail a newer task. */
#define WEBSOCKET_SEND_CANCELLED (-2)
  int websocketSendBinaryGuarded(WebsocketClient *client, const char *audioData, size_t dataSize,
                                bool (*current)(void *), void *guard_arg);
  typedef struct {
    uint32_t enqueued_frames, enqueued_bytes, written_bytes, completed_frames;
    uint32_t queue_current, queue_peak, queue_full, pending_bytes, send_max_ms, receive_max_ms;
    uint32_t wait_count, wait_total_ms, wait_max_ms, wait_timeout, wait_cancel;
    bool connected;
  } WebsocketTransportStats;
  /* Scalar observations, including last closed socket when client is NULL. */
  void websocketGetTransportStats(WebsocketClient *client, WebsocketTransportStats *stats);

  /* Only from this client's receive callback, never from a shell/other task. */
  void websocketGetSendBacklogOnReceive(WebsocketClient *client, unsigned int *queued_messages,
                                      unsigned int *pending_bytes);

  /* false retains clientHandler and every callback owner; retry later.
   * true means the transport worker has fully retired and caller may free. */
  bool closeWebsocket(WebsocketClient *client);

  /** Release the stable client wrapper after closeWebsocket detached and
   * drained the transport. This is separate from close so re-entrant callbacks
   * never observe a freed API handle. */
  void destroyWebsocketClient(WebsocketClient *client);

#ifdef __cplusplus
}
#endif

#endif // LINGXIN_WEBSOCKET_H
