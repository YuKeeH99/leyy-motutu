#include "audio_buffer_play.h"
#include "lingxin_protocol_manager.h"
#include "chat_state_machine.h"
#include "lingxin_voice_chat_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include "lingxin_semaphore.h"
#include "lingxin_common.h"
#include "lingxin_log.h"
#include "cJSON.h"
#include "lingxin_json_util.h"
#include "lingxin_hook_websocket.h"
#include "lingxin_user_track.h"
#include "lingxin_system_time.h"
#include "lingxin_memory.h"
#include "lingxin_thread.h"
#include "lingxin_test_runner.h"
#include "lingxin_server.h"
#include "lingxin_base64_utils.h"
#include "lingxin_trace.h"
#include "lingxin_chat_upload_manager.h"

#define MAX_LISTENERS 10 // 定义最大监听器数量
#ifdef CONFIG_LINGXIN_START_TASK_ACK_TIMEOUT_MS
#define LINGXIN_START_TASK_ACK_TIMEOUT_MS ((uint32_t)CONFIG_LINGXIN_START_TASK_ACK_TIMEOUT_MS)
#else
#define LINGXIN_START_TASK_ACK_TIMEOUT_MS 15000U
#endif
#define LINGXIN_TERMINATE_ACK_TIMEOUT_MS 5000U
#define LINGXIN_FIRST_TTS_TIMEOUT_MS      30000U
#define LINGXIN_END_AUDIO_ACK_TIMEOUT_MS  5000U
#define LINGXIN_PROTOCOL_WATCHDOG_POLL_MS 250U
#define LINGXIN_HANDLER_DRAIN_TIMEOUT_MS  2500U

struct VoiceChatHandler
{
  VoiceChatConfig *chat_config;
  WebsocketClient *websocket;
  bool waitTerminate;
  VoiceChatContextInfo *contextInfo;
  struct VoiceChatHandler **selfPointer; // 存储双指针的引用
  bool need_free_taskId;
  volatile unsigned int active_operations;
  volatile bool closing;
  bool close_requested; // guarded by s_handler_lifecycle_guard
};
static int global_chat_listener_count = 0;
static ChatEventListener global_chat_listeners[MAX_LISTENERS];
static VoiceChatHandler *globalHandler = NULL;
static volatile bool s_handler_lifecycle_guard = false;
static volatile bool s_handler_close_finalizing = false;

static bool isAIResponseding = false; // 已给服务端发请求，带响应，或者 已在响应，YES 才能发打断
static bool isAudioSending = false;
static bool isWebsocketConnected = false;
static bool isTaskStarted = false;
static bool s_audio_response_open = false;
static bool s_task_audio_input;
static bool waitTerminateOrEndSuccess = false;
static volatile uint32_t s_connection_failure_pending_epoch = 0U;
static volatile uint32_t s_recovery_epoch = 1U;
static uint32_t s_diag_upload_frame_count = 0;
static uint32_t s_diag_upload_bytes_total = 0;
static uint32_t s_diag_download_audio_chunk_count = 0;
static uint32_t s_diag_download_audio_bytes_total = 0;
static uint32_t s_diag_binary_audio_send_count = 0;
static uint32_t s_diag_binary_audio_send_bytes = 0;
static uint32_t s_diag_binary_audio_drop_before_task_started = 0;
static uint32_t s_diag_start_task_seq = 0;
static long s_diag_start_task_sent_at_s = 0;
static bool s_diag_waiting_task_started = false;
static uint32_t s_start_task_watchdog_seq = 0;
static uint32_t s_terminate_watchdog_seq = 0;
static uint32_t s_first_tts_watchdog_seq = 0;
static bool s_waiting_task_terminated = false;
static bool s_waiting_first_tts = false;
static volatile bool s_protocol_watchdog_started = false;
static volatile uint32_t s_start_task_deadline_ms = 0U;
static volatile uint32_t s_terminate_deadline_ms = 0U;
static volatile uint32_t s_first_tts_deadline_ms = 0U;
static volatile uint32_t s_start_task_watchdog_epoch = 0U;
static volatile uint32_t s_terminate_watchdog_epoch = 0U;
static volatile uint32_t s_first_tts_watchdog_epoch = 0U;
static bool s_end_audio_sent = false;
static uint32_t s_record_cue_epoch;
static bool s_record_cue_suppressed;
static volatile bool s_audio_ended_received = false;
static volatile bool s_task_stage_end_pending = false;
static volatile bool s_end_audio_ack_grace_expired = false;
static volatile uint32_t s_end_audio_ack_deadline_ms = 0U;
static volatile uint32_t s_end_audio_ack_watchdog_epoch = 0U;
static volatile uint32_t s_response_audio_epoch;
static bool s_end_task_sent = false;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
/* v2 Cloud VAD owns the upload boundary. Audio is accepted only between
 * task_started and the first vad_end/vad_exit for the active task. */
static bool s_cloud_vad_upload_allowed = false;
static bool s_cloud_vad_close_requested = false;
static uint32_t s_cloud_vad_boundary_epoch;
#endif

static void freeVoiceChat(VoiceChatHandler **handlerAddress);
static void notify_audio_upload_acked(VoiceChatHandler *handler)
{
  (void)handler;
  s_audio_ended_received = true;
}
static void notify_recording_ended(uint32_t epoch)
{
#if defined(CONFIG_BOARD_ARCS_MINI_V2_1) && defined(CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX)
  for (;;) {
    /* Read the claim before validating epoch. An old caller cannot consume
     * the new round's claim after start_task; no reset-to-false ABA. */
    uint32_t previous = __atomic_load_n(&s_record_cue_epoch, __ATOMIC_ACQUIRE);
    if (previous == epoch || epoch != __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE) ||
        !globalHandler || globalHandler->waitTerminate || s_record_cue_suppressed ||
        !s_diag_binary_audio_send_bytes) return;
    if (__atomic_compare_exchange_n(&s_record_cue_epoch, &previous, epoch, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
  }
  extern void voice_player_notify_upload_complete(uint32_t epoch);
  lingxin_log_ut_with_args(LINGXIN_DEBUG, "input_boundary_cue",
      "source=%s epoch=%u task_id=%s tx_bytes=%u",
      "cloud_vad_end", epoch,
      globalHandler->chat_config ? globalHandler->chat_config->taskId : "",
      s_diag_binary_audio_send_bytes);
  voice_player_notify_upload_complete(epoch);
#else
  (void)epoch;
#endif
}
static void on_voice_chat_init_fail(void);
static void on_voice_chat_init_fail_for_epoch(uint32_t epoch);

static void voice_chat_handler_guard_lock(void)
{
  while (__atomic_test_and_set(&s_handler_lifecycle_guard, __ATOMIC_ACQUIRE))
  {
    lingxin_thread_sleep(1);
  }
}

static void voice_chat_handler_guard_unlock(void)
{
  __atomic_clear(&s_handler_lifecycle_guard, __ATOMIC_RELEASE);
}

static VoiceChatHandler *voice_chat_handler_acquire(void)
{
  VoiceChatHandler *handler = NULL;
  voice_chat_handler_guard_lock();
  if (globalHandler && !__atomic_load_n(&globalHandler->closing, __ATOMIC_ACQUIRE))
  {
    handler = globalHandler;
    (void)__atomic_add_fetch(&handler->active_operations, 1U, __ATOMIC_ACQUIRE);
  }
  voice_chat_handler_guard_unlock();
  return handler;
}

static void voice_chat_handler_release(VoiceChatHandler *handler)
{
  if (handler)
  {
    (void)__atomic_sub_fetch(&handler->active_operations, 1U, __ATOMIC_RELEASE);
  }
}

static VoiceChatHandler *voice_chat_handler_begin_close(bool *already_pending)
{
  VoiceChatHandler *handler = NULL;
  *already_pending = false;
  voice_chat_handler_guard_lock();
  if (globalHandler && globalHandler->selfPointer)
  {
    if (globalHandler->close_requested)
    {
      *already_pending = true;
    }
    else
    {
      handler = globalHandler;
      handler->close_requested = true;
      __atomic_store_n(&handler->closing, true, __ATOMIC_RELEASE);
    }
  }
  voice_chat_handler_guard_unlock();
  return handler;
}

void setWaitTerminateOrEndSuccess(bool target)
{
  waitTerminateOrEndSuccess = target;
}

static char *getReqId(VoiceChatHandler *handler)
{
  return (handler && handler->contextInfo && handler->contextInfo->requestId) ? handler->contextInfo->requestId : "";
}

static bool has_valid_request_id(VoiceChatHandler *handler)
{
  const char *req_id = getReqId(handler);
  return req_id != NULL && req_id[0] != '\0';
}

// #region debug-point A:start-task-ack
static void diag_note_start_task_sent(VoiceChatHandler *handler, const char *path)
{
  s_diag_start_task_seq++;
  s_diag_start_task_sent_at_s = lingxin_get_timestamp_s();
  s_diag_waiting_task_started = true;
  lingxin_server_diag_timing_mark("S03_start_task_sent", (int)s_diag_start_task_seq);
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_diag_start_task_sent",
                           "seq=%u, path=%s, ws_connected=%d, handler=%p, request_id:%s",
                           s_diag_start_task_seq,
                           path ? path : "",
                           isWebsocketConnected ? 1 : 0,
                           handler,
                           getReqId(handler));
}

static void diag_note_task_started_wait_end(VoiceChatHandler *handler, const char *reason)
{
  long now_s = lingxin_get_timestamp_s();
  long elapsed_s = (s_diag_start_task_sent_at_s > 0) ? (now_s - s_diag_start_task_sent_at_s) : -1;
  lingxin_server_diag_timing_mark((reason && strcmp(reason, "task_started") == 0)
                                      ? "S04_task_started"
                                      : (reason ? reason : "task_started_wait_end"),
                                  (int)elapsed_s);
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_diag_task_started_wait_end",
                           "seq=%u, reason=%s, elapsed_s=%ld, ws_connected=%d, request_id:%s",
                           s_diag_start_task_seq,
                           reason ? reason : "",
                           elapsed_s,
                           isWebsocketConnected ? 1 : 0,
                           getReqId(handler));
  s_diag_waiting_task_started = false;
}
// #endregion

