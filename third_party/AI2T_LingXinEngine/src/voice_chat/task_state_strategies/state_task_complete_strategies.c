#include <stdio.h>
#include "lingxin_log.h"
#include "chat_state_machine.h"
#include "state_task_complete_stategies.h"
#include "chat_runtime_context.h"
#include "state_download_manager.h"
#include "task_continue_guard.h"

static task_continue_guard_t s_action_continue_guard;

void voice_chat_action_hold_acquire(void)
{
    task_continue_guard_acquire(&s_action_continue_guard);
}

void voice_chat_action_hold_release(void)
{
    if (!task_continue_guard_release(&s_action_continue_guard)) {
        return;
    }

    /* Task_Complete was waiting for the final physical action. Re-evaluate
     * the original round policy now that no servo action can feed the mic. */
    if (get_current_chat_state() == State_Task_Complete) {
        turn_to_task_complete(get_current_context(), NULL);
    }
}

void voice_chat_action_hold_cancel(void)
{
    task_continue_guard_reset(&s_action_continue_guard);
}

static void continue_strategy(StateEvent event)
{
    end_current_chat_runtime_context();

    /* task_id scopes exactly one start_task ... task_ended cycle. Clear the
     * one-shot id before creating the next continuous-listening round. */
    update_sesseion_context(CTX_FIELD_CURRENT_TASK_ID, (ContextValue){.string = NULL});

    // 执行继续对话策略
    start_new_chat_runtime_context();

    turn_to_with_preset_inner_state(State_Upload_Init, event, NULL);
}

static void continue_with_prompt_strategy()
{
    // 执行带提示音的继续对话策略
    lingxin_log_debug("播放跟进提示音");
    module_local_play_continue_audio();
}

static void vad_exit_strategy(bool input_timeout_audio)
{
    (void)input_timeout_audio;
    /* task_ended and local playback have already converged before entering
     * Task_Complete. A Cloud VAD timeout ends the session and closes the WS. */
    state_task_complete_receive_event(Event_Inc_TaskComplete_End, NULL);
}

static void single_round_exit_strategy()
{
    // 执行单轮对话退出策略
    state_task_complete_receive_event(Event_Inc_TaskComplete_End, NULL);
}

// 切换到任务完成态
void turn_to_task_complete(const ChatStateRuntimeContext *current_context, InnerStateCollection *innerPayload)
{
    if (!current_context)
    {
        lingxin_log_debug(" current_context is null");
        return;
    }
    if (task_continue_guard_should_defer(&s_action_continue_guard))
    {
        lingxin_log_debug("defer continuous listening until physical actions finish");
        return;
    }
    if (current_context->has_is_vad_exit && current_context->is_vad_exit)
    {
        vad_exit_strategy(current_context->input_timeout_audio);
        return;
    }
    if (current_context->single_round)
    {
        // 执行带提示音的继续对话策略
        single_round_exit_strategy();
    }
    else
    {
        if (current_context->need_continue_prompt)
        {
            // 执行带提示音的继续对话策略
            continue_with_prompt_strategy();
        }
        else
        {
            // 执行继续对话策略
            continue_strategy(Event_Inc_TaskComplete_End);
        }
    }
}

// 接受到事件
void state_task_complete_receive_event(StateEvent event, StateEventPayload *payload)
{
    lingxin_log_debug("state_machine_receive_event: %d", event);
    // 根据当前状态和事件，决定状态转移
    switch (event)
    {
    case State_Event_VoiceChat_AIEnd:
    {
        if (get_current_context() != NULL && get_current_context()->is_vad_exit)
        {
            // 执行退出逻辑策略
            vad_exit_strategy(get_current_context()->input_timeout_audio);
        }
        break;
    }
    case State_Event_WillExit:
    {
        if (get_current_context() != NULL && get_current_context()->is_vad_exit && get_current_context()->input_timeout_audio)
        {
            // 执行退出逻辑策略
            playback_manager_terminate();
        }
        if (payload && payload->will_exit_payload && payload->will_exit_payload->disable_close_ws_immediately)
        {
            InnerStateForExit inner_state = {true, true, true, true, true};
            InnerStateCollection inner_state_collection = {
                .inner_state_for_exit = &inner_state,
            };
            turn_to_with_preset_inner_state(State_Exit, State_Event_WillExit, &inner_state_collection);
        }
        else
        {
            InnerStateForExit inner_state = {true, true, true, false, true};
            InnerStateCollection inner_state_collection = {
                .inner_state_for_exit = &inner_state,
            };
            turn_to_with_preset_inner_state(State_Exit, State_Event_WillExit, &inner_state_collection);
        }
        break;
    }
    case State_Event_ContinuePrompt_PlayEnd:
    {
        continue_strategy(State_Event_ContinuePrompt_PlayEnd);
        break;
    }

    case State_Event_VoiceChat_ExitEnd:
    {
        break;
    }
    case Event_Inc_TaskComplete_End:
    {
        /* Upload_CloseEnd, task_ended and playback end have converged before
         * Task_Complete. Closing the websocket is optional in the official
         * protocol, so return directly to Idle and keep the connection warm
         * for the next wakeup. The old async-close path could remain stuck in
         * State_Exit forever when its destroy callback was lost. */
        turn_to_with_preset_inner_state(State_Idle, event, NULL);
        break;
    }
    case Event_Inc_TaskComplete_PlayEnd:
    {
        // Legacy vad-exit path keeps the websocket for compatibility.
        turn_to_with_preset_inner_state(State_Idle, event, NULL);
        break;
    }
    default:
        break;
    }
}
