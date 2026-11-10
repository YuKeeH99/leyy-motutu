// recorder_adapter.c
#include "lingxin_recorder_manager.h"
#include "upload_record_interface.h"
#include "chat_state_machine.h"
#include "chat_runtime_context.h"
#include "lingxin_log.h"

#include "lingxin_protocol_manager.h"
#include "lingxin_chat_upload_manager.h"
#include "chat_state_machine_event.h"
#include "lingxin_trace.h"
#include "lingxin_server.h"

static void on_record_init(bool success);
static void on_record_stop(bool success);
static void on_record_terminate(bool success);
static unsigned int s_cancelled_record_stop_epoch;

static void sent_start_callback(void *buf, int rlen, int index)
{
    if (buf != NULL && rlen > 0)
    {
        voiceChatSendAudio(buf, rlen);
    }
    else
    {
        lingxin_log_error("chat模块 上行录音发送失败");
    }
}

static void on_record_stop_no_event(bool success)
{
    voiceChatStopSendAudio();
    if (!success)
    {
        lingxin_log_error("chat模块 上行录音打断失败");
    }
}

// 监听器
static void onChatEvent(ChatEventType event, const char *data, const size_t len)
{
    bool upload_ended = voice_chat_audio_end_sent();
    lingxin_log_debug("chat模块 上行事件回调 event:%d", event);
    switch (event)
    {
    case CHAT_EVENT_ON_VAD_END:
        lingxin_log_ut(LINGXIN_DEBUG, "chat_upload_manager_cloud_vad_end_stop_record");
        if (!upload_ended) module_record_stop_once(0, on_record_stop);
        break;
    case CHAT_EVENT_ON_VAD_EXIT:
        /* Cloud VAD timeout still follows the official end_audio handshake.
         * Mark the round as vad_exit, then use the same recorder-stop callback
         * as vad_end so end_audio is sent exactly once. */
        lingxin_log_ut(LINGXIN_DEBUG, "chat_upload_manager_cloud_vad_exit_stop_record");
        state_machine_run_event_with_payload(State_Event_Vad_Exit, NULL);
        if (!upload_ended) module_record_stop_once(0, on_record_stop);
        break;
    case CHAT_EVENT_ON_AI_READY:
        // 调用录音发送的方法
        lingxin_trace_set("chat:upload_ai_ready");
        lingxin_log_debug("chat模块 AI_READY -> module_record_start_send");
        module_record_start_send(sent_start_callback);
        break;
    default:
        break;
    }
}

static void on_record_init(bool success)
{
    lingxin_log_debug("chat模块 on_record_init success=%d", success ? 1 : 0);
    lingxin_trace_set(success ? "chat:record_init_ok" : "chat:record_init_fail");
    if (!success)
    {
        lingxin_log_error("chat模块 上行初始化失败");
        voice_chat_fail_current_round();
        return;
    }
    state_machine_run_event_with_payload(State_Event_Upload_InitEnd, NULL);
}

static void on_record_stop(bool success)
{
    if (__atomic_load_n(&s_cancelled_record_stop_epoch, __ATOMIC_ACQUIRE) == voice_chat_current_epoch())
        return;
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "chat_upload_manager_on_record_stop", "success=%d", success ? 1 : 0);
    lingxin_trace_set(success ? "chat:record_stop_ok" : "chat:record_stop_fail");
    lingxin_server_diag_timing_mark("C01_record_stop_complete", success ? 1 : 0);
    if (!success)
    {
        lingxin_log_error("chat模块 上行停止录音失败");
        voice_chat_fail_current_round();
        return;
    }
    lingxin_log_ut(LINGXIN_DEBUG, "chat_upload_manager_send_end_audio");
    voiceChatStopSendAudio();
    state_machine_run_event_with_payload(State_Event_Upload_CloseEnd, NULL);
}