static bool protocol_watchdog_expired(volatile uint32_t *remaining_ms)
{
  uint32_t remaining = __atomic_load_n(remaining_ms, __ATOMIC_ACQUIRE);
  while (remaining != 0U)
  {
    uint32_t next = remaining <= LINGXIN_PROTOCOL_WATCHDOG_POLL_MS
                        ? 0U
                        : remaining - LINGXIN_PROTOCOL_WATCHDOG_POLL_MS;
    if (__atomic_compare_exchange_n(remaining_ms, &remaining, next, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
      return next == 0U;
    }
  }
  return false;
}

static void *protocol_watchdog_thread(void *arg)
{
  (void)arg;

  while (1)
  {
    lingxin_thread_sleep(LINGXIN_PROTOCOL_WATCHDOG_POLL_MS);
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    lingxin_chat_upload_manager_poll_record_limit();
#endif

    if (s_diag_waiting_task_started && !isTaskStarted &&
        protocol_watchdog_expired(&s_start_task_deadline_ms))
    {
      uint32_t seq = s_start_task_watchdog_seq;
      s_diag_waiting_task_started = false;
      lingxin_trace_set("chat:start_task_ack_timeout");
      lingxin_log_ut_with_args(LINGXIN_WARN,
                               "chat_manager_start_task_timeout",
                               "seq=%u, timeout_ms=%u, ws_connected=%d, ai_responding=%d, request_id:%s",
                               seq,
                               (unsigned)LINGXIN_START_TASK_ACK_TIMEOUT_MS,
                               isWebsocketConnected ? 1 : 0,
                               isAIResponseding ? 1 : 0,
                               getReqId(globalHandler));
      on_voice_chat_init_fail_for_epoch(s_start_task_watchdog_epoch);
      continue;
    }

    if (s_waiting_task_terminated && protocol_watchdog_expired(&s_terminate_deadline_ms))
    {
      uint32_t seq = s_terminate_watchdog_seq;
      s_waiting_task_terminated = false;
      s_waiting_first_tts = false;
      __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
      lingxin_trace_set("chat:terminate_task_ack_timeout");
      lingxin_log_ut_with_args(LINGXIN_ERROR,
                               "chat_manager_terminate_timeout",
                               "seq=%u, timeout_ms=%u, ws_connected=%d",
                               seq,
                               (unsigned)LINGXIN_TERMINATE_ACK_TIMEOUT_MS,
                               isWebsocketConnected ? 1 : 0);
      on_voice_chat_init_fail_for_epoch(s_terminate_watchdog_epoch);
      continue;
    }

    if (s_waiting_first_tts && protocol_watchdog_expired(&s_first_tts_deadline_ms))
    {
      uint32_t seq = s_first_tts_watchdog_seq;
      s_waiting_first_tts = false;
      lingxin_trace_set("chat:first_tts_timeout");
      lingxin_log_ut_with_args(LINGXIN_ERROR,
                               "chat_manager_first_tts_timeout",
                               "seq=%u, timeout_ms=%u, ws_connected=%d, task_started=%d",
                               seq,
                               (unsigned)LINGXIN_FIRST_TTS_TIMEOUT_MS,
                               isWebsocketConnected ? 1 : 0,
                               isTaskStarted ? 1 : 0);
      on_voice_chat_init_fail_for_epoch(s_first_tts_watchdog_epoch);
    }

    if (s_end_audio_sent && !s_audio_ended_received &&
        protocol_watchdog_expired(&s_end_audio_ack_deadline_ms))
    {
      s_end_audio_ack_grace_expired = true;
      s_task_stage_end_pending = false;
      lingxin_trace_set("chat:end_audio_ack_timeout");
      lingxin_log_ut_with_args(LINGXIN_ERROR,
                               "chat_manager_end_audio_ack_timeout",
                               "timeout_ms=%u, ws_connected=%d, task_started=%d",
                               (unsigned)LINGXIN_END_AUDIO_ACK_TIMEOUT_MS,
                               isWebsocketConnected ? 1 : 0,
                               isTaskStarted ? 1 : 0);
      on_voice_chat_init_fail_for_epoch(s_end_audio_ack_watchdog_epoch);
    }
  }
  return NULL;
}

static bool ensure_protocol_watchdog(void)
{
  bool expected = false;
  if (!__atomic_compare_exchange_n(&s_protocol_watchdog_started, &expected, true,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
  {
    return true;
  }

  lingxin_tid_t thread_id = 0;
  lingxin_thread_param_t thread_param = {
      .name = "lx_proto_wd",
      .priority = 0,
      .stack_size = 3072,
  };
  if (lingxin_thread_create(&thread_id, &thread_param, protocol_watchdog_thread, NULL) != 0)
  {
    __atomic_store_n(&s_protocol_watchdog_started, false, __ATOMIC_RELEASE);
    lingxin_log_ut(LINGXIN_ERROR, "chat_manager_protocol_watchdog_thread_fail");
    return false;
  }
  return true;
}

static void arm_start_task_watchdog(void)
{
  s_start_task_watchdog_seq = s_diag_start_task_seq;
  s_start_task_watchdog_epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  __atomic_store_n(&s_start_task_deadline_ms, LINGXIN_START_TASK_ACK_TIMEOUT_MS, __ATOMIC_RELEASE);
  if (!ensure_protocol_watchdog())
  {
    __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);
    on_voice_chat_init_fail();
  }
}

static void arm_terminate_task_watchdog(void)
{
  s_waiting_task_terminated = true;
  s_terminate_watchdog_seq++;
  if (s_terminate_watchdog_seq == 0U)
  {
    s_terminate_watchdog_seq = 1U;
  }
  s_terminate_watchdog_epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  __atomic_store_n(&s_terminate_deadline_ms, LINGXIN_TERMINATE_ACK_TIMEOUT_MS, __ATOMIC_RELEASE);
  if (!ensure_protocol_watchdog())
  {
    s_waiting_task_terminated = false;
    on_voice_chat_init_fail();
  }
}

static void arm_first_tts_watchdog(void)
{
  s_waiting_first_tts = true;
  s_first_tts_watchdog_seq++;
  if (s_first_tts_watchdog_seq == 0U)
  {
    s_first_tts_watchdog_seq = 1U;
  }
  s_first_tts_watchdog_epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  __atomic_store_n(&s_first_tts_deadline_ms, LINGXIN_FIRST_TTS_TIMEOUT_MS, __ATOMIC_RELEASE);
  if (!ensure_protocol_watchdog())
  {
    s_waiting_first_tts = false;
    on_voice_chat_init_fail();
  }
}

static void notify_all_listeners(ChatEventType event, const char *data, const size_t len)
{
  // 遍历所有注册的监听器并调用它们
  for (int i = 0; i < global_chat_listener_count; i++)
  {
    if (global_chat_listeners[i])
    {
      global_chat_listeners[i](event, data, len);
    }
  }
}

#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
/* No reset between tasks: an old timeout cannot claim a new task's boundary.
 * The real VAD event and watchdog arbitrate the same per-epoch terminal slot. */
static bool claim_cloud_vad_boundary(uint32_t epoch)
{
  for (;;) {
    uint32_t previous = __atomic_load_n(&s_cloud_vad_boundary_epoch, __ATOMIC_ACQUIRE);
    if (previous == epoch || epoch != voice_chat_current_epoch() || !isTaskStarted)
      return false;
    if (__atomic_compare_exchange_n(&s_cloud_vad_boundary_epoch, &previous, epoch, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
  }
}
#endif

static void cloud_vad_close_upload_once(ChatEventType close_event, const char *reason)
{
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  uint32_t cue_epoch = voice_chat_current_epoch();
  if (!claim_cloud_vad_boundary(cue_epoch))
  {
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_cloud_vad_close_skip",
                             "reason=%s, task_started=%d, already_closed=%d",
                             reason ? reason : "",
                             isTaskStarted ? 1 : 0,
                             s_cloud_vad_close_requested ? 1 : 0);
    return;
  }

  s_cloud_vad_close_requested = true;
  s_cloud_vad_upload_allowed = false;
  /* VAD_END is the user-visible input boundary, not the later audio_ended
   * acknowledgement. Upload is already disabled, so the cue cannot feed
   * into this task. VAD_EXIT must never sound the input-complete cue. */
  if (close_event == CHAT_EVENT_ON_VAD_EXIT) {
    s_record_cue_suppressed = true;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    extern void voice_player_cancel_upload_cue(uint32_t epoch);
    voice_player_cancel_upload_cue(cue_epoch);
#endif
  } else if (close_event == CHAT_EVENT_ON_VAD_END) {
    notify_recording_ended(cue_epoch);
  }
  lingxin_trace_set("chat:cloud_vad_close_upload");
#ifdef CONFIG_LINGXIN_VERBOSE_LOG
  /* This function is called only from the socket's receive callback, after
   * task-id validation. Snapshot its real backlog before stopping capture. */
  unsigned int queued_messages = 0U, pending_bytes = 0U;
  websocketGetSendBacklogOnReceive(globalHandler ? globalHandler->websocket : NULL,
                                 &queued_messages, &pending_bytes);
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_cloud_vad_close_upload",
                           "event=%d reason=%s tx_bytes=%u ws_queue=%u pending=%u task_id=%s request_id=%s",
                           (int)close_event,
                           reason ? reason : "",
                           (unsigned)s_diag_binary_audio_send_bytes,
                           queued_messages, pending_bytes,
                           globalHandler && globalHandler->chat_config ? globalHandler->chat_config->taskId : "",
                           getReqId(globalHandler));
#endif

  notify_all_listeners(close_event, "", 0);
#else
  (void)close_event;
  (void)reason;
#endif
}

static void on_voice_chat_error(char *error)
{
  lingxin_log_ut_with_args(LINGXIN_ERROR, "on_voice_chat_error", error);
  notify_all_listeners(CHAT_EVENT_ON_ERROR, error, strlen(error));
  lingxin_report_error(error);
}

static bool fill_config_params(VoiceChatConfig *chat_config, ChatStartNewParams *params)
{
  if (!chat_config)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "config is null");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }
  char *appKey = lingxin_auth_license_get();
  if (!appKey || strlen(appKey) == 0)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "appKey is null");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }
  char *sn = lingxin_auth_sn_get();
  if (!sn || strlen(sn) == 0)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "sn is null");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }
  char *appId = lingxin_auth_appId_get();
  if (!appId || strlen(appId) == 0)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "appId is null");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }
  char *agentCode = lingxin_auth_appCode_get();
  if (!agentCode || strlen(agentCode) == 0)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "agentCode is null");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }

  char *custom_parameters = lingxin_chat_custom_parameter_get();
  char *biz_parameters = lingxin_chat_biz_parameter_get();
  const char *task = (params && params->task) ? params->task : "";
  const char *input_mode = (params && params->input_mode) ? params->input_mode : "";
  const char *output_mode = (params && params->output_mode) ? params->output_mode : "";
  const char *schedule_task_id = (params && params->scheduleTaskId) ? params->scheduleTaskId : "";
  const char *user_input = (params && params->user_input) ? params->user_input : "";
  const char *custom_json = (custom_parameters && custom_parameters[0] != '\0') ? custom_parameters : "{}";
  const char *biz_json = (biz_parameters && biz_parameters[0] != '\0') ? biz_parameters : "{}";

#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  chat_config->serverPath = WEBSOCKET_CLOUD_VAD_CHAT_PATH;
#else
  chat_config->serverPath = WEBSOCKET_CHAT_PATH;
#endif
  chat_config->appKey = appKey;
  chat_config->sn = sn;
  chat_config->appId = appId;
  /* A task id belongs to exactly one start_task ... task_ended/terminated
   * cycle. Never reuse the previous handler id for a new start_task. Duplicate
   * the requested id before freeing the old one because params may refer to
   * runtime context derived from that handler. */
  const char *requested_task_id = (params && params->taskId) ? params->taskId : "";
  s_task_audio_input = strcmp(input_mode, "audio") == 0;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  char generated_task_id[160];
  if (!requested_task_id[0]) {
    if (lingxin_next_task_id(generated_task_id, sizeof(generated_task_id)) != 0)
      return false;
    requested_task_id = generated_task_id;
  }
#endif
  char *next_task_id = requested_task_id[0] != '\0'
                           ? lingxin_strdup(requested_task_id)
                           : generateUUID(32);
  if (!next_task_id)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "task id allocate fail");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }
  if (chat_config->taskId)
  {
    lingxin_free(chat_config->taskId);
  }
  chat_config->taskId = next_task_id;
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_task_id_strdup",
                           "src_len=%d, dst_len=%d, dst=%p",
                           (params && params->taskId) ? (int)strlen(params->taskId) : -1,
                           chat_config->taskId ? (int)strlen(chat_config->taskId) : -1,
                           chat_config->taskId);
  lingxin_checkpoint_report_with_string("lingxin_test_task_id_check", (char *)chat_config->taskId);

