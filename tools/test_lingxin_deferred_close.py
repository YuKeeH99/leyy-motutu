"""Exercise real close callers while transport cleanup remains busy."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_lingxin_round_races import COMMON, ROOT, body, run_c

PROTOCOL = 'third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c'
STATE = 'third_party/AI2T_LingXinEngine/src/voice_chat/chat_state_machine.c'
SERVER = 'src/server/lingxin_server/lingxin_server.c'


class DeferredCloseTest(unittest.TestCase):
    def test_handler_slot_and_finalizing_are_one_snapshot(self):
        run_c(COMMON + r'''
static void *globalHandler=(void*)1;
static bool s_handler_close_finalizing,locked,inject=true;
static void voice_chat_handler_guard_lock(void){assert(!locked);locked=true;}
static void voice_chat_handler_guard_unlock(void){assert(locked);locked=false;}
static bool load_flag(bool*p){
 bool previous=*p;
 if(inject && !locked){inject=false;s_handler_close_finalizing=true;globalHandler=NULL;}
 return previous;
}
#define __atomic_load_n(p,order) load_flag(p)
bool isVoiceChatInited(void)
''' + body(PROTOCOL, 'isVoiceChatInited') + r'''
int main(void){assert(isVoiceChatInited());
 globalHandler=NULL;s_handler_close_finalizing=true;assert(isVoiceChatInited());
 s_handler_close_finalizing=false;assert(!isVoiceChatInited());assert(!locked);}
''')

    def test_wake_retries_close_after_dispatch_failure_without_restarting_modules(self):
        source = (ROOT / STATE).read_text(encoding='utf-8')
        start = source.rindex('  case State_Event_WillExit:')
        end = source.index('  case State_Event_VoiceChat_ExitEnd:', start)
        run_c(COMMON + r'''
typedef struct {bool record_terminated,buffer_play_terminated,voice_chat_terminated,voice_chat_exited,is_normal_exit;} InnerStateForExit;
typedef struct {InnerStateForExit*inner_state_for_exit;} InnerStateCollection;
typedef struct {bool disable_close_ws_immediately;} ExitPayload;
typedef struct {ExitPayload*will_exit_payload;} StateEventPayload;
enum {State_Idle,State_Exit,State_Task_Complete,State_Event_WillExit};
static int chat_current_state=State_Exit,requests,restarts;
static bool accepted;
static bool module_voiceChat_exit(void){requests++;return accepted;}
static void state_task_complete_receive_event(int e,StateEventPayload*p){assert(0);}
static void turn_to_with_preset_inner_state(int s,int e,InnerStateCollection*p){restarts++;}
static void receive_event(int event,StateEventPayload*payload){switch(event){
''' + source[start:end] + r'''
default:break;}}
int main(void){
 assert(!module_voiceChat_exit()); /* Initial resource shortage. */
 accepted=true;receive_event(State_Event_WillExit,NULL); /* New wake. */
 assert(requests==2 && restarts==0);
 receive_event(State_Event_WillExit,NULL);assert(requests==3 && restarts==0);
}
''')

    def test_legacy_create_cannot_replace_closing_owner_and_publishes_before_start(self):
        for lower, upper in (('asr','ASR'),('tts','TTS')):
            with self.subTest(module=lower):
                path=f'third_party/AI2T_LingXinEngine/src/{lower}/{lower}.c'
                signature=('ASRHandler **handlerAddress,ASRConfig *config,ASREventListener listener'
                           if lower=='asr' else 'TTSHandler **handlerAddress,TTSConfig *config,const char *payload,TTSEventListener listener')
                args='&owner,&config,NULL' if lower=='asr' else '&owner,&config,"payload",NULL'
                run_c(COMMON + r'''
#define WEBSOCKET_ASR_PATH "asr"
#define WEBSOCKET_TTS_PATH "tts"
typedef struct {void*clientHandler;} WebsocketClient;
typedef struct {int unused;} WebsocketConfig;
typedef struct {char *taskId,*requestId,*instanceId;} ASRExtraInfo;
typedef ASRExtraInfo TTSExtraInfo;
typedef void (*ASREventListener)(void);
typedef ASREventListener TTSEventListener;
typedef struct {char *sn,*appKey,*appId;} ASRConfig;
typedef ASRConfig TTSConfig;
typedef struct TTSHandler {WebsocketClient*websocket;ASREventListener listener;ASRExtraInfo*extraInfo;
 struct TTSHandler**selfPointer;const char*payload;bool canRequestNextVoice,isVoiceEnd;void*voiceQueue,*notifyQueue;
} ASRHandler;
typedef ASRHandler TTSHandler;
static ASRHandler old,*owner=&old;
static WebsocketClient ws;
static WebsocketConfig wc;
static int allocations,started;
static void*lingxin_calloc(size_t n,size_t sz){allocations++;return calloc(n,sz);}
static void lingxin_free(void*p){free(p);}
static void onWebSocketEvent(void){}
static WebsocketConfig*createWebsocketConfig(void*h,char*a,char*b,char*c,char*d,void(*e)(void)){return &wc;}
static WebsocketClient*hook_websocket_init(WebsocketConfig*c){return &ws;}
static void free_websocket_config(WebsocketConfig*c){}
static char*generateUUID(int n){return "instance";}
static bool hook_websocket_start(WebsocketClient*w){assert(owner && owner!=&old && owner->selfPointer==&owner);started++;return true;}
'''+'char *'+lower+'Create('+signature+')\n'+body(path,lower+'Create')+r'''
int main(void){ASRConfig config={"sn","key","app"};
 assert('''+lower+'Create('+args+r''')==NULL && !allocations && !started && owner==&old);
 owner=NULL;assert('''+lower+'Create('+args+r''')!=NULL && started==1 && owner && allocations==2);
 free(owner->extraInfo);free(owner);return 0;}
''')

    def test_reuse_rejects_closing_owner_and_pins_handler_through_start(self):
        run_c(COMMON + r'''
typedef struct {bool is_schedule_timer_task;} ChatStartNewParams;
typedef struct {bool closing,waitTerminate;unsigned active_operations;void*chat_config;} VoiceChatHandler;
static VoiceChatHandler h={.chat_config=(void*)1},*globalHandler=&h;
static bool isAIResponseding,isAudioSending,isTaskStarted,isWebsocketConnected=true,locked;
static bool s_handler_close_finalizing;
static uint32_t s_connection_failure_pending_epoch,s_recovery_epoch=7;
static int filled,sent,released,failures;
static bool fail_fill;
static void voice_chat_handler_guard_lock(void){assert(!locked);locked=true;}
static void voice_chat_handler_guard_unlock(void){assert(locked);locked=false;}
static void voice_chat_handler_release(VoiceChatHandler*p){assert(p==&h && h.active_operations==1);h.active_operations--;released++;}
static void on_voice_chat_init_fail(void){failures++;}
static bool voiceChatCreate(ChatStartNewParams*p){assert(0);return false;}
static bool fill_config_params(void*c,ChatStartNewParams*p){assert(c==(void*)1 && !locked && h.active_operations==1);filled++;h.closing=true;return !fail_fill;}
static bool sendStartTask(VoiceChatHandler*p){assert(p==&h && h.active_operations==1);sent++;return false;}
bool voice_chat_start_new(ChatStartNewParams *params)
''' + body(PROTOCOL, 'voice_chat_start_new') + r'''
int main(void){
 ChatStartNewParams p={0};h.closing=true;
 assert(!voice_chat_start_new(&p));assert(!filled && !sent && s_recovery_epoch==7 && !h.active_operations);
 h.closing=false;assert(!voice_chat_start_new(&p));
 assert(filled==1 && sent==1 && released==1 && !h.active_operations && failures==1);
 h.closing=false;fail_fill=true;assert(!voice_chat_start_new(&p));
 assert(filled==2 && sent==1 && released==2 && !h.active_operations);
}
''')

    def test_disconnect_is_not_cleanup_completion(self):
        source = (ROOT / STATE).read_text(encoding='utf-8')
        # Execute the real ExitEnd case for an already exiting, abnormal task.
        start = source.rindex('  case State_Event_VoiceChat_ExitEnd:')
        end = source.index('  case State_Event_TerminatePrompt_PlayEnd:', start)
        exit_case = source[start:end]
        run_c(COMMON + r'''
typedef int ExitCode;
typedef struct {bool record_terminated,buffer_play_terminated,voice_chat_terminated,voice_chat_exited,is_normal_exit;} InnerStateForExit;
typedef struct {InnerStateForExit *inner_state_for_exit;} InnerStateCollection;
typedef union {bool boolean;int exit_code;} ContextValue;
typedef struct {bool is_normal_exit;} Context;
enum {EXIT_REASON_WEBSOCKET_DISCONNECT=1,EXIT_REASON_WEBSOCKET_CONNECTION_FAILED,State_Exit,State_Idle,State_Event_WillExit,
 State_Event_VoiceChat_ExitEnd,CTX_FIELD_IS_VAD_EXIT,CTX_FIELD_EXIT_CODE};
static int chat_current_state=State_Exit,reset_calls,done;
static bool owner=true;
static Context ctx={0};
static InnerStateForExit inner_state_for_exit;
static Context *get_current_context(void){return &ctx;}
static bool isVoiceChatInited(void){return owner;}
static void update_current_context(int f,ContextValue v){}
static void turn_to_with_preset_inner_state(int s,int e,InnerStateCollection*p){reset_calls++;inner_state_for_exit=*p->inner_state_for_exit;}
static void from_exit_to_idle(InnerStateForExit*s,int e){assert(s->record_terminated && s->buffer_play_terminated);done++;}
void state_machine_receive_error(ExitCode exit_code)
''' + body(STATE, 'state_machine_receive_error') + '\nstatic void exit_event(int event){switch(event){\n'+exit_case+r'''
default:break;}}
int main(void){
 state_machine_receive_error(EXIT_REASON_WEBSOCKET_DISCONNECT);
 assert(reset_calls==1 && !inner_state_for_exit.voice_chat_exited);
 inner_state_for_exit.record_terminated=true;inner_state_for_exit.buffer_play_terminated=true;
 owner=false;exit_event(State_Event_VoiceChat_ExitEnd);
 assert(done==1 && reset_calls==1);
}
''')

    def test_legacy_destroy_callbacks_wait_for_close_and_release_config_once(self):
        for lower, upper, destroy, release in (
            ('asr', 'ASR', 'asrDestroy', 'freeASR'),
            ('tts', 'TTS', 'ttsDestroy', 'freeTTS'),
            ('schedule', 'ScheduleChat', 'scheduleWsDestroy', 'freeScheduleWs'),
        ):
            with self.subTest(module=lower):
                path = f'third_party/AI2T_LingXinEngine/src/{lower}/{lower if lower != "schedule" else "schedule_protocol"}.c'
                source = (ROOT / path).read_text(encoding='utf-8')
                done = lower + '_close_completed'
                callback = ('static void '+done+'(void *user)\n'+body(path, done)) if done in source else ''
                event = body(path, 'onWebSocketEvent')
                remote = event.split('case ON_WEBSOCKET_DESTROY:', 1)[1].split('break;', 1)[0]
                run_c(COMMON + r'''
typedef struct WebsocketConfig {void*userContext;} WebsocketConfig;
typedef struct {void*clientHandler;WebsocketConfig*config;bool closing,pending;} WebsocketClient;
typedef struct {char *instanceId,*requestId;} Info;
typedef struct {int sysEventTimerId;bool isDestroying;} Status;
typedef struct Handler {
 WebsocketClient*websocket;struct Handler**selfPointer;Info*extraInfo;
 Status*state;void*state_mutex;void*notifyQueue;void*voiceQueue;
} Handler;
typedef Handler ASRHandler;typedef Handler TTSHandler;typedef Handler ScheduleChatHandler;
#define INVALID_TIMER_ID (-1)
#define ASR_EVENT_ON_DESTROY 1
#define TTS_EVENT_ON_DESTROY 1
static int raw_freed,hook_freed,handler_freed,wrapper_freed,notified;
static int jobs;
static bool reject_job;
static Handler h,*owner=&h;
static WebsocketConfig raw,hook;
static WebsocketClient ws;
static void (*pending_callback)(void*);static void*pending_user;
static void lingxin_free(void*p){if(p==&raw)raw_freed++;else if(p==&h)handler_freed++;else assert(!p);}
static void free_websocket_config(WebsocketConfig*p){assert(p==&hook);hook_freed++;}
static void destroyWebsocketClient(WebsocketClient*p){assert(!p->clientHandler);wrapper_freed++;}
static void lingxin_mutex_lock(void*p){}static void lingxin_mutex_unlock(void*p){}
static void lingxin_mutex_destroy(void*p){}static void lingxin_one_shot_timer_delete(int id){}
static void triggerCallback(Handler*h,int e,const char*d,size_t n){notified++;assert(!h->websocket->clientHandler);}
static void triggerCallbackOrEnqueue(Handler*h,int e,const char*d,size_t n){triggerCallback(h,e,d,n);}
static bool hook_websocket_close(WebsocketClient*w){return false;}
static bool hook_websocket_close_async(WebsocketClient*w,void(*cb)(void*),void*u){
 if(w->pending)return true;if(reject_job)return false;
 w->pending=true;w->closing=true;jobs++;pending_callback=cb;pending_user=u;return true;
}
''' + 'static void '+release+'('+upper+'Handler **handlerAddress)\n'+body(path, release)+
                callback+'void '+destroy+'('+upper+'Handler *handler)\n'+body(path, destroy)+
                'static void remote_destroy(Handler *handler){char *data=NULL;size_t len=0;\n'+remote+'}\n'+r'''
static void setup(void){memset(&h,0,sizeof(h));memset(&ws,0,sizeof(ws));
 owner=&h;h.selfPointer=&owner;h.websocket=&ws;ws.clientHandler=(void*)1;ws.config=&hook;hook.userContext=&raw;}
int main(void){
 setup();reject_job=true;
''' + destroy + r'''(&h);assert(owner==&h && !handler_freed && !hook_freed);
 reject_job=false;
''' + destroy + r'''(&h);
''' + destroy + r'''(&h);
 assert(jobs==1 && !handler_freed && !hook_freed && !notified);
 ws.clientHandler=NULL;pending_callback(pending_user);
 assert(handler_freed==1 && hook_freed==1 && raw_freed==1 && wrapper_freed==1);
 setup();remote_destroy(&h);
 assert(jobs==2 && handler_freed==1);
 /* The hook detaches/frees raw_config only after remote listener returns. */
 hook.userContext=NULL;lingxin_free(&raw);ws.clientHandler=NULL;
 pending_callback(pending_user);
 assert(handler_freed==2 && hook_freed==2 && raw_freed==2 && wrapper_freed==2);
 return 0;
}
''')

    def test_voice_exit_preserves_owner_until_transport_ack_and_operations_drain(self):
        source = (ROOT / PROTOCOL).read_text(encoding='utf-8')
        new = 'voice_chat_close_completed' in source
        claim_signature = 'bool *already_pending' if new else 'void'
        completed = ('static void voice_chat_close_completed(void *user)\n' +
                     body(PROTOCOL, 'voice_chat_close_completed')) if new else ''
        run_c(COMMON + r'''
#define LINGXIN_HANDLER_DRAIN_TIMEOUT_MS 2500U
typedef struct {int id;} WebsocketClient;
typedef struct VoiceChatHandler {
 WebsocketClient *websocket;
 struct VoiceChatHandler **selfPointer;
 unsigned active_operations;
 bool closing, close_requested;
} VoiceChatHandler;
static WebsocketClient ws;
static VoiceChatHandler h;
static VoiceChatHandler *globalHandler=&h;
static bool locked, allocation_fails;
static bool s_handler_close_finalizing;
static int jobs, frees, exits, sleeps;
static void (*completion)(void *);
static void *completion_user;
static bool s_waiting_task_terminated, s_waiting_first_tts, s_diag_waiting_task_started;
static bool isAudioSending=true,isAIResponseding=true,isWebsocketConnected=true,isTaskStarted=true;
static bool s_audio_response_open=true,s_cloud_vad_upload_allowed=true;
static uint32_t s_terminate_deadline_ms,s_first_tts_deadline_ms,s_start_task_deadline_ms,s_end_audio_ack_deadline_ms;
#define State_Event_VoiceChat_ExitEnd 1
static void voice_chat_handler_guard_lock(void){assert(!locked);locked=true;}
static void voice_chat_handler_guard_unlock(void){assert(locked);locked=false;}
static void lingxin_thread_sleep(int ms){assert(!locked && !frees);sleeps+=ms;if(sleeps==2600)h.active_operations=0;}
static bool isVoiceChatInited(void);
static void freeVoiceChat(VoiceChatHandler **p){assert(!h.active_operations);frees++;*p=NULL;assert(isVoiceChatInited());}
static void state_machine_run_event(int e){assert(!globalHandler && frees==1 && isVoiceChatInited());exits++;}
static bool closeWebsocket(WebsocketClient *w){return false;}
static bool hook_websocket_close_async(WebsocketClient*w,void(*cb)(void*),void*u){
 assert(!locked && w==&ws);if(allocation_fails)return false;
 jobs++;completion=cb;completion_user=u;return true;
}
''' + 'static bool isVoiceChatInited(void)\n' + body(PROTOCOL, 'isVoiceChatInited') +
              'static VoiceChatHandler *voice_chat_handler_begin_close(' + claim_signature + ')\n' +
              body(PROTOCOL, 'voice_chat_handler_begin_close') + completed +
              'bool module_voiceChat_exit(void)\n' + body(PROTOCOL, 'module_voiceChat_exit') + r'''
int main(void){
 h.websocket=&ws;h.selfPointer=&globalHandler;
 assert(module_voiceChat_exit());
 assert(globalHandler==&h && h.closing && !frees && !exits && jobs==1);
 assert(module_voiceChat_exit() && jobs==1 && !frees);
 h.active_operations=1;
 completion(completion_user);
 assert(!globalHandler && frees==1 && exits==1 && sleeps==2600);
 assert(!isVoiceChatInited());
 assert(!isWebsocketConnected && !isTaskStarted && !isAudioSending);
 assert(!module_voiceChat_exit() && exits==1);
 /* Allocation failure keeps the old owner closed and retryable. */
 globalHandler=&h;h.close_requested=false;h.closing=false;
 allocation_fails=true;assert(!module_voiceChat_exit());
 assert(globalHandler==&h && h.closing && !h.close_requested);
 allocation_fails=false;assert(module_voiceChat_exit() && jobs==2);
 return 0;
}
''', '-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX')

    def test_state_machine_does_not_enter_idle_when_close_cannot_be_dispatched(self):
        run_c(COMMON + r'''
typedef struct {bool record_terminated,buffer_play_terminated,voice_chat_terminated,voice_chat_exited;} InnerStateForExit;
typedef int StateEvent;
enum {State_Event_Upload_TerminateEnd=1,State_Event_BufferPlay_TerminateEnd,State_Event_VoiceChat_TerminateEnd,
 State_Event_VoiceChat_AIEnd,State_Event_VoiceChat_ExitEnd,State_Idle};
static bool owner=true,accepted;
static int idle,requests;
typedef struct {int exit_code;} Context;
static Context *get_current_context(void){return NULL;}
static bool isVoiceChatInited(void){return owner;}
static bool module_voiceChat_exit(void){requests++;return accepted;}
static void module_voiceChat_terminate(void){}
static void turn_to(int s,int e){idle++;}
static void from_exit_to_idle(InnerStateForExit *inner_state,StateEvent event)
''' + body(STATE, 'from_exit_to_idle') + r'''
int main(void){
 InnerStateForExit s={true,true,true,false};
 from_exit_to_idle(&s,State_Event_VoiceChat_TerminateEnd);assert(requests==1 && idle==0);
 accepted=true;from_exit_to_idle(&s,State_Event_VoiceChat_TerminateEnd);assert(idle==0);
 owner=false;from_exit_to_idle(&s,State_Event_VoiceChat_ExitEnd);assert(idle==1);
 s.voice_chat_exited=false;accepted=false;
 from_exit_to_idle(&s,State_Event_VoiceChat_TerminateEnd);assert(idle==2);
}
''')

    def test_probe_busy_retains_old_connection_and_request_buffers(self):
        run_c(COMMON + r'''
#define CONFIG_VOICE_LINK_LINGXIN 1
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define WEBSOCKET_CHAT_PATH "chat"
#define LISA_WS_OK 0
typedef int lisa_ws_err_e;
typedef struct {uint8_t *scheme,*host,*path,*port;int timeout;char *extra_header;void*user;void(*on_event)(void);void(*on_data)(void);} lisa_ws_request_t;
static struct {void *ws;} s_probe_ctx;
static int cleanup_result, initialized, signatures;
static lisa_ws_request_t previous;
static void lingxin_probe_ws_event(void){}static void lingxin_probe_ws_data(void){}
static const char *lingxin_server_config_cloud_host(void){return "host";}
static int lingxin_server_config_cloud_port(void){return 443;}
static bool lingxin_server_config_provisioned(void){return true;}
static bool lingxin_server_config_ready(void){return true;}
static const char *lingxin_server_config_sn(void){return "sn";}
static const char *lingxin_server_config_app_id(void){return "app";}
static const char *lingxin_server_config_license(void){return "license";}
static long lingxin_get_timestamp_s(void){return 10+signatures;}
static const char *generateSignature(const char*a,const char*b,const char*c,const char*d){signatures++;return "sig";}
static void lingxin_free(void*p){}static void lingxin_trace_clear(void){}
static int lisa_ws_cleanup(void*p){assert(p==(void*)1);return cleanup_result;}
static void *lisa_ws_init(lisa_ws_request_t *r){initialized++;previous=*r;return (void*)1;}
static int lisa_ws_connect(void*p){return 0;}
int lingxin_server_ws_probe_start(void)
''' + body(SERVER, 'lingxin_server_ws_probe_start') + r'''
int main(void){
 assert(lingxin_server_ws_probe_start()==0 && initialized==1);
 char saved[512];strcpy(saved,previous.extra_header);
 cleanup_result=-6;
 assert(lingxin_server_ws_probe_start()==-1);
 assert(s_probe_ctx.ws==(void*)1 && initialized==1 && signatures==1);
 assert(strcmp(saved,previous.extra_header)==0);
 cleanup_result=0;assert(lingxin_server_ws_probe_start()==0 && initialized==2);
}
''')


if __name__ == '__main__':
    unittest.main()