static void on_record_terminate(bool success)
{
    lingxin_trace_set(success ? "chat:record_terminate_ok" : "chat:record_terminate_fail");
    if (!success)
    {
        lingxin_log_error("chat模块 上行录音打断失败");
        voice_chat_fail_current_round();
        return;
    }
    state_machine_run_event_with_payload(State_Event_Upload_TerminateEnd, NULL);
}

static void voice_chat_continue()
{
    ChatStartNewParams voice_chat_continue_params = {0};
    // 初始化参数
    voice_chat_continue_params.taskId = "";
    voice_chat_continue_params.task = "";
    voice_chat_continue_params.input_mode = "";
    voice_chat_continue_params.output_mode = "";
    voice_chat_continue_params.user_input = "";
    voice_chat_continue_params.scheduleTaskId = "";

    if (get_current_context() != NULL)
    {
        if (get_current_context()->has_global_task)
        {
            voice_chat_continue_params.task = get_current_context()->global_task;
        }

        if (get_current_context()->has_current_task_id)
        {
            voice_chat_continue_params.taskId = get_current_context()->current_task_id;
        }

        if (get_current_context()->has_disable_server_vad)
        {
            voice_chat_continue_params.server_vad = !(get_current_context()->disable_server_vad);
        }

        voice_chat_continue_params.output_mode = get_output_type_string();
        voice_chat_continue_params.input_mode = get_input_type_string();

        print_chat_context(get_current_context());
    }

    lingxin_log_debug("voice_chat_continue_params.taskId:%s, voice_chat_continue_params.task:%s voice_chat_continue_params.user_input:%s, voice_chat_continue_params.scheduleTaskId:%s", voice_chat_continue_params.taskId, voice_chat_continue_params.task, voice_chat_continue_params.user_input, voice_chat_continue_params.scheduleTaskId);
    voice_chat_start_new(&voice_chat_continue_params); // 普通连续对话
}

static int record_init()
{
    int ret;

    // 添加事件的监听器
    add_protocol_event_listener(onChatEvent);

    lingxin_trace_set("chat:record_init_start");
    lingxin_log_debug("chat模块 record_init start module_record_start");
    ret = module_record_start(on_record_init);
    if (ret != 0)
    {
        lingxin_trace_set("chat:record_init_start_fail");
        lingxin_log_error("chat模块 record_init module_record_start failed");
        on_record_init(false);
        return 0;
    }

    return 1;
}
static void send_start()
{
    lingxin_trace_set("chat:send_start_before_continue");
    lingxin_log_debug("chat模块 send_start before voice_chat_continue");
    voice_chat_continue();
    lingxin_trace_set("chat:send_start_after_continue");
    lingxin_log_debug("chat模块 send_start after voice_chat_continue");
}

static void recorder_terminate(void)
{
    __atomic_store_n(&s_cancelled_record_stop_epoch, voice_chat_current_epoch(), __ATOMIC_RELEASE);
    // TODO: 打断时需要阻塞websocket, 移动到外层
    extern void freeze_websocket();
    freeze_websocket();
    module_record_stop(0, on_record_terminate);
}

static void recorder_destory(void)
{
    remove_protocol_event_listener(onChatEvent);
}

void lingxin_chat_upload_manager_stop_record()
{
    module_record_stop_once(1, on_record_stop);
}

void lingxin_chat_upload_manager_poll_record_limit(void)
{
    module_record_poll_limit(on_record_stop);
}

void lingxin_chat_upload_manager_vad_exit()
{
    lingxin_log_ut(LINGXIN_DEBUG, "chat_upload_manager_local_vad_exit");
    state_machine_run_event_with_payload(State_Event_Vad_Exit, NULL);
    module_record_stop(0, on_record_stop_no_event);
}

const UploadModuleInterface g_chatUploadManager = {
    .upload_init = record_init,
    .upload_start = send_start,
    .upload_terminate = recorder_terminate,
    .upload_destory = recorder_destory,
};