#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  const char *payload_format = "{\"task\":\"%s\",\"input_mode\":\"%s\",\"output_mode\":\"%s\",\"schedule_task_id\":\"%s\",\"ai_app_code\":\"%s\",\"agent_code\":\"%s\",\"max_sentence_silence\":%d,\"asr_params\":{\"show_asr_output\":true},\"agent_basic_inputs\":{\"user_input\":\"%s\"},\"custom_parameters\":%s,\"biz_parameters\":%s}";
  int length = snprintf(NULL, 0, payload_format, task, input_mode, output_mode, schedule_task_id, agentCode, agentCode, CONFIG_LINGXIN_CLOUD_VAD_MAX_SENTENCE_SILENCE_MS, user_input, custom_json, biz_json);
#else
  const char *vad_str = params->server_vad ? "true" : "false";
  const char *payload_format = "{\"cloud_vad\":%s,\"task\":\"%s\",\"input_mode\":\"%s\",\"output_mode\":\"%s\",\"schedule_task_id\":\"%s\",\"ai_app_code\":\"%s\",\"agent_code\":\"%s\",\"agent_basic_inputs\":{\"user_input\":\"%s\"},\"custom_parameters\":%s,\"biz_parameters\":%s}";
  int length = snprintf(NULL, 0, payload_format, vad_str, task, input_mode, output_mode, schedule_task_id, agentCode, agentCode, user_input, custom_json, biz_json);
#endif
  if (length <= 0)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "payload format error");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }
  char *new_payload = realloc((char *)chat_config->payload, length + 1);
  if (!new_payload)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_get_config_fail", "payload realloc fail");
    core_node_record("chat_manager_get_config_fail");
    return false;
  }
  chat_config->payload = (const char *)new_payload;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  snprintf((char *)chat_config->payload, length + 1, payload_format, task, input_mode, output_mode, schedule_task_id, agentCode, agentCode, CONFIG_LINGXIN_CLOUD_VAD_MAX_SENTENCE_SILENCE_MS, user_input, custom_json, biz_json);
#else
  snprintf((char *)chat_config->payload, length + 1, payload_format, vad_str, task, input_mode, output_mode, schedule_task_id, agentCode, agentCode, user_input, custom_json, biz_json);
#endif
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_start_task_payload",
                           "path=%s, task=%s, input=%s, output=%s, schedule=%s, app_code=%s",
                           chat_config->serverPath,
                           task,
                           input_mode,
                           output_mode,
                           schedule_task_id[0] != '\0' ? schedule_task_id : "<empty>",
                           agentCode);
  // 释放biz_parameters，申请在lingxin_chat_biz_parameter_get中
  lingxin_free(biz_parameters);
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_get_config_finish");
  return true;
}

static void event_send_log(char *event_name, int state, char *args)
{
  switch (state)
  {
  case 1:
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_send_event", "event: %s, status: begin, request_id:%s", event_name, getReqId(globalHandler));
    break;
  case 2:
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_send_event", "event: %s, status: fail, request_id:%s, reason: %s", event_name, getReqId(globalHandler), args ? args : "");
    break;
  case 3:
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_send_event", "event: %s, status: finish, request_id:%s", event_name, getReqId(globalHandler));
    break;
  default:
    break;
  }
}

static bool sendStartTask(VoiceChatHandler *handler)
{
  s_audio_response_open = false;
  s_diag_upload_frame_count = 0;
  s_diag_upload_bytes_total = 0;
  s_diag_download_audio_chunk_count = 0;
  s_diag_download_audio_bytes_total = 0;
  s_diag_binary_audio_send_count = 0;
  s_diag_binary_audio_send_bytes = 0;
  s_diag_binary_audio_drop_before_task_started = 0;
  isTaskStarted = false;
  isAudioSending = false;
  isAIResponseding = true;
  s_waiting_task_terminated = false;
  s_terminate_watchdog_seq++;
  __atomic_store_n(&s_terminate_deadline_ms, 0U, __ATOMIC_RELEASE);
  s_waiting_first_tts = false;
  s_first_tts_watchdog_seq++;
  __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);
  s_end_audio_sent = false;
  s_audio_ended_received = false;
  s_task_stage_end_pending = false;
  s_end_audio_ack_grace_expired = false;
  __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
  s_end_task_sent = false;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  s_cloud_vad_upload_allowed = false;
  s_cloud_vad_close_requested = false;
  s_record_cue_suppressed = false;
#endif
  event_send_log("start_task", 1, NULL);
  if (!handler)
  {
    event_send_log("start_task", 2, "handler null");
    return false;
  }

  if (!handler->chat_config || !handler->chat_config->taskId || !handler->chat_config->payload)
  {
    event_send_log("start_task", 2, "params valid");
    return false;
  }

  // 计算所需缓冲区大小
  char *format_str = "{\"header\":{\"action\":\"start_task\",\"task_id\":\"%s\"},\"payload\":%s}";
  int message_len = snprintf(NULL, 0, format_str, handler->chat_config->taskId, handler->chat_config->payload);
  if (message_len <= 0)
  {
    event_send_log("start_task", 2, "params format fail");
    return false;
  }
  // 动态分配内存
  char *message = (char *)lingxin_malloc(message_len + 1);
  if (!message)
  {
    event_send_log("start_task", 2, "memory allocate fail");
    return false;
  }
  // 格式化消息
  snprintf(message, message_len + 1, format_str, handler->chat_config->taskId, handler->chat_config->payload);
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_start_task_send",
                           "task_id:%s, payload_len:%d",
                           handler->chat_config->taskId,
                           (int)strlen(handler->chat_config->payload));
  // 诊断：定位服务端 1007（非法 UTF-8）关闭的字节来源
  {
    size_t msg_len = strlen(message);
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_msg_len",
                             "task_id_len=%d, payload_len=%d, msg_len=%d",
                             (int)strlen(handler->chat_config->taskId),
                             (int)strlen(handler->chat_config->payload),
                             (int)msg_len);
    for (size_t i = 0; i < msg_len; i++)
    {
      unsigned char c = (unsigned char)message[i];
      if (c >= 0x80)
      {
        char hex_dump[96];
        int n = 0;
        size_t start = (i >= 8) ? i - 8 : 0;
        size_t end = (i + 9 < msg_len) ? i + 9 : msg_len;
        for (size_t j = start; j < end && n < (int)sizeof(hex_dump) - 3; j++)
        {
          n += snprintf(hex_dump + n, sizeof(hex_dump) - (size_t)n, "%02X ", (unsigned char)message[j]);
        }
        lingxin_log_ut_with_args(LINGXIN_ERROR,
                                 "chat_manager_msg_non_ascii",
                                 "offset=%u, byte=0x%02X, msg_len=%u, around=[%s]",
                                 (unsigned)i, c, (unsigned)msg_len, hex_dump);
        break;
      }
    }
  }
  /* Commit the waiting state before enqueueing: the websocket worker may
   * deliver task_started before the send call returns. Never re-arm after ACK. */
  uint32_t epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  diag_note_start_task_sent(handler, isWebsocketConnected ? "reuse_or_connected" : "create_or_connecting");
  arm_start_task_watchdog();
  bool result = hook_websocket_send_text(handler->websocket, message);

  if (!result)
  {
    // #region debug-point A:start-task-send-fail
    lingxin_log_ut_with_args(LINGXIN_WARN,
                             "chat_manager_diag_start_task_send_fail",
                             "seq=%u, ws_connected=%d, handler=%p, request_id:%s",
                             s_diag_start_task_seq + 1U,
                             isWebsocketConnected ? 1 : 0,
                             handler,
                             getReqId(handler));
    // #endregion
    event_send_log("start_task", 2, "send fail");
    on_voice_chat_init_fail_for_epoch(epoch);
    lingxin_free(message);
    return false;
  }
  core_node_record("start_task");
  event_send_log("start_task", 3, NULL);
  lingxin_free(message);
  return result;
}

static void sendEndTask(VoiceChatHandler *handler)
{
  event_send_log("end_task", 1, NULL);
  if (s_end_task_sent)
  {
    event_send_log("end_task", 2, "already sent");
    return;
  }
  if (!handler)
  {
    event_send_log("end_task", 2, "handler null");
    return;
  }

  if (!handler->chat_config || !handler->chat_config->taskId)
  {
    event_send_log("end_task", 2, "params null");
    return;
  }
  if (!isTaskStarted)
  {
    event_send_log("end_task", 2, "task not started");
    return;
  }
  if (handler->waitTerminate)
  {
    event_send_log("end_task", 2, "waitTerminate, can not send end_task");
    return;
  }
  char message[256];
  snprintf(message, sizeof(message), "{\"header\":{\"action\":\"end_task\",\"task_id\":\"%s\"},\"payload\":{}}", handler->chat_config->taskId);
  uint32_t epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  waitTerminateOrEndSuccess = true;
  s_end_task_sent = true;
  bool result = hook_websocket_send_text(handler->websocket, message);
  if (!result)
  {
    event_send_log("end_task", 2, "send fail");
    on_voice_chat_init_fail_for_epoch(epoch);
    return;
  }
  core_node_record("end_task");
  lingxin_server_diag_timing_mark("S20_end_task_sent", 0);
  event_send_log("end_task", 3, NULL);
}

/* A real TTS payload for the validated current response proves cloud input
 * processing has advanced. Some sessions omit audio_ended; do not tear down
 * a completed answer merely because that redundant acknowledgement is absent.
 * Never infer this from text_output, an unannounced binary or another epoch. */
static void complete_upload_ack_from_response(VoiceChatHandler *handler, uint32_t epoch)
{
  bool finish_stage = false;
  voice_chat_handler_guard_lock();
  if (!handler || handler != globalHandler || handler->waitTerminate || handler->closing ||
      epoch != voice_chat_current_epoch() || !isTaskStarted || !s_end_audio_sent ||
      s_audio_ended_received || s_end_audio_ack_watchdog_epoch != epoch ||
      __atomic_load_n(&s_response_audio_epoch, __ATOMIC_ACQUIRE) != epoch
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
      || s_cloud_vad_upload_allowed
#endif
      ) {
    voice_chat_handler_guard_unlock();
    return;
  }
  notify_audio_upload_acked(handler);
  __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
  /* Recorder-close may have armed these after the entire TTS was received. */
  s_waiting_first_tts = false;
  __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
  s_end_audio_ack_grace_expired = false;
  finish_stage = s_task_stage_end_pending;
  s_task_stage_end_pending = false;
  voice_chat_handler_guard_unlock();
  lingxin_server_diag_timing_mark("upload_ack_from_current_tts", (int)epoch);
  if (finish_stage && epoch == voice_chat_current_epoch()) sendEndTask(handler);
}

static void note_audio_response_progress(VoiceChatHandler *handler)
{
  voice_chat_handler_guard_lock();
  if (!handler || handler != globalHandler || handler->waitTerminate || handler->closing ||
      !isTaskStarted || !s_audio_response_open) {
    voice_chat_handler_guard_unlock();
    return;
  }
  uint32_t epoch = voice_chat_current_epoch();
  __atomic_store_n(&s_response_audio_epoch, epoch, __ATOMIC_RELEASE);
  s_waiting_first_tts = false;
  __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
  voice_chat_handler_guard_unlock();
  complete_upload_ack_from_response(handler, epoch);
}

