#include "lingxin_log.h"
#include "state_upload_manager.h"
#include "upload_manager_factory.h"

static const UploadModuleInterface *s_upload = NULL;

static ChatStateMediaType current_upload_type = Media_Type_Chat;

int upload_manager_init(ChatStateMediaType type)
{

    lingxin_log_debug("进入上传状态，初始化录音器");
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "upload_manager_init_enter", "type=%d", (int)type);
    lingxin_trace_set("upload:init_enter");

    if (s_upload && s_upload->upload_destory)
    {
        lingxin_trace_set("upload:destroy_prev");
        s_upload->upload_destory();
    }

    // 通过工厂类方法创建相关类
    s_upload = upload_factory_manager_init(type);
    lingxin_trace_set(s_upload ? "upload:factory_ok" : "upload:factory_null");
    lingxin_log_debug("upload_manager_init type=%d upload=%p", type, s_upload);
    if (!s_upload || !s_upload->upload_init)
    {
        lingxin_log_error("upload_manager_init failed, upload=%p", s_upload);
        return 0;
    }

    current_upload_type = type;

    lingxin_log_ut(LINGXIN_DEBUG, "upload_manager_init_before_handler");
    s_upload->upload_init();
    lingxin_log_ut(LINGXIN_DEBUG, "upload_manager_init_after_handler");
    lingxin_trace_set("upload:init_ret");

    return 1;
}

void upload_manager_start()
{
    lingxin_trace_set("upload:start_enter");
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "upload_manager_start_enter", "upload=%d", s_upload ? 1 : 0);
    lingxin_log_debug("upload_manager_start upload=%p", s_upload);
    if (s_upload && s_upload->upload_start)
    {
        lingxin_log_debug("上传模块 发送 task_start");
        lingxin_trace_set("upload:start_call");
        lingxin_log_ut(LINGXIN_DEBUG, "upload_manager_start_before_handler");
        s_upload->upload_start();
        lingxin_log_ut(LINGXIN_DEBUG, "upload_manager_start_after_handler");
        lingxin_trace_set("upload:start_ret");
    }
    else
    {
        lingxin_trace_set("upload:start_no_handler");
        lingxin_log_error("upload_manager_start no handler upload=%p", s_upload);
    }
}

void upload_manager_terminate(void)
{
    if (s_upload && s_upload->upload_terminate)
    {
        s_upload->upload_terminate();
    }
    else
    {
        state_machine_run_event_with_payload(State_Event_Upload_TerminateEnd, NULL);
    }
}
