"""Task identity and replay guards executed from production functions."""
import unittest
from test_lingxin_round_races import ROOT, COMMON, body, run_c, PROTOCOL
from test_lingxin_stage_end_runtime import event_body


class TaskIdentityTest(unittest.TestCase):
    def test_idle_handler_close_waits_on_failure_and_rechecks_owner(self):
        run_c(COMMON + r'''
#define LOGW(...) ((void)0)
#define State_Idle 0
#define LINGXIN_PLAYBACK_READY_WAIT_MS 5000U
#define LINGXIN_PLAYBACK_READY_POLL_MS 20U
static unsigned owner=7,epoch=9,closes,waited;
static uint32_t s_lingxin_exit_sequence;
static bool s_lingxin_exit_callback_complete=true;
static bool inited=true,close_ok=true,cancel;
static bool isVoiceChatInited(void){return inited;}
static unsigned voice_chat_current_epoch(void){return epoch;}
static int get_current_chat_state(void){return State_Idle;}
static bool lingxin_start_request_current(unsigned o,unsigned e){return o==owner&&e==epoch;}
static bool lingxin_audio_playback_input_ready(void){return true;}
static int lingxin_server_chat_stop(void){assert(!"Idle must not issue WillExit");return -1;}
static void lisa_thread_mdelay(unsigned ms){waited+=ms;}
static bool module_voiceChat_exit(void){closes++;if(cancel)owner++;if(close_ok)inited=false;return close_ok;}
static int lingxin_close_previous_voice_session(uint32_t owner,uint32_t *epoch)
''' + body('src/server/lingxin_server/lingxin_server.c', 'lingxin_close_previous_voice_session') + r'''
int main(void) {
 unsigned request_epoch=epoch;
 assert(!lingxin_close_previous_voice_session(7,&request_epoch) && closes==1 && !inited);
 inited=true;close_ok=false;
 assert(lingxin_close_previous_voice_session(7,&request_epoch)==-1 && closes==2 && inited && waited==5000);
 close_ok=true;cancel=true;
 assert(lingxin_close_previous_voice_session(7,&request_epoch)==-2 && closes==3);
}
''')

    def test_start_ack_accepts_official_string_code_only_once(self):
        source = (ROOT / PROTOCOL).read_text(encoding='utf-8')
        guards = event_body(source, 'task_started').split('    if (reqId &&')[0]
        run_c(COMMON + r'''
typedef struct {const char *valuestring;double valuedouble;int type;} cJSON;
static cJSON value;
static bool s_diag_waiting_task_started=true,isTaskStarted;
static int admitted,failures;
static cJSON *cJSON_GetObjectItemCaseSensitive(void*p,const char*k){return &value;}
#define cJSON_IsString(p) ((p)->type==1)
#define cJSON_IsNumber(p) ((p)->type==2)
static unsigned voice_chat_current_epoch(void){return 7;}
static void on_voice_chat_init_fail_for_epoch(unsigned e){assert(e==7);failures++;}
static void ack(void)
''' + guards.replace('message_json', 'NULL') + r'''
 admitted++;s_diag_waiting_task_started=false;isTaskStarted=true;
}
int main(void) {
 value=(cJSON){"20000001",0,1};ack();assert(admitted==1 && !failures);
 ack();assert(admitted==1); /* Duplicate cannot reopen upload. */
 isTaskStarted=false;ack();assert(admitted==1); /* Task already ended. */
 s_diag_waiting_task_started=true;value=(cJSON){0,20000001,2};ack();assert(admitted==2);
 s_diag_waiting_task_started=true;isTaskStarted=false;
 value=(cJSON){"20000001bad",0,1};ack();assert(failures==1 && admitted==2);
 value=(cJSON){0,20000001.5,2};ack();assert(failures==2 && admitted==2);
 value=(cJSON){0,0,0};ack();assert(failures==3 && admitted==2);
}
''')

    def test_strict_connection_and_task_ownership(self):
        run_c(COMMON + r'''
typedef struct {bool closing;} Ws;
typedef struct {const char *taskId;} Config;
typedef struct {Ws *websocket;Config *chat_config;bool closing;} VoiceChatHandler;
static Ws ws;
static Config cfg={"SN^42^1"}, old_cfg={"SN^41^1"};
static VoiceChatHandler h={&ws,&cfg},old={&ws,&old_cfg},*globalHandler=&h;
static bool task_event_is_current(VoiceChatHandler *handler,const char *received_task_id)
''' + body(PROTOCOL,'task_event_is_current') + r'''
int main(void) {
 assert(task_event_is_current(&h,"SN^42^1"));
 assert(!task_event_is_current(&h,"SN^41^1"));
 assert(!task_event_is_current(&h,""));assert(!task_event_is_current(&h,NULL));
 assert(!task_event_is_current(&old,"SN^41^1"));
 ws.closing=true;assert(!task_event_is_current(&h,"SN^42^1"));
 ws.closing=false;h.closing=true;assert(!task_event_is_current(&h,"SN^42^1"));
 h.closing=false;cfg.taskId="";assert(!task_event_is_current(&h,""));
 assert(!task_event_is_current(NULL,"SN^42^1"));
}
''')

    def test_continuous_ids_use_reserved_persistent_sequence(self):
        run_c(COMMON + r'''
#include <limits.h>
#define LOGE(...) ((void)0)
#define LINGXIN_CHAT_SESSION_ID_KEY "session"
#define __atomic_compare_exchange_n(p,e,n,w,s,f) cas(p,e,n)
static bool cas(unsigned *p,unsigned *e,unsigned n) {
 if(*p!=*e){*e=*p;return false;}*p=n;return true;
}
static int stored=80,writes;
static bool reject=true;
static const char *lingxin_server_config_sn(void){return "SN";}
static int lisa_kv_get_int(const char*k,int*v){*v=stored;return 0;}
static int lisa_kv_set_int(const char*k,int v){if(reject)return -1;stored=v;writes++;return 0;}
int lingxin_next_task_id(char *task_id,size_t task_id_size)
''' + body('src/server/lingxin_server/lingxin_server.c','lingxin_next_task_id') + r'''
int main(void) {
 char id[100],expected[100];
 assert(lingxin_next_task_id(id,sizeof id)<0 && stored==80);
 reject=false;
 for(int i=81;i<=150;i++) {
  assert(!lingxin_next_task_id(id,sizeof id));
  snprintf(expected,sizeof expected,"SN^%d^1",i);assert(!strcmp(id,expected));
  assert(stored>=i);
 }
 assert(writes==3 && stored==176);
 assert(lingxin_next_task_id(id,2)<0);
}
''')