static void dealResponseDataEvent(cJSON *jsonMessage)
{
  cJSON *payload_json = cJSON_GetObjectItemCaseSensitive(jsonMessage, "payload");
  if (!cJSON_IsObject(payload_json))
  {
    lingxin_log_error("payload not an object");
    return;
  }
  cJSON *data_json = cJSON_GetObjectItemCaseSensitive(payload_json, "data");
  if (!cJSON_IsObject(data_json))
  {
    lingxin_log_error("data not an object");
    return;
  }

  char *frame_content = NULL;
  char *frame_type = NULL;
  // int frame_index = 0;
  bool last_frame = false;
  size_t frame_length = 0;

  cJSON *frame_base64_json = cJSON_GetObjectItemCaseSensitive(data_json, "frame_base64");
  if (cJSON_IsString(frame_base64_json) && frame_base64_json->valuestring)
  {
    frame_content = lingxin_base64_decode(frame_base64_json->valuestring, strlen(frame_base64_json->valuestring), &frame_length);
  }
  // cJSON *frame_index_json = cJSON_GetObjectItemCaseSensitive(data_json, "frame_index");
  // if (cJSON_IsNumber(frame_index_json) && frame_index_json->valueint)
  // {
  //   frame_index = frame_index_json->valueint;
  // }
  cJSON *frame_type_json = cJSON_GetObjectItemCaseSensitive(data_json, "frame_type");
  if (cJSON_IsString(frame_type_json) && frame_type_json->valuestring)
  {
    frame_type = frame_type_json->valuestring;
  }
  cJSON *last_frame_json = cJSON_GetObjectItemCaseSensitive(data_json, "last_frame");
  if (cJSON_IsBool(last_frame_json))
  {
    last_frame = cJSON_IsTrue(last_frame_json);
  }
  if (frame_type == NULL)
  {
    lingxin_log_ut_with_args(LINGXIN_WARN,
                             "chat_manager_parse_frame",
                             "frame_type missing, drop frame, decoded_len=%d, last_frame=%d",
                             (int)frame_length,
                             last_frame ? 1 : 0);
  }
  else if (strcmp(frame_type, "audio") == 0)
  {
    if (!globalHandler->waitTerminate)
    {
      // lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_parse_frame", "after decode, data length is %d", frame_length);
      if (frame_content && frame_length > 0U)
      {
        note_audio_response_progress(globalHandler);
        s_diag_download_audio_chunk_count++;
        s_diag_download_audio_bytes_total += (uint32_t)frame_length;
        if (s_diag_download_audio_chunk_count == 1U)
        {
          lingxin_server_diag_timing_mark("S14_first_tts_audio", (int)frame_length);
        }
        if (s_diag_download_audio_chunk_count == 1U || (s_diag_download_audio_chunk_count % 20U) == 0U)
        {
          lingxin_log_ut_with_args(LINGXIN_DEBUG,
                                   "chat_manager_recv_audio_frame",
                                   "chunks=%u, bytes=%u, last=%u, last_frame=%d",
                                   s_diag_download_audio_chunk_count,
                                   s_diag_download_audio_bytes_total,
                                   (uint32_t)frame_length,
                                   last_frame ? 1 : 0);
        }
        state_machine_receive_mp3_data((void *)frame_content, (int)frame_length);
      }
      if (last_frame)
      {
        // 云端本轮音频下行结束：通知状态机一次性结束下行流，与 task_ended 任一先到
        lingxin_log_ut_with_args(LINGXIN_DEBUG,
                                 "chat_manager_recv_last_frame",
                                 "chunks=%u, bytes=%u",
                                 s_diag_download_audio_chunk_count,
                                 s_diag_download_audio_bytes_total);
        state_machine_notify_audio_data_end();
      }
    }
  }
  else
  {
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_parse_frame",
                             "frame_type=%s as stream data, decoded_len=%d",
                             frame_type,
                             (int)frame_length);
    notify_all_listeners(CHAT_EVENT_ON_STREAM_DATA, frame_content, frame_length);
  }
  // 释放解码后的数据
  if (frame_content)
  {
    lingxin_free(frame_content);
  }
}

static void module_terminateCallback()
{
  state_machine_run_event(State_Event_VoiceChat_TerminateEnd);
}

static const char *message_task_id(cJSON *message_json)
{
  cJSON *header = cJSON_GetObjectItemCaseSensitive(message_json, "header");
  cJSON *task_id = header ? cJSON_GetObjectItemCaseSensitive(header, "task_id") : NULL;

  return cJSON_IsString(task_id) && task_id->valuestring ? task_id->valuestring : "";
}

static bool task_event_is_current(VoiceChatHandler *handler, const char *received_task_id)
{
  return handler && handler == globalHandler && handler->websocket &&
      !__atomic_load_n(&handler->closing, __ATOMIC_ACQUIRE) &&
      !__atomic_load_n(&handler->websocket->closing, __ATOMIC_ACQUIRE) &&
      received_task_id && received_task_id[0] && handler->chat_config &&
      handler->chat_config->taskId && handler->chat_config->taskId[0] &&
      strcmp(received_task_id, handler->chat_config->taskId) == 0;
}

static void dealEventFromServer(VoiceChatHandler *handler, const char *event, cJSON *message_json, const char *message_string)
{
  // 等待打断期间，只接收task_terminated和error
  // strcmp(event, "xxx")代表，event不等于xxx

  char *reqId = parseRequestId(message_json);
  const char *received_task_id = message_task_id(message_json);
  const char *active_task_id = (handler && handler->chat_config && handler->chat_config->taskId)
                                   ? handler->chat_config->taskId
                                   : "";
  if (!task_event_is_current(handler, received_task_id))
  {
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_discard_stale_task_event",
                             "event=%s, received_task_id=%s, active_task_id=%s",
                             event,
                             received_task_id,
                             active_task_id);
    return;
  }
  if (handler->waitTerminate && strcmp(event, "task_terminated") && strcmp(event, "task_ended") && strcmp(event, "error"))
  {
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_discard_event", "wait terminate, event: %s, request_id:%s", event, reqId);
    return;
  }
  lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_recv_event", "event: %s, request_id: %s", event, reqId);
  if (strcmp(event, "task_started") == 0)
  {
    if (!s_diag_waiting_task_started || isTaskStarted) return;
    cJSON *header = cJSON_GetObjectItemCaseSensitive(message_json, "header");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(header, "code");
    bool succeeded = (cJSON_IsString(code) && code->valuestring &&
                      strcmp(code->valuestring, "20000001") == 0) ||
                     (cJSON_IsNumber(code) && code->valuedouble == 20000001.0);
    if (!succeeded) {
      on_voice_chat_init_fail_for_epoch(voice_chat_current_epoch());
      return;
    }
    if (reqId && handler->contextInfo)
    {
      if (!handler->contextInfo->requestId || strlen(handler->contextInfo->requestId) == 0)
      {
        handler->contextInfo->requestId = lingxin_strdup(reqId);
      }
      else if (strlen(reqId) == strlen(handler->contextInfo->requestId))
      {
        memcpy(handler->contextInfo->requestId, reqId, strlen(reqId));
      }
      else
      {
        char *old_requestId = handler->contextInfo->requestId;
        handler->contextInfo->requestId = lingxin_strdup(reqId);
        lingxin_free(old_requestId);
      }
    }
    // 复位，防止异常
    handler->waitTerminate = false;
    isTaskStarted = true;
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "task_ready",
        "epoch=%u task_id=%s request_id=%s", voice_chat_current_epoch(), active_task_id, reqId);
    lingxin_server_task_started(active_task_id);
    __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    s_cloud_vad_upload_allowed = true;
