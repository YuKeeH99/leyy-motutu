#include <stdlib.h>
#include <string.h>
#include "lingxin_hook_websocket.h"
#include "lingxin_system_time.h"
#include "lingxin_memory.h"
#include "lingxin_trace.h"
#include "lingxin_thread.h"

// 临时放到这里，防止影响客户适配
static long last_websocket_active_time = 0;

// 临时放到这里，防止影响客户适配
static bool should_freeze = false;
void freeze_websocket(bool freeze)
{
    should_freeze = freeze;
}
// 临时放到这里，防止影响客户适配
bool get_websocket_freeze_status(void)
{
    return should_freeze;
}
// 临时放到这里，防止影响客户适配
void set_websocket_freeze_status(bool status)
{
    should_freeze = status;
}

static void on_hook_websocket_event(WebSocketEventType event, const char *data, const size_t len, const int is_binary, void *user_context)
{
  WebsocketConfig *raw_config = (WebsocketConfig *)user_context;
  if (!raw_config)
  {
    return;
  }
  last_websocket_active_time = lingxin_get_timestamp_s();
  raw_config->listener(event, data, len, is_binary, raw_config->userContext);
  if (event == ON_WEBSOCKET_DESTROY)
  {
    lingxin_free(raw_config);
    raw_config = NULL;
  }
}

WebsocketClient *hook_websocket_init(WebsocketConfig *raw_config)
{
  lingxin_trace_set("hook_ws:init_enter");
  WebsocketConfig *hook_config = (WebsocketConfig *)lingxin_calloc(1, sizeof(WebsocketConfig));
  if (!hook_config)
  {
    lingxin_trace_set("hook_ws:init_alloc_fail");
    return NULL;
  }
  memcpy(hook_config, raw_config, sizeof(WebsocketConfig));
  hook_config->listener = on_hook_websocket_event;
  hook_config->userContext = raw_config;
  WebsocketClient *client = initWebsocket(hook_config);
  lingxin_trace_set(client ? "hook_ws:init_ret_ok" : "hook_ws:init_ret_fail");
  return client;
}

bool hook_websocket_start(WebsocketClient *client)
{
  lingxin_trace_set("hook_ws:start_enter");
  bool ret = startWebsocket(client);
  lingxin_trace_set(ret ? "hook_ws:start_ret_ok" : "hook_ws:start_ret_fail");
  return ret;
}

bool hook_websocket_send_text(WebsocketClient *client, const char *message)
{
  if (websocketSendText(client, message))
  {
    last_websocket_active_time = lingxin_get_timestamp_s();
    return true;
  }
  return false;
}

int hook_websocket_send_binary(WebsocketClient *client, const char *audio, size_t size)
{
  int result = websocketSendBinary(client, audio, size);
  if (result > 0)
  {
    last_websocket_active_time = lingxin_get_timestamp_s();
  }
  return result;
}

int hook_websocket_send_binary_guarded(WebsocketClient *client, const char *audio, size_t size,
                                     bool (*current)(void *), void *guard_arg)
{
  int result = websocketSendBinaryGuarded(client, audio, size, current, guard_arg);
  if (result > 0) last_websocket_active_time = lingxin_get_timestamp_s();
  return result;
}

bool hook_websocket_close(WebsocketClient *client)
{
  return closeWebsocket(client);
}

typedef struct {
  WebsocketClient *client;
  void (*completed)(void *);
  void *user;
} websocket_close_job_t;

static void *websocket_close_worker(void *arg)
{
  websocket_close_job_t *job = (websocket_close_job_t *)arg;
  while (!closeWebsocket(job->client))
  {
    /* A blocked resolver keeps its stack and callback owner until it
     * returns. One retained job retries; repeated wakes cannot spawn more. */
    lingxin_thread_sleep(20);
  }
  if (job->completed)
  {
    job->completed(job->user);
  }
  /* Completion may free client and user; never touch either again. The
   * client's pending flag stays set through destruction to deduplicate. */
  lingxin_free(job);
  return NULL;
}

bool hook_websocket_close_async(WebsocketClient *client, void (*completed)(void *), void *user)
{
  if (!completed) return false;
  bool expected = false;
  if (client && !__atomic_compare_exchange_n(&client->close_retry_pending, &expected, true,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
  {
    return true;
  }
  if (client) __atomic_store_n(&client->closing, true, __ATOMIC_RELEASE);
  websocket_close_job_t *job = lingxin_calloc(1, sizeof(*job));
  if (!job)
  {
    if (client) __atomic_store_n(&client->close_retry_pending, false, __ATOMIC_RELEASE);
    return false;
  }
  job->client = client;
  job->completed = completed;
  job->user = user;
  lingxin_tid_t thread_id = 0;
  lingxin_thread_param_t param = {.name = "lx_ws_close", .priority = 0, .stack_size = 3072};
  if (lingxin_thread_create(&thread_id, &param, websocket_close_worker, job) != 0)
  {
    lingxin_free(job);
    if (client) __atomic_store_n(&client->close_retry_pending, false, __ATOMIC_RELEASE);
    return false;
  }
  return true;
}

/**
 * 这里判断的是板子上当前所有的websocket实例，是否进入空闲状态
 * 距离上次数据收发（ping、pong除外）超过5秒，则认为websocket处于空闲状态
 */
bool is_all_websocket_idle()
{
  return (lingxin_get_timestamp_s() - last_websocket_active_time) > 20;
}
