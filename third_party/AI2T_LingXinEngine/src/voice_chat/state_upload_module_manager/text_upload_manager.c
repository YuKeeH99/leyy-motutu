// recorder_adapter.c
#include "chat_state_machine.h"
#include "chat_runtime_context.h"
#include "lingxin_log.h"
#include "upload_record_interface.h"

#include "schedule_timer_manager.h"

static void onChatEvent(ChatEventType event, const char *data, const size_t len);

static void onChatEvent(ChatEventType event, const char *data, const size_t len)
{
  switch (event)
  {
  case CHAT_EVENT_ON_AI_READY:
    state_machine_run_event_with_payload(State_Event_Upload_CloseEnd, NULL);
    break;
  case CHAT_EVENT_ON_ERROR:
    // 通知定时任务模块定时任务触发失败
    recieve_schedule_task_error();
    break;
  default:
    break;
  }
}

static void voice_chat_continue()
{
  lingxin_log_ut(LINGXIN_DEBUG, "text_upload_voice_chat_continue_enter");

  ChatStartNewParams voice_chat_continue_params = {0};
  voice_chat_continue_params.taskId = "";
  voice_chat_continue_params.task = "";
  voice_chat_continue_params.input_mode = "";
  voice_chat_continue_params.output_mode = "";
  voice_chat_continue_params.user_input = "";
  voice_chat_continue_params.scheduleTaskId = "";

  // 三个参数 char* input_mode;  char *output_mode;
  voice_chat_continue_params.output_mode = "voice";
  voice_chat_continue_params.input_mode = "no_voice";

  voice_chat_continue_params.task = "chat";

  if (get_current_context() != NULL)
  {
    if (get_current_context()->has_current_schedule_id)
    {
      voice_chat_continue_params.scheduleTaskId = get_current_context()->current_schedule_id;
      voice_chat_continue_params.is_schedule_timer_task = true;
    }

    if (get_current_context()->has_current_user_input)
    {
      voice_chat_continue_params.user_input = get_current_context()->current_user_input;
    }

    if (get_current_context()->has_current_task_id)
    {
      voice_chat_continue_params.taskId = get_current_context()->current_task_id;
    }

    print_chat_context(get_current_context());
  }

  lingxin_log_debug("纯 text 模块任务 voice_chat_continue_params.taskId:%s, voice_chat_continue_params.task:%s voice_chat_continue_params.user_input:%s, voice_chat_continue_params.scheduleTaskId:%s", voice_chat_continue_params.taskId, voice_chat_continue_params.task, voice_chat_continue_params.user_input, voice_chat_continue_params.scheduleTaskId);

  lingxin_log_ut_with_args(LINGXIN_DEBUG,
                           "text_upload_voice_chat_continue_params",
                           "task=%s, task_id=%s, input_mode=%s, output_mode=%s, user_input=%s",
                           voice_chat_continue_params.task,
                           voice_chat_continue_params.taskId,
                           voice_chat_continue_params.input_mode,
                           voice_chat_continue_params.output_mode,
                           voice_chat_continue_params.user_input);
  voice_chat_start_new(&voice_chat_continue_params);
  lingxin_log_ut(LINGXIN_DEBUG, "text_upload_voice_chat_continue_return");
}

static int schedule_upload_manager_init()
{
  // 初始化录音器模块
  lingxin_log_ut(LINGXIN_DEBUG, "text_upload_manager_init_enter");
  lingxin_log_debug("定时任务模块 上行初始化成功");
  add_protocol_event_listener(onChatEvent);
  state_machine_run_event_with_payload(State_Event_Upload_InitEnd, NULL);
  lingxin_log_ut(LINGXIN_DEBUG, "text_upload_manager_init_return");

  return 1;
}
static void send_start()
{
  lingxin_log_ut(LINGXIN_DEBUG, "text_upload_send_start_enter");
  voice_chat_continue();
  lingxin_log_ut(LINGXIN_DEBUG, "text_upload_send_start_return");
}

static void schdule_upload_manager_destory(void)
{
  remove_protocol_event_listener(onChatEvent);
}

const UploadModuleInterface g_scheduleUploadManager = {
    .upload_init = schedule_upload_manager_init,
    .upload_start = send_start,
    .upload_terminate = NULL,
    .upload_destory = schdule_upload_manager_destory};