#endif
    waitTerminateOrEndSuccess = false;
    // #region debug-point A:task-started-ack
    diag_note_task_started_wait_end(handler, "task_started");
    // #endregion
    notify_all_listeners(CHAT_EVENT_ON_AI_READY, "", 0);
  }
  else if (strcmp(event, "task_stage_end") == 0)
  {
    if (!isTaskStarted) return;
    if (s_task_audio_input && !s_end_audio_sent) {
      s_task_stage_end_pending = true;
      return;
    }
    lingxin_server_diag_timing_mark("S19_task_stage_end", 0);
    if (s_end_audio_sent && !s_audio_ended_received)
    {
      s_task_stage_end_pending = true;
      lingxin_log_ut_with_args(LINGXIN_DEBUG,
                               "chat_manager_defer_task_stage_end",
                               "waiting audio_ended, task_id=%s",
                               active_task_id);
      if (s_end_audio_ack_grace_expired)
      {
        s_task_stage_end_pending = false;
        on_voice_chat_init_fail_for_epoch(s_end_audio_ack_watchdog_epoch);
      }
      return;
    }
    sendEndTask(handler);
  }
  else if (strcmp(event, "task_terminated") == 0)
  {
    if (!handler->waitTerminate && !s_waiting_task_terminated) return;
    s_audio_response_open = false;
    lingxin_server_diag_timing_mark("INT03_task_terminated", 0);
    handler->waitTerminate = false;
    s_waiting_task_terminated = false;
    __atomic_store_n(&s_terminate_deadline_ms, 0U, __ATOMIC_RELEASE);
    s_waiting_first_tts = false;
    __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
    s_task_stage_end_pending = false;
    isAIResponseding = false;
    isTaskStarted = false;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    s_cloud_vad_upload_allowed = false;
#endif
    waitTerminateOrEndSuccess = false;
    // #region debug-point A:task-started-cancel-terminated
    if (s_diag_waiting_task_started)
    {
      diag_note_task_started_wait_end(handler, "task_terminated_before_task_started");
    }
    // #endregion
    module_terminateCallback();
  }
  else if (strcmp(event, "task_ended") == 0)
  {
    if (!isTaskStarted && !handler->waitTerminate && !s_diag_waiting_task_started) return;
    s_audio_response_open = false;
    lingxin_server_diag_timing_mark("S21_task_ended", (int)s_diag_download_audio_bytes_total);
    handler->waitTerminate = false;
    s_waiting_task_terminated = false;
    s_waiting_first_tts = false;
    __atomic_store_n(&s_terminate_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
    s_task_stage_end_pending = false;
    isAIResponseding = false;
    isTaskStarted = false;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    s_cloud_vad_upload_allowed = false;
#endif
    waitTerminateOrEndSuccess = false;
    // #region debug-point A:task-started-cancel-ended
    if (s_diag_waiting_task_started)
    {
      diag_note_task_started_wait_end(handler, "task_ended_before_task_started");
    }
    // #endregion
    state_machine_run_event(State_Event_VoiceChat_AIEnd);
  }
  else if (strcmp(event, "text_output") == 0)
  {
    if (!isTaskStarted) return;
    lingxin_server_diag_timing_mark("S16_text_output", 0);
    // 这里用到了cJSON_PrintUnformatted，注意要释放
    const char *payload = parsePayloadStr(message_json);
    lingxin_emit_chat_event(CHAT_LIFE_CYCLE_EVENT_TEXT_OUT, (char *)payload);
    cJSON_free((char *)payload);
  }
  else if (strcmp(event, "audio_response_start") == 0)
  {
    s_audio_response_open = isTaskStarted;
    lingxin_server_diag_timing_mark("S13_audio_response_start", 0);
  }
  else if (strcmp(event, "audio_response_end") == 0)
  {
    if (!isTaskStarted || !s_audio_response_open) return;
    s_audio_response_open = false;
    s_waiting_first_tts = false;
    __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
    lingxin_server_diag_timing_mark("S18_audio_response_end", (int)s_diag_download_audio_bytes_total);
    state_machine_notify_audio_data_end();
  }
  else if (strcmp(event, "audio_ended") == 0)
  {
    if (!s_end_audio_sent || s_audio_ended_received) return;
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "upload_ack",
        "epoch=%u task_id=%s", voice_chat_current_epoch(), active_task_id);
    lingxin_server_diag_timing_mark("S10_audio_ended", 0);
    notify_audio_upload_acked(handler);
    s_end_audio_ack_grace_expired = false;
    __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
    if (s_task_stage_end_pending)
    {
      s_task_stage_end_pending = false;
      sendEndTask(handler);
    }
  }
  else if (strcmp(event, "asr_ended") == 0)
  {
    lingxin_server_diag_timing_mark("legacy_asr_ended_ignored", 0);
    const char *payload = parsePayloadStr(message_json);
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_recv_asr_ended",
                             "payload=%s, request_id:%s",
                             payload ? payload : "",
                             reqId);
    if (payload)
    {
      cJSON_free((char *)payload);
    }
  }
  else if (strcmp(event, "task_interrupted") == 0)
  {
    /* task_interrupted belongs to the v1 full-duplex API. It must never drive
     * the v2 Cloud VAD state machine. */
    lingxin_server_diag_timing_mark("legacy_task_interrupted_ignored", 0);
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_recv_legacy_task_interrupted",
                             "ignored in v2 cloud-vad mode, task_id=%s, request_id:%s",
                             handler->chat_config ? handler->chat_config->taskId : "",
                             reqId);
  }
  else if (strcmp(event, "error") == 0)
  {
    s_waiting_task_terminated = false;
    s_waiting_first_tts = false;
    __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_terminate_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
    s_task_stage_end_pending = false;
    lingxin_server_diag_timing_mark("server_error", 0);
    // #region debug-point A:task-started-cancel-error
    if (s_diag_waiting_task_started)
    {
      diag_note_task_started_wait_end(handler, "server_error_before_task_started");
    }
    // #endregion
    on_voice_chat_error((char *)message_string);
  }
  else if (strcmp(event, "vad_end") == 0)
  {
    lingxin_server_diag_timing_mark("S07_vad_end", 0);
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    cloud_vad_close_upload_once(CHAT_EVENT_ON_VAD_END, "vad_end");
#else
    notify_all_listeners(CHAT_EVENT_ON_VAD_END, "", 0);
#endif
  }
  else if (strcmp(event, "vad_exit") == 0)
  {
    lingxin_server_diag_timing_mark("S08_vad_exit", 0);
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    cloud_vad_close_upload_once(CHAT_EVENT_ON_VAD_EXIT, "vad_exit");
#else
    notify_all_listeners(CHAT_EVENT_ON_VAD_EXIT, "", 0);
#endif
  }
  else if (strcmp(event, "system_event") == 0)
  {
    if (!isTaskStarted) return;
    // 这里用到了cJSON_PrintUnformatted，注意要释放
    const char *payload = parsePayloadStr(message_json);
    notify_all_listeners(CHAT_EVENT_ON_SYSTEM_EVENT, payload, strlen(payload));
    cJSON_free((char *)payload);
  }
  else if (strcmp(event, "response_data") == 0)
  {
    dealResponseDataEvent(message_json);
  }
  else if (strcmp(event, "request_ended") == 0)
  {
    const char *payload = parsePayloadStr(message_json);
    notify_all_listeners(CHAT_EVENT_ON_REQUEST_DATA_END, payload, strlen(payload));
    cJSON_free((char *)payload);
  }
  else
  {
    lingxin_log_ut_with_args(LINGXIN_WARN, "chat_manager_unknown_event", "event: %s unknown, ignore", event);
  }
}

static void onEventMessageReceived(VoiceChatHandler *handler, const char *message)
{
  cJSON *jsonMessage = cJSON_Parse(message);
  if (!jsonMessage)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_recv_parse_error", "message not json: %s, request_id:%s", message, getReqId(handler));
    return;
  }
  // 解析 event
  const char *eventStr = parseEvent(jsonMessage);
  if (!eventStr)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR,
                             "chat_manager_recv_parse_error",
                             "event null, request_id:%s, message=%s",
                             getReqId(handler),
                             message ? message : "");
    cJSON_Delete(jsonMessage);
    return;
  }
  dealEventFromServer(handler, eventStr, jsonMessage, message);
  cJSON_Delete(jsonMessage);
}

static void freeVoiceChat(VoiceChatHandler **handlerAddress)
{
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_exit_free_begin");

  if (!handlerAddress)
  {
    lingxin_log_ut_with_args(LINGXIN_WARN, "chat_manager_exit_free_fail", "handlerAddress null");
    return;
  }

  VoiceChatHandler *handler = *handlerAddress;
  if (!handler)
  {
    lingxin_log_ut_with_args(LINGXIN_WARN, "chat_manager_exit_free_fail", "handler null");
    return;
  }
  if (handler->websocket &&
      __atomic_load_n(&handler->websocket->clientHandler, __ATOMIC_ACQUIRE) == NULL &&
      handler->websocket->config)
  {
    WebsocketConfig *hook_config = handler->websocket->config;
    WebsocketConfig *raw_config = (WebsocketConfig *)hook_config->userContext;
    free_websocket_config(hook_config);
    handler->websocket->config = NULL;
    /* ON_WEBSOCKET_DESTROY clears hook_config->userContext before invoking
     * the hook, and the hook releases raw_config.  A failed handshake has no
     * destroy event, so the still-present raw wrapper is ours to release. */
    if (raw_config)
    {
      lingxin_free(raw_config);
    }
  }
  if (handler->websocket)
  {
    destroyWebsocketClient(handler->websocket);
    handler->websocket = NULL;
  }
  if (handler->chat_config)
  {
    if (handler->chat_config->taskId)
    {
      lingxin_free(handler->chat_config->taskId);
      handler->chat_config->taskId = NULL;
    }
    lingxin_free(handler->chat_config);
    handler->chat_config = NULL;
  }
  if (handler->contextInfo)
  {
    if (handler->contextInfo->instanceId)
    {
      lingxin_free(handler->contextInfo->instanceId);
      handler->contextInfo->instanceId = NULL;
    }
    if (handler->contextInfo->requestId)
    {
      lingxin_free(handler->contextInfo->requestId);
      handler->contextInfo->requestId = NULL;
    }
    lingxin_free(handler->contextInfo);
    handler->contextInfo = NULL;
  }
  voice_chat_handler_guard_lock();
  if (*handlerAddress == handler) *handlerAddress = NULL;
  voice_chat_handler_guard_unlock();
  lingxin_free(handler);
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_exit_free_finish");
}

static void *voice_chat_connection_failure_thread(void *arg)
{
  uint32_t epoch = (uint32_t)(uintptr_t)arg;
  uint32_t current_epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  uint32_t pending_epoch = __atomic_load_n(&s_connection_failure_pending_epoch, __ATOMIC_ACQUIRE);

  if (epoch == current_epoch && epoch == pending_epoch)
  {
    state_machine_receive_error(EXIT_REASON_WEBSOCKET_CONNECTION_FAILED);
    /* Business watchdogs can fire while the transport itself is still alive.
     * Explicitly close it from this deferred worker so old task callbacks and
     * the old handler cannot survive into the next wakeup. */
    (void)module_voiceChat_exit();
  }
  else
  {
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_ignore_stale_failure",
                             "failure_epoch=%u, current_epoch=%u, pending_epoch=%u",
                             epoch, current_epoch, pending_epoch);
  }

  uint32_t expected = epoch;
  (void)__atomic_compare_exchange_n(&s_connection_failure_pending_epoch, &expected, 0U,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  return NULL;
}

static void on_voice_chat_init_fail_for_epoch(uint32_t epoch)
{
  voice_chat_handler_guard_lock();
  uint32_t current_epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  if (epoch == 0U || epoch != current_epoch)
  {
    voice_chat_handler_guard_unlock();
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_ignore_stale_init_fail",
                             "failure_epoch=%u, current_epoch=%u",
                             epoch, current_epoch);
    return;
  }

  /* Reserve teardown before making the old socket appear idle/reusable. */
  uint32_t expected = 0U;
  if (!__atomic_compare_exchange_n(&s_connection_failure_pending_epoch, &expected, epoch,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
  {
    voice_chat_handler_guard_unlock();
    return;
  }

  /* The callback runs on the WebSocket worker thread, while error handling
   * synchronously enters State_Exit and destroys the WebSocket.  Defer it so
   * lisa_ws_cleanup() never deletes/frees its own running callback context. */
  isAIResponseding = false;
  s_audio_response_open = false;
  isAudioSending = false;
  isTaskStarted = false;
  s_waiting_task_terminated = false;
  s_waiting_first_tts = false;
  __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_terminate_deadline_ms, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
  s_task_stage_end_pending = false;
  voice_chat_handler_guard_unlock();
  lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_start_new_fail", "chat init fail");
  core_node_record("chat_manager_start_new_fail");
  // #region debug-point A:task-started-cancel-init-fail
  if (s_diag_waiting_task_started)
  {
    diag_note_task_started_wait_end(globalHandler, "init_fail_before_task_started");
  }
  // #endregion
  lingxin_tid_t thread_id = 0;
  lingxin_thread_param_t thread_param = {
      .name = "lx_ws_fail",
      .priority = 0,
      .stack_size = 3072,
  };
  if (lingxin_thread_create(&thread_id, &thread_param,
                            voice_chat_connection_failure_thread, (void *)(uintptr_t)epoch) != 0)
  {
    expected = epoch;
    (void)__atomic_compare_exchange_n(&s_connection_failure_pending_epoch, &expected, 0U,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_start_new_fail",
                             "defer websocket failure handling failed");
  }
}

static void on_voice_chat_init_fail(void)
{
  on_voice_chat_init_fail_for_epoch(__atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE));
}

void voice_chat_fail_current_round(void)
{
  /* Recorder/transport failures cannot be reported as successful boundaries.
   * Reuse the deferred teardown; callers may be recorder or websocket tasks. */
  on_voice_chat_init_fail();
}

unsigned int voice_chat_current_epoch(void)
{
  return __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
}

void voice_chat_fail_epoch(unsigned int epoch)
{
  on_voice_chat_init_fail_for_epoch(epoch);
}

void voice_chat_fail_waiting_cloud_vad(unsigned int epoch)
{
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  if (s_end_audio_sent || !claim_cloud_vad_boundary(epoch)) return;
  on_voice_chat_init_fail_for_epoch(epoch);
#else
  (void)epoch;
#endif
}

