"""Execute query props, real context merging and the continuous-round strategy."""
import unittest

from test_lingxin_round_races import COMMON, ROOT, body, run_c

SERVER = 'src/server/lingxin_server/lingxin_server.c'
CONTEXT = 'third_party/AI2T_LingXinEngine/src/voice_chat/chat_runtime_context.c'
COMPLETE = 'third_party/AI2T_LingXinEngine/src/voice_chat/task_state_strategies/state_task_complete_strategies.c'


class QueryContinuationTest(unittest.TestCase):
    def test_local_result_is_text_once_then_normal_audio_without_old_task_id(self):
        header = (ROOT / 'third_party/AI2T_LingXinEngine/src/inc/chat_runtime_context.h').read_text(encoding='utf-8')
        types = header[header.index('typedef struct'):header.index('// 函数声明')]
        start = body(SERVER, 'lingxin_server_chat_start_internal')
        props = start[start.index('    StartNewChatProps props ='):start.index('    /* Official v2')]
        code = COMMON + r'''
typedef int ExitCode;
typedef int ChatStateMediaType;
typedef int StateEvent;
#define EXIT_REASON_USER_INITIATED 0
#define Media_Type_TextOnly 1
#define Media_Type_Chat 2
#define Media_Type_Multimodal 3
#define State_Upload_Init 4
#define lingxin_free free
#define lingxin_mutex_lock(m) ((void)0)
#define lingxin_mutex_unlock(m) ((void)0)
static int lingxin_taskid_mutex;
static char *lingxin_strdup(const char*s){if(!s)return NULL;size_t n=strlen(s)+1;char*p=malloc(n);memcpy(p,s,n);return p;}
typedef struct {bool disable_welcome_audio,single_round;char *user_input;} StartNewChatProps;
static StartNewChatProps get_start_new_chat_default_props(void){return (StartNewChatProps){0};}
static StartNewChatProps prepare(const char *text, bool continue_after_text){
''' + props + r'''
 return props;
}
''' + types + r'''
static ChatStateRuntimeContext *global_config,*session_config,*temp_config,*current_config;
static ChatStateRuntimeContext s_context_pool[4];
static bool s_context_in_use[4];
static unsigned audio_starts;
static void print_chat_context(const ChatStateRuntimeContext *ctx){}
static void turn_to_with_preset_inner_state(int state,StateEvent e,void *p){
 assert(state==State_Upload_Init && current_config->upload_type==Media_Type_Chat);audio_starts++;
}
'''
        functions = [
            ('static ChatStateRuntimeContext *alloc_context(void)', 'alloc_context'),
            ('static void free_string(ChatStateRuntimeContext *ctx)', 'free_string'),
            ('static void free_context(ChatStateRuntimeContext *ctx)', 'free_context'),
            ('static bool copy_context(ChatStateRuntimeContext *dst,const ChatStateRuntimeContext *src)', 'copy_context'),
            ('bool set_session_context(const ChatStateRuntimeContext *src)', 'set_session_context'),
            ('bool set_temp_session_context(const ChatStateRuntimeContext *src)', 'set_temp_session_context'),
            ('static void clear_temp_session_context(void)', 'clear_temp_session_context'),
            ('static void set_current_upload_type(char *task,char *user_input,char *schedule_task_id)', 'set_current_upload_type'),
            ('bool start_new_chat_runtime_context(void)', 'start_new_chat_runtime_context'),
            ('void end_current_chat_runtime_context(void)', 'end_current_chat_runtime_context'),
            ('bool update_runtime_context_data(ChatStateRuntimeContext *ctx,ChatContextField field,ContextValue value)', 'update_runtime_context_data'),
            ('bool update_temp_context(ChatContextField field,ContextValue value)', 'update_temp_context'),
            ('bool update_sesseion_context(ChatContextField field,ContextValue value)', 'update_sesseion_context'),
        ]
        for signature, name in functions:
            code += '\n' + signature + body(CONTEXT, name)
        code += '\nstatic void continue_strategy(StateEvent event)' + body(COMPLETE, 'continue_strategy')
        code += r'''
int main(void){
 StartNewChatProps query=prepare("local version result",true);
 assert(!query.single_round && query.disable_welcome_audio);
 assert(prepare("ordinary shell text",false).single_round);
 assert(!prepare(NULL,false).single_round);
 assert(update_sesseion_context(CTX_FIELD_GLOBAL_TASK,(ContextValue){.string="chat"}));
 assert(update_sesseion_context(CTX_FIELD_SINGLE_ROUND,(ContextValue){.boolean=query.single_round}));
 assert(update_sesseion_context(CTX_FIELD_CURRENT_TASK_ID,(ContextValue){.string="query-1"}));
 assert(update_temp_context(CTX_FIELD_CURRENT_USER_INPUT,(ContextValue){.string=query.user_input}));
 assert(update_temp_context(CTX_FIELD_UPLOAD_TYPE,(ContextValue){.media_type=Media_Type_TextOnly}));
 assert(start_new_chat_runtime_context());
 assert(current_config->upload_type==Media_Type_TextOnly && !current_config->single_round);
 assert(current_config->current_user_input && !strcmp(current_config->current_user_input,"local version result"));
 assert(!temp_config->has_current_user_input && !temp_config->has_upload_type);
 continue_strategy(0);
 assert(audio_starts==1 && !current_config->single_round && current_config->upload_type==Media_Type_Chat);
 assert(!current_config->has_current_user_input && !current_config->current_user_input);
 assert(!current_config->has_current_task_id && !current_config->current_task_id);
 continue_strategy(0);
 assert(audio_starts==2 && current_config->upload_type==Media_Type_Chat && !current_config->current_user_input);
}
'''
        run_c(code)

    def test_only_query_api_requests_voice_continuation(self):
        source = (ROOT / SERVER).read_text(encoding='utf-8')
        self.assertTrue('int lingxin_server_chat_start_query_text(' in source,
                        'device queries need an explicit continuation API; generic text must stay single round')
        code = COMMON + r'''
static int calls;
static bool resume;
static int lingxin_server_chat_start_internal(const char *text,bool continue_after_text,uint32_t owner){
 assert(text && text[0] && owner==0);calls++;resume=continue_after_text;return 0;
}
int lingxin_server_chat_start_text(const char *text)
''' + body(SERVER, 'lingxin_server_chat_start_text') + r'''
int lingxin_server_chat_start_query_text(const char *text)
''' + body(SERVER, 'lingxin_server_chat_start_query_text') + r'''
int main(void){
 assert(!lingxin_server_chat_start_query_text("version") && calls==1 && resume);
 assert(!lingxin_server_chat_start_text("shell") && calls==2 && !resume);
 assert(lingxin_server_chat_start_query_text(NULL)<0 && lingxin_server_chat_start_query_text("")<0 && calls==2);
}
'''
        run_c(code)
        worker = body('apps/arcs-mini/services/service_voice_action.c', 'voice_query_task')
        self.assertIn('lingxin_server_chat_start_query_text(prompt)', worker)
        self.assertNotIn('lingxin_server_chat_start_text(prompt)', worker)


if __name__ == '__main__':
    unittest.main()
