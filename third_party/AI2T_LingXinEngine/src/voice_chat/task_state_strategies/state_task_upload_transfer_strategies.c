#include "state_task_upload_transfer_strategies.h"
#include "lingxin_log.h"
#include "chat_state_machine.h"
#include "chat_runtime_context.h"
#include "state_upload_manager.h"
#include "lingxin_chat_upload_manager.h"

void turn_to_upload_transfer(const ChatStateRuntimeContext *current_context, InnerStateCollection *innerPayload)
{
    lingxin_log_debug("turn_to_upload_transfer start upload manager");
    upload_manager_start(); // 录音开始 / 发送 task_start
}

// 接受到事件
void state_upload_transfer_receive_event(StateEvent event, StateEventPayload *payload)
{
    switch (event)
    {
    case State_Event_VoiceChat_AIEnd:
    {
        /* Protocol-order recovery only: a conforming v2 server sends
         * vad_end/vad_exit first. Stop capture so the async Upload_CloseEnd
         * path can still converge without leaking the recorder. */
        lingxin_log_warn("task_ended before vad_end/vad_exit; force recorder stop");
        lingxin_chat_upload_manager_stop_record();
        break;
    }
    case State_Event_Upload_CloseEnd:
        turn_to_with_preset_inner_state(State_Download_Init, event, NULL);
        break;
    case State_Event_Vad_Exit:
    {
        /* Do not leave the upload state yet. The recorder stop callback sends
         * end_audio and raises Upload_CloseEnd, which enters Download_Init to
         * accept the server's optional timeout prompt. */
        update_current_context(CTX_FIELD_IS_VAD_EXIT, (ContextValue){.boolean = true});
        update_current_context(CTX_FIELD_EXIT_CODE, (ContextValue){.exit_code = EXIT_REASON_NO_INPUT_TIMEOUT});
    }
    break;

    default:
        lingxin_log_debug("state_upload_transfer_receive_event 未处理的事件 %d", event);
        break;
    }
}