static void onWebSocketEvent(WebSocketEventType event, const char *data, const size_t len, const int isBinary, void *userData)
{
  VoiceChatHandler *handler = (VoiceChatHandler *)userData;
  if (!handler || handler != globalHandler)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_recv_parse_error", "handler nul");
    return;
  }
  switch (event)
  {
  case ON_WEBSOCKET_CONNECTION_SUCCESS:
    isWebsocketConnected = true;
    lingxin_server_diag_timing_mark("S02_websocket_connected", 0);
    lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_websocket_connect_success");
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_ws_connected",
                             "task_id:%s, request_id:%s",
                             handler->chat_config && handler->chat_config->taskId ? handler->chat_config->taskId : "",
                             getReqId(handler));
    if (!sendStartTask(handler))
    {
      lingxin_log_ut_with_args(LINGXIN_ERROR,
                               "chat_manager_start_new_fail",
                               "start_task send fail after websocket connected");
      on_voice_chat_init_fail();
    }
    break;
  case ON_WEBSOCKET_CONNECTION_FAIL:
    isWebsocketConnected = false;
    isAudioSending = false;
    isAIResponseding = false;
    isTaskStarted = false;
    waitTerminateOrEndSuccess = false;
    s_waiting_task_terminated = false;
    s_waiting_first_tts = false;
    __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
    s_task_stage_end_pending = false;
    on_voice_chat_init_fail();
    break;
  case ON_WEBSOCKET_DATA_RECEIVED:
    if (handler != globalHandler || !handler->websocket ||
        __atomic_load_n(&handler->closing, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&handler->websocket->closing, __ATOMIC_ACQUIRE)) break;
    if (isBinary)
    {
      /* Binary frames carry no task_id. Only admit them inside a response
       * announced for the active task; termination must close the old task
       * before this socket can start the next one. */
      if (data && len > 0 && handler == globalHandler && isTaskStarted &&
          s_audio_response_open && !handler->waitTerminate)
      {
        note_audio_response_progress(handler);
        s_waiting_first_tts = false;
        __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
        s_diag_download_audio_chunk_count++;
        s_diag_download_audio_bytes_total += (uint32_t)len;
        if (s_diag_download_audio_chunk_count == 1U)
        {
          lingxin_server_diag_timing_mark("S14_first_tts_audio", (int)len);
        }
        lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_recv_binary", "binary data length: %d", len);
        state_machine_receive_mp3_data((void *)data, (int)len);
      }
      else
      {
        lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_discard_data", "binary data length: %d", len);
      }
    }
    else
    {
      onEventMessageReceived(handler, data);
    }
    break;
  case ON_WEBSOCKET_ERROR:
    s_waiting_task_terminated = false;
    s_waiting_first_tts = false;
    __atomic_store_n(&s_terminate_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_end_audio_ack_deadline_ms, 0U, __ATOMIC_RELEASE);
    s_task_stage_end_pending = false;
  {
    char *errorData = (char *)lingxin_malloc(len + 1);
    if (!errorData)
    {
      lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_recv_parse_error", "allocate memory fail for error data");
      return;
    }
    // 复制数据
    memcpy(errorData, data, len);
    // 添加字符串结束符
    errorData[len] = '\0';
    on_voice_chat_error(errorData);
    lingxin_free(errorData);
  }
  break;
  case ON_WEBSOCKET_DESTROY:
    s_waiting_task_terminated = false;
    s_waiting_first_tts = false;
    __atomic_store_n(&s_terminate_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_first_tts_deadline_ms, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);

    lingxin_server_diag_timing_mark("S23_websocket_closed", 0);
    isAudioSending = false;
    isAIResponseding = false;
    isWebsocketConnected = false;
    isTaskStarted = false;
    lingxin_log_ut(LINGXIN_ERROR, "chat_manager_exit_callback");
    lingxin_checkpoint_report("lingxin_test_websocket_destroyed");
    /* Never destroy the protocol handler from the websocket callback thread.
     * An intentional close is owned by module_voiceChat_exit(); a spontaneous
     * disconnect is handed to the generation-scoped recovery worker. */
    if (!handler->websocket ||
        !__atomic_load_n(&handler->websocket->closing, __ATOMIC_ACQUIRE))
    {
      on_voice_chat_init_fail_for_epoch(
          __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE));
    }
    break;
  default:
    break;
  }
}

bool isVoiceChatInited()
{
  voice_chat_handler_guard_lock();
  bool occupied = __atomic_load_n(&s_handler_close_finalizing, __ATOMIC_ACQUIRE) ||
      globalHandler != NULL;
  voice_chat_handler_guard_unlock();
  return occupied;
}

bool isVoiceChatResponding() { return isAIResponseding; }
bool isVoiceChatConnected() { return isWebsocketConnected; }
void voice_chat_get_transport_stats(VoiceChatTransportStats *stats)
{
  if (!stats) return;
  stats->epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  stats->start_seq = s_diag_start_task_seq;
  stats->upload_frames = s_diag_binary_audio_send_count;
  stats->upload_bytes = s_diag_binary_audio_send_bytes;
  stats->download_chunks = s_diag_download_audio_chunk_count;
  stats->task_started = isTaskStarted;
  stats->audio_sending = isAudioSending;
  stats->terminating = s_waiting_task_terminated;
}

void voice_chat_get_websocket_stats(WebsocketTransportStats *stats)
{
  VoiceChatHandler *handler = voice_chat_handler_acquire();
  websocketGetTransportStats(handler ? handler->websocket : NULL, stats);
  voice_chat_handler_release(handler);
}

static int voiceChatTerminateCheck()
{
  event_send_log("terminate_task", 1, NULL);
  if (!isAIResponseding)
  {
    event_send_log("terminate_task", 2, "not need terminate");
    return 0;
  }
  if (waitTerminateOrEndSuccess)
  {
    event_send_log("terminate_task", 2, "before end_task or terminate success,cannot terminate");
    return 2;
  }
  if (!globalHandler)
  {
    event_send_log("terminate_task", 2, "handler null");
    return 0;
  }
  if (!globalHandler->chat_config || !globalHandler->chat_config->taskId)
  {
    event_send_log("terminate_task", 2, "taskId null");
    return 0;
  }
  if (!isTaskStarted && !s_diag_waiting_task_started)
  {
    /* Connection establishment is still in flight. Do not invent a successful
     * termination and then reuse the same transport for a second live task. */
    event_send_log("terminate_task", 2, "start not submitted yet");
    on_voice_chat_init_fail();
    return 2;
  }
  if (!isWebsocketConnected)
  {
    event_send_log("terminate_task", 2, "websocket not connected");
    on_voice_chat_init_fail();
    return 2;
  }
  char message[256];
  snprintf(message, sizeof(message), "{\"header\":{\"action\":\"terminate_task\",\"task_id\":\"%s\"},\"payload\":{}}", globalHandler->chat_config->taskId);
  uint32_t epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  waitTerminateOrEndSuccess = true;
  isAudioSending = false;
  globalHandler->waitTerminate = true;
  s_audio_response_open = false;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  s_cloud_vad_upload_allowed = false;
#endif
  __atomic_store_n(&s_start_task_deadline_ms, 0U, __ATOMIC_RELEASE);
  arm_terminate_task_watchdog();
  if (!hook_websocket_send_text(globalHandler->websocket, message))
  {
    event_send_log("terminate_task", 2, "send fail");
    on_voice_chat_init_fail_for_epoch(epoch);
    return 2;
  }
  core_node_record("terminate_task");
  lingxin_server_diag_timing_mark("INT02_terminate_task_sent", 0);
  event_send_log("terminate_task", 3, NULL);
  return 1;
}

/**
 * 打断对话
 * @return 0 打断指令没有发出去 1 打断指令已经正常发送出去 2 打断指令或者end_task指令之前已经发送出去过，需要等待响应回来
 */
int module_voiceChat_terminate()
{
  int r = voiceChatTerminateCheck();
  if (r == 0)
  {
    lingxin_log_ut_with_args(LINGXIN_WARN, "chat_manager_terminate_callback", "terminate fail, directly callback");
    // 打断失败，这里不阻塞下次运行
    isAIResponseding = false;
    module_terminateCallback();
  }
  else
  {
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_manager_terminate_callback", r == 2 ? "wait terminated" : "normal terminate");
  }
  return r;
}

static void voice_chat_close_completed(void *user)
{
  VoiceChatHandler *handler = (VoiceChatHandler *)user;
  /* Runs on the close worker, after the websocket worker has acknowledged
   * exit. Keep the owner alive until callers that acquired it also return. */
  uint32_t drain_waited_ms = 0U;
  while (__atomic_load_n(&handler->active_operations, __ATOMIC_ACQUIRE) != 0U)
  {
    lingxin_thread_sleep(1);
    drain_waited_ms++;
    if (drain_waited_ms == LINGXIN_HANDLER_DRAIN_TIMEOUT_MS)
    {
      lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_exit_deferred",
                               "handler operations did not drain, active=%u",
                               __atomic_load_n(&handler->active_operations, __ATOMIC_ACQUIRE));
    }
  }

  /* Intentional close suppresses transport callbacks, so it must retire the
   * old connection flags here rather than waiting for ON_WEBSOCKET_DESTROY. */
  s_waiting_task_terminated = false;
  s_waiting_first_tts = false;
  s_diag_waiting_task_started = false;
  s_terminate_deadline_ms = 0U;
  s_first_tts_deadline_ms = 0U;
  s_start_task_deadline_ms = 0U;
  s_end_audio_ack_deadline_ms = 0U;
  isAudioSending = false;
  isAIResponseding = false;
  isWebsocketConnected = false;
  isTaskStarted = false;
  s_audio_response_open = false;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  s_cloud_vad_upload_allowed = false;
#endif
  /* An Idle residual connection has no new EXIT callback barrier yet. Keep
   * it reported as occupied across clearing the slot and delivering ExitEnd. */
  __atomic_store_n(&s_handler_close_finalizing, true, __ATOMIC_RELEASE);
  freeVoiceChat(handler->selfPointer);
  state_machine_run_event(State_Event_VoiceChat_ExitEnd);
  __atomic_store_n(&s_handler_close_finalizing, false, __ATOMIC_RELEASE);
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_exit_finish");
}

bool module_voiceChat_exit()
{
  bool already_pending = false;
  VoiceChatHandler *handler = voice_chat_handler_begin_close(&already_pending);
  if (!handler)
  {
    return already_pending;
  }
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_exit_begin");
  lingxin_server_diag_timing_mark("S22_websocket_close_requested", 0);
  if (!hook_websocket_close_async(handler->websocket, voice_chat_close_completed, handler))
  {
    voice_chat_handler_guard_lock();
    handler->close_requested = false;
    voice_chat_handler_guard_unlock();
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_exit_deferred",
                             "close worker unavailable; owner retained for retry");
    return false;
  }
  /* Accepted, not completed. Only voice_chat_close_completed emits ExitEnd. */
  return true;
}

static bool voiceChatCreate(ChatStartNewParams *params)
{
  WebsocketConfig *websocketConfig = NULL;
  lingxin_trace_set("chat:voicechat_create_begin");
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_voicechat_create_enter",
                           "task=%s, task_id=%s, input=%s, output=%s, user_input=%s",
                           (params && params->task) ? params->task : "",
                           (params && params->taskId) ? params->taskId : "",
                           (params && params->input_mode) ? params->input_mode : "",
                           (params && params->output_mode) ? params->output_mode : "",
                           (params && params->user_input) ? params->user_input : "");
  lingxin_log_debug("voiceChatCreate begin");
  globalHandler = (VoiceChatHandler *)lingxin_calloc(1, sizeof(struct VoiceChatHandler));
  if (!globalHandler)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_init_fail", "voicechat handler allocate fail");
    goto create_fail;
  }
  globalHandler->chat_config = (VoiceChatConfig *)lingxin_calloc(1, sizeof(VoiceChatConfig));
  if (!fill_config_params(globalHandler->chat_config, params))
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_init_fail", "config get fail");
    goto create_fail;
  }
  lingxin_trace_set("chat:voicechat_fill_config_ok");
  lingxin_log_debug("voiceChatCreate fill_config_params ok");
  globalHandler->waitTerminate = false;
  websocketConfig = createWebsocketConfig(globalHandler, globalHandler->chat_config->sn, globalHandler->chat_config->appKey, globalHandler->chat_config->appId, globalHandler->chat_config->serverPath, onWebSocketEvent);
  if (!websocketConfig)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_init_fail", "websocket config create fail");
    goto create_fail;
  }
  lingxin_trace_set("chat:voicechat_ws_config_ok");
  lingxin_log_debug("voiceChatCreate websocketConfig ok");
  globalHandler->websocket = hook_websocket_init(websocketConfig);
  if (!globalHandler->websocket)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_init_fail", "websocket init fail");
    goto create_fail;
  }
  lingxin_trace_set("chat:voicechat_ws_init_ok");
  lingxin_log_debug("voiceChatCreate websocket init ok");
  globalHandler->contextInfo = (VoiceChatContextInfo *)lingxin_calloc(1, sizeof(VoiceChatContextInfo));
  if (globalHandler->contextInfo)
  {
    globalHandler->contextInfo->requestId = NULL;
    globalHandler->contextInfo->instanceId = generateUUID(16);
  }
  /* Install before starting the asynchronous websocket thread. */
  globalHandler->selfPointer = &globalHandler;
  lingxin_server_diag_timing_mark("S01_websocket_connect_requested", 0);
  if (hook_websocket_start(globalHandler->websocket) == false)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_init_fail", "websocket start fail");
    goto create_fail;
  }
  lingxin_trace_set("chat:voicechat_ws_start_ok");
  lingxin_log_debug("voiceChatCreate websocket start ok");
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_init_finish");
  return true;

create_fail:
  core_node_record("chat_manager_init_fail");
  if (globalHandler)
  {
    if (globalHandler->chat_config)
    {
      if (globalHandler->chat_config->payload)
      {
        lingxin_free(globalHandler->chat_config->payload);
        globalHandler->chat_config->payload = NULL;
      }
      lingxin_free(globalHandler->chat_config);
      globalHandler->chat_config = NULL;
    }
    if (websocketConfig)
    {
      free_websocket_config(websocketConfig);
    }

    lingxin_free(globalHandler);
    globalHandler = NULL;
  }
  return false;
}

bool add_protocol_event_listener(ChatEventListener listener)
{
  if (!listener)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_add_event_listener", "listener is null");
    return false;
  }
  // 检查监听器是否已存在
  for (int i = 0; i < global_chat_listener_count; i++)
  {
    if (global_chat_listeners[i] == listener)
    {
      lingxin_log_ut_with_args(LINGXIN_WARN, "chat_manager_add_event_listener", "listener already exists");
      return false;
    }
  }
  // 检查是否已达到最大监听器数量
  if (global_chat_listener_count >= MAX_LISTENERS)
  {
    lingxin_log_ut_with_args(LINGXIN_WARN, "chat_manager_add_event_listener", "max listeners reached");
    return false;
  }
  // 添加新的监听器
  global_chat_listeners[global_chat_listener_count] = listener;
  global_chat_listener_count++;
  return true;
}

bool remove_protocol_event_listener(ChatEventListener listener)
{
  if (!listener)
  {
    return false;
  }
  bool result = false;
  // 查找并移除指定的监听器
  for (int i = 0; i < global_chat_listener_count; i++)
  {
    if (global_chat_listeners[i] == listener)
    {
      // 将后续监听器向前移动一位
      for (int j = i; j < global_chat_listener_count - 1; j++)
      {
        global_chat_listeners[j] = global_chat_listeners[j + 1];
      }
      global_chat_listener_count--;
      global_chat_listeners[global_chat_listener_count] = NULL;
      result = true;
      break;
    }
  }
  return result;
}

bool voice_chat_start_new(ChatStartNewParams *params)
{
  if (!params) return false;
  VoiceChatHandler *handler;
  bool result = false;
  voice_chat_handler_guard_lock();
  if (__atomic_load_n(&s_handler_close_finalizing, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&s_connection_failure_pending_epoch, __ATOMIC_ACQUIRE) != 0U ||
      (globalHandler && __atomic_load_n(&globalHandler->closing, __ATOMIC_ACQUIRE)))
  {
    voice_chat_handler_guard_unlock();
    return false;
  }
  handler = globalHandler;
  if (handler)
    (void)__atomic_add_fetch(&handler->active_operations, 1U, __ATOMIC_ACQUIRE);
  voice_chat_handler_guard_unlock();
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_start_new_begin");
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_start_new_snapshot",
                           "handler=%d, aiResponding=%d, audioSending=%d, waitTerminate=%d",
                           handler ? 1 : 0,
                           isAIResponseding ? 1 : 0,
                           isAudioSending ? 1 : 0,
                           (handler && handler->waitTerminate) ? 1 : 0);
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_start_new_params",
                           "task=%s, task_id=%s, input=%s, output=%s, user_input=%s, schedule=%s",
                           (params && params->task) ? params->task : "",
                           (params && params->taskId) ? params->taskId : "",
                           (params && params->input_mode) ? params->input_mode : "",
                           (params && params->output_mode) ? params->output_mode : "",
                           (params && params->user_input) ? params->user_input : "",
                           (params && params->scheduleTaskId) ? params->scheduleTaskId : "");
  if (params->is_schedule_timer_task)
  {
    // 如果是拉起新一轮novoice循环,允许continue
    isAIResponseding = false;
  }

  /* A failed handshake leaves a handler allocated while its websocket is no
   * longer usable.  Reclaim it before attempting to reuse the connection. */
  if (handler && !isWebsocketConnected && !isAIResponseding)
  {
    lingxin_log_ut(LINGXIN_WARN, "chat_manager_reclaim_disconnected_handler");
    /* Continuation can be called by PlayEnd dispatch. Closing synchronously
     * here can wait on the websocket worker while it waits to initialize the
     * next player. Let the existing failure worker own transport teardown. */
    on_voice_chat_init_fail();
    goto done;
  }

  // 不允许连续多次continue
  if (handler && isAIResponseding)
  {
    lingxin_log_ut_with_args(LINGXIN_WARN, "chat_manager_start_new_fail", "isAIResponseding true");
    lingxin_trace_set("chat:start_new_blocked_ai_responding");
    goto done;
  }

  /* Every accepted dialog start owns a new recovery epoch. Deferred failures
   * from an older websocket/task must never tear down this round. */
  voice_chat_handler_guard_lock();
  if (__atomic_load_n(&s_connection_failure_pending_epoch, __ATOMIC_ACQUIRE) != 0U ||
      globalHandler != handler ||
      (handler && __atomic_load_n(&handler->closing, __ATOMIC_ACQUIRE))) {
    voice_chat_handler_guard_unlock();
    goto done;
  }
  uint32_t recovery_epoch = __atomic_add_fetch(&s_recovery_epoch, 1U, __ATOMIC_ACQ_REL);
  if (recovery_epoch == 0U)
  {
    __atomic_store_n(&s_recovery_epoch, 1U, __ATOMIC_RELEASE);
  }
  voice_chat_handler_guard_unlock();
  // 之前没有建联或者建联后断联了
  if (handler == NULL)
  {
    lingxin_trace_set("chat:start_new_create_handler");
    isAudioSending = false;
    isTaskStarted = false;
    isAIResponseding = true;
    if (!voiceChatCreate(params))
    {
      on_voice_chat_init_fail();
      goto done;
    }
  }
  else
  {
    lingxin_trace_set("chat:start_new_reuse_handler");
    if (!fill_config_params(handler->chat_config, params))
    {
      lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_start_new_fail", "config get fail");
      core_node_record("chat_manager_start_new_fail");
      goto done;
    }
    // 非一次调用continue，需要重新发送start
    if (!sendStartTask(handler))
    {
      /* sendStartTask schedules epoch-bound teardown. Recreating a socket
       * here in the same epoch would let that worker destroy the new one. */
      on_voice_chat_init_fail();
      goto done;
    }
  }
  result = true;
done:
  voice_chat_handler_release(handler);
  return result;
}

static bool voice_chat_audio_send_current(void *arg)
{
  uint32_t epoch = *(const uint32_t *)arg;
  return epoch == __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE) &&
      isTaskStarted && !s_waiting_task_terminated
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
      && s_cloud_vad_upload_allowed
#endif
      ;
}

void voiceChatSendAudio(void *audioData, int dataSize)
{
  if (!audioData || !dataSize)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_send_audio_fail", "params null");
    return;
  }
  VoiceChatHandler *handler = voice_chat_handler_acquire();
  if (!handler)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_send_audio_fail", "handler null or closing");
    return;
  }
  if (!isTaskStarted)
  {
    s_diag_binary_audio_drop_before_task_started++;
    if (s_diag_binary_audio_drop_before_task_started == 1U ||
        (s_diag_binary_audio_drop_before_task_started % 50U) == 0U)
    {
      lingxin_log_ut_with_args(LINGXIN_WARN,
                               "chat_manager_send_audio_skip",
                               "drop=%u, last=%d, reason=task_not_started, start_seq=%u, wait_s=%ld",
                               s_diag_binary_audio_drop_before_task_started,
                               dataSize,
                               s_diag_start_task_seq,
                               s_diag_start_task_sent_at_s > 0 ? (lingxin_get_timestamp_s() - s_diag_start_task_sent_at_s) : -1L);
    }
    voice_chat_handler_release(handler);
    return;
  }
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  if (!s_cloud_vad_upload_allowed)
  {
    voice_chat_handler_release(handler);
    return;
  }
#endif
  uint32_t epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  int result = hook_websocket_send_binary_guarded(handler->websocket, audioData, dataSize,
                                                 voice_chat_audio_send_current, &epoch);
  if (result == WEBSOCKET_SEND_CANCELLED || !voice_chat_audio_send_current(&epoch)) {
    /* A wake, cloud VAD or round replacement owns this boundary. A delayed
     * old sender must neither fail nor increment counters for the new round. */
    voice_chat_handler_release(handler);
    return;
  }
  if (result != dataSize)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "chat_manager_send_audio_fail", "send fail, request_id:%s", getReqId(handler));
    voice_chat_handler_release(handler);
    /* The adapter already exhausted its bounded retries. Dropping this PCM
     * then sending the suffix creates a different sentence at the server. */
    on_voice_chat_init_fail_for_epoch(epoch);
    return;
  }
  s_diag_binary_audio_send_count++;
  s_diag_binary_audio_send_bytes += (uint32_t)result;
#ifdef ARCS_TEMP_UPLINK_SAMPLE
  extern void lingxin_uplink_sample_append(const void *, uint32_t, uint32_t);
  lingxin_uplink_sample_append(audioData, (uint32_t)result, epoch);
#endif
  if (s_diag_binary_audio_send_count == 1U)
  {
    lingxin_server_diag_timing_mark("S05_first_audio_uploaded", result);
  }
  if (s_diag_binary_audio_send_count == 1U || (s_diag_binary_audio_send_count % 100U) == 0U)
  {
    if (s_diag_binary_audio_send_count > 1U)
    {
      lingxin_server_diag_timing_mark("S06_audio_upload_progress", (int)s_diag_binary_audio_send_count);
    }
    lingxin_log_ut_with_args(LINGXIN_DEBUG,
                             "chat_manager_send_audio_ok",
                             "sent=%u, bytes=%u, last=%d, request_id:%s",
                             s_diag_binary_audio_send_count,
                             s_diag_binary_audio_send_bytes,
                             result,
                             getReqId(handler));
  }
  isAudioSending = true;
  voice_chat_handler_release(handler);
}

void voiceChatStopSendAudio()
{
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
  s_cloud_vad_upload_allowed = false;
#endif
  event_send_log("end_audio", 1, NULL);
  lingxin_log_ut(LINGXIN_DEBUG, "chat_manager_send_end_audio_begin");
  if (s_end_audio_sent)
  {
    event_send_log("end_audio", 2, "already sent");
    return;
  }
  if (!globalHandler)
  {
    event_send_log("end_audio", 2, "handler null");
    return;
  }
  if (!globalHandler->chat_config || !globalHandler->chat_config->taskId)
  {
    event_send_log("end_audio", 2, "params null");
    return;
  }
  if (!isTaskStarted)
  {
    event_send_log("end_audio", 2, "task not started");
    return;
  }
  char message[256];
  snprintf(message, sizeof(message), "{\"header\":{\"action\":\"end_audio\",\"task_id\":\"%s\"},\"payload\":{}}", globalHandler->chat_config->taskId);
  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "chat_manager_send_end_audio_try",
                           "task_id:%s, request_id:%s, audioSending=%d, taskStarted=%d",
                           globalHandler->chat_config->taskId,
                           getReqId(globalHandler),
                           isAudioSending ? 1 : 0,
                           isTaskStarted ? 1 : 0);
  uint32_t epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  isAudioSending = false;
  s_end_audio_sent = true;
  s_audio_ended_received = false;
  /* Keep a stage_end that raced the asynchronous recorder close. Its ACK
   * must still send end_task, even if the server emits stage_end only once. */
  s_end_audio_ack_grace_expired = false;
  s_end_audio_ack_watchdog_epoch = __atomic_load_n(&s_recovery_epoch, __ATOMIC_ACQUIRE);
  __atomic_store_n(&s_end_audio_ack_deadline_ms, LINGXIN_END_AUDIO_ACK_TIMEOUT_MS, __ATOMIC_RELEASE);
  arm_first_tts_watchdog();
  if (!hook_websocket_send_text(globalHandler->websocket, message))
  {
    event_send_log("end_audio", 2, "send fail");
    on_voice_chat_init_fail_for_epoch(epoch);
    return;
  }
  complete_upload_ack_from_response(globalHandler, epoch);
  lingxin_server_diag_timing_mark("S09_end_audio_sent", 0);
  lingxin_log_ut_with_args(LINGXIN_DEBUG, "upload_end_queued",
      "epoch=%u task_id=%s tx_bytes=%u", epoch,
      globalHandler->chat_config->taskId, s_diag_binary_audio_send_bytes);
  event_send_log("end_audio", 3, NULL);
}

bool voice_chat_audio_end_sent(void)
{
  return s_end_audio_sent;
}

// 多模态结束任务
bool voiceChat_send_end_up_task(size_t confirm_data_count, Multimodal_Chat_Confirm_Data confirm_data[])
{
  event_send_log("end_up_task", 1, NULL);
  char *reqId = getReqId(globalHandler);
  if (!reqId || reqId[0] == '\0')
  {
    event_send_log("end_up_task", 2, "request_id empty");
    return false;
  }

  // 无confirm data 场景
  if (confirm_data_count <= 0 || !confirm_data)
  {
    char message[256];
    snprintf(message, sizeof(message), "{\"header\":{\"action\":\"end_up_task\",\"request_id\":\"%s\",\"task_id\":\"%s\"},\"payload\":{}}", reqId, globalHandler->chat_config->taskId);
    bool result = hook_websocket_send_text(globalHandler->websocket, message);
    if (!result)
    {
      event_send_log("end_up_task", 2, "send fail");
      return false;
    }
    event_send_log("end_up_task", 3, NULL);
    return result;
  }
  // 有confirm data 场景
  int base_len = snprintf(NULL, 0,
                          "{\"header\":{\"action\":\"end_up_task\",\"request_id\":\"%s\",\"task_id\":\"%s\"},\"payload\":{\"confirm_data\":[",
                          reqId, globalHandler->chat_config->taskId);
  if (base_len <= 0)
  {
    event_send_log("end_up_task", 2, "base len calc fail");
    return false;
  }

  // 计算 confirm_data 部分长度
  int data_len = 0;
  for (size_t i = 0; i < confirm_data_count; i++)
  {
    if (i > 0)
    {
      data_len += 1; // 逗号
    }
    int item_len = snprintf(NULL, 0,
                            "{\"unique_id\":\"%s\",\"frame_type\":\"%s\"}",
                            confirm_data[i].unique_id, confirm_data[i].frame_type);
    if (item_len < 0)
    {
      event_send_log("end_up_task", 2, "data item len calc fail");
      return false;
    }
    data_len += item_len;
  }

  int total_len = base_len + data_len + 4; // 4 for "]}]}"

  char *message = (char *)lingxin_malloc(total_len + 1);
  if (!message)
  {
    event_send_log("end_up_task", 2, "memory calc fail");
    return false;
  }

  int offset = snprintf(message, total_len + 1,
                        "{\"header\":{\"action\":\"end_up_task\",\"request_id\":\"%s\",\"task_id\":\"%s\"},"
                        "\"payload\":{\"confirm_data\":[",
                        reqId, globalHandler->chat_config->taskId);

  if (offset < 0 || offset >= total_len + 1)
  {
    event_send_log("end_up_task", 2, "base message format fail");
    lingxin_free(message);
    return false;
  }

  for (size_t i = 0; i < confirm_data_count; i++)
  {
    if (i > 0)
    {
      int comma_len = snprintf(message + offset, total_len + 1 - offset, ",");
      if (comma_len < 0 || comma_len >= total_len + 1 - offset)
      {
        event_send_log("end_up_task", 2, "comma len format fail");
        lingxin_free(message);
        return false;
      }
      offset += comma_len;
    }

    int item_len = snprintf(message + offset, total_len + 1 - offset,
                            "{\"unique_id\":\"%s\",\"frame_type\":\"%s\"}",
                            confirm_data[i].unique_id, confirm_data[i].frame_type);
    if (item_len < 0 || item_len >= total_len + 1 - offset)
    {
      event_send_log("end_up_task", 2, "data item format fail");
      lingxin_free(message);
      return false;
    }
    offset += item_len;
  }

  int end_len = snprintf(message + offset, total_len + 1 - offset, "]}}");
  if (end_len < 0 || end_len >= total_len + 1 - offset)
  {
    event_send_log("end_up_task", 2, "end message format fail");
    lingxin_free(message);
    return false;
  }
  bool result = hook_websocket_send_text(globalHandler->websocket, message);
  lingxin_free(message);
  if (!result)
  {
    event_send_log("end_up_task", 2, "send fail");
    return false;
  }
  event_send_log("end_up_task", 3, NULL);
  return result;
}

bool voiceChat_send_request_data_stream(char *unique_id, int index, const char *content, int content_len, char *content_type, char *frame_type, bool is_last)
{
  // event_send_log("request_data", 1, NULL);
  if (!unique_id || !content || !content_type)
  {
    event_send_log("request_data", 2, "unique_id or content or type null");
    return false;
  }
  if (!globalHandler)
  {
    event_send_log("request_data", 2, "handler null");
    return false;
  }
  if (!globalHandler->chat_config || !globalHandler->chat_config->taskId)
  {
    event_send_log("request_data", 2, "taskId null");
    return false;
  }
  char *encode_result = lingxin_base64_encode(content, content_len);
  if (!encode_result)
  {
    event_send_log("request_data", 2, "encode fail");
    return false;
  }
  char *reqId = getReqId(globalHandler);
  char *is_last_str = is_last ? "true" : "false";
  // 计算所需缓冲区大小
  char *format_str =
      "{\"header\":{\"action\":\"request_data\",\"request_id\":\"%s\",\"task_id\":\"%s\"},"
      "\"payload\":{\"data\":{\"unique_id\":\"%s\",\"frame_index\":%d,\"frame_base64\":\"%s\","
      "\"frame_type\":\"%s\",\"content_type\":\"%s\",\"last_frame\":%s}}}";
  int message_len = snprintf(NULL, 0, format_str, reqId, globalHandler->chat_config->taskId, unique_id, index, encode_result, frame_type, content_type, is_last_str);
  if (message_len <= 0)
  {
    event_send_log("request_data", 2, "message len calc fai");
    lingxin_free(encode_result);
    return false;
  }
  // 动态分配内存
  char *message = (char *)lingxin_malloc(message_len + 1);
  if (!message)
  {
    event_send_log("request_data", 2, "message allocate fai");
    lingxin_free(encode_result);
    return false;
  }
  // 格式化消息
  snprintf(message, message_len + 1, format_str, reqId, globalHandler->chat_config->taskId, unique_id, index, encode_result, frame_type, content_type, is_last_str);
  bool result = hook_websocket_send_text(globalHandler->websocket, message);
  if (!result)
  {
    event_send_log("request_data", 2, "send fail");
    lingxin_log_ut_with_args(LINGXIN_ERROR,
                             "chat_manager_send_audio_frame",
                             "send fail, frame_index=%d, bytes=%d, type=%s",
                             index,
                             content_len,
                             content_type);
  }
  else
  {
    s_diag_upload_frame_count++;
    s_diag_upload_bytes_total += (uint32_t)content_len;
    if (s_diag_upload_frame_count == 1U || (s_diag_upload_frame_count % 100U) == 0U || is_last)
    {
      lingxin_log_ut_with_args(LINGXIN_DEBUG,
                               "chat_manager_send_audio_frame",
                               "sent=%u, bytes=%u, frame_index=%d, last=%d, type=%s",
                               s_diag_upload_frame_count,
                               s_diag_upload_bytes_total,
                               index,
                               is_last ? 1 : 0,
                               content_type);
    }
  }
  // event_send_log("request_data", 3, NULL);
  lingxin_free(message);
  lingxin_free(encode_result);
  return result;
}

bool voiceChat_send_request_data_text(char *content)
{
  if (!content)
  {
    event_send_log("request_data", 2, "content null");
    return false;
  }
  if (!globalHandler)
  {
    event_send_log("request_data", 2, "handler null");
    return false;
  }
  if (!globalHandler->chat_config || !globalHandler->chat_config->taskId)
  {
    event_send_log("request_data", 2, "taskId null");
    return false;
  }
  char *reqId = getReqId(globalHandler);

  // 计算所需缓冲区大小
  char *format_str = "{\"header\":{\"action\":\"request_data\",\"request_id\":\"%s\",\"task_id\":\"%s\"},"
                     "\"payload\":{\"data\":{\"content\":\"%s\",\"frame_type\":\"text\"}}}";

  int message_len = snprintf(NULL, 0, format_str, reqId, globalHandler->chat_config->taskId, content);
  if (message_len <= 0)
  {
    event_send_log("request_data", 2, "message len calc fail");
    return false;
  }
  // 动态分配内存
  char *message = (char *)lingxin_malloc(message_len + 1);
  if (!message)
  {
    event_send_log("request_data", 2, "message allocate fail");
    return false;
  }
  // 格式化消息
  snprintf(message, message_len + 1, format_str, reqId, globalHandler->chat_config->taskId, content);
  bool result = hook_websocket_send_text(globalHandler->websocket, message);
  if (!result)
  {
    event_send_log("request_data", 2, "send fail");
  }
  lingxin_free(message);
  return result;
}
