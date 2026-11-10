"""Local audio filtering is checked when starting queued audio, not only on send."""
import unittest
from tools.test_offline_action import OfflineActionTest, c_function, ROOT


class OfflineLocalAudioTest(unittest.TestCase):
    run_c = OfflineActionTest.run_c

    def test_provisioning_waits_for_cancel_ack_and_old_callbacks(self):
        source = (ROOT/'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        handler = c_function(source, 'voice_player_handle_msg')
        cancel = handler[handler.index('    case VOICE_MSG_OFFLINE_AUDIO_CANCEL:'):
                         handler.index('    case VOICE_MSG_OFFLINE_LOCAL_VOICE:')]
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define VOICE_MSG_OFFLINE_AUDIO_CANCEL 14
static volatile uint32_t s_offline_audio_cancel_generation;
static void *tone_player=(void*)1,*tts_player=(void*)2,*beat_player=(void*)3;
static uint32_t s_beat_music_owner=7;
static uint32_t owner=7;
static bool tone_pending,tts_pending,beat_pending;
static unsigned stops,flushes;
static bool voice_player_offline_audio_cancel_ready(uint32_t generation);
static uint32_t service_offline_action_generation(void) {return owner;}
static bool app_player_callbacks_idle(void *p) {
 return p==tone_player?!tone_pending:p==beat_player?!beat_pending:!tts_pending;
}
static void voice_player_beat_music_cancel(void) {s_beat_music_owner=0;}
static void voice_player_cancel_cloud_connect_success_tone(void) {}
static void voice_player_flush_tone_queue(void) {++flushes;}
static void app_player_stop(void *p) {
 assert(!voice_player_offline_audio_cancel_ready(owner));++stops;
 if(p==tone_player) tone_pending=true;else if(p==beat_player) beat_pending=true;else tts_pending=true;
}
''' + c_function(source, 'voice_player_offline_audio_cancel_ready') + r'''
static void cancel_audio(void *data,uint32_t len) {switch(14) {
''' + cancel + r'''
}}
int main(void) {
 assert(!voice_player_offline_audio_cancel_ready(owner));
 uint32_t stale=6;cancel_audio(&stale,4);assert(!stops);
 cancel_audio(&owner,3);assert(!stops);
 cancel_audio(&owner,4);assert(stops==3 && flushes==1 && s_beat_music_owner==0);
 assert(!voice_player_offline_audio_cancel_ready(owner));
 tone_pending=false;assert(!voice_player_offline_audio_cancel_ready(owner));
 tts_pending=false;assert(!voice_player_offline_audio_cancel_ready(owner));
 beat_pending=false;assert(voice_player_offline_audio_cancel_ready(owner));
 s_beat_music_owner=owner;assert(!voice_player_offline_audio_cancel_ready(owner));
 s_beat_music_owner=0;
 ++owner;assert(!voice_player_offline_audio_cancel_ready(owner));
 cancel_audio(&stale,4);assert(!voice_player_offline_audio_cancel_ready(owner));
 cancel_audio(&owner,4);tone_pending=tts_pending=beat_pending=false;
 assert(voice_player_offline_audio_cancel_ready(owner));
 return 0;
}
''')

    def test_socket_reuse_survives_wake_but_not_network_mode_change(self):
        source=(ROOT/'src/server/lingxin_server/adapter/lingxin_websocket_adapter.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define ON_WEBSOCKET_DATA_RECEIVED 1
#define LISA_WS_BIN 2
typedef struct {void (*listener)(int,const char*,size_t,int,void*);void *userContext;} Config;
typedef struct {Config *config;unsigned owner_generation;unsigned owner_network_epoch;int closing;} lingxin_ws_ctx_t;
typedef struct {void *user,*buf;int len,type;} lisa_ws_data_t;
static unsigned owner=8,received,network_epoch=4;
static bool online=true;
static bool service_offline_action_network_current(unsigned epoch) {return online && epoch==network_epoch;}
static bool service_offline_action_online_current(unsigned generation) {return online && owner==generation;}
static void receive(int event,const char *data,size_t size,int binary,void *user) {received++;}
''' + c_function(source,'lingxin_ws_on_data') + r'''
int main(void) {
 Config config={receive,0};lingxin_ws_ctx_t ctx={.config=&config,.owner_generation=7,.owner_network_epoch=4};
 lisa_ws_data_t data={&ctx,"task_terminated",15,0};
 /* A newer wake reuses the socket and must receive the old task's termination. */
 lingxin_ws_on_data(&data);assert(received==1);
 owner++;data.buf="task_started";lingxin_ws_on_data(&data);assert(received==2);
 data.buf="new TTS";data.type=LISA_WS_BIN;lingxin_ws_on_data(&data);assert(received==3);
 ctx.closing=1;lingxin_ws_on_data(&data);assert(received==3);
 ctx.closing=0;
 network_epoch++;lingxin_ws_on_data(&data);assert(received==3);
 ctx.owner_network_epoch=network_epoch;online=false;lingxin_ws_on_data(&data);assert(received==3);
 return 0;
}
''')

    def test_old_uplink_release_cannot_reopen_explicit_offline_recording(self):
        source=(ROOT/'src/server/lingxin_server/adapter/lingxin_audio_adapter.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LOGI(...) ((void)0)
static unsigned s_record_uplink_gate_generation=5,s_record_uplink_gate_drop_count;
static bool s_record_uplink_gate_open,online;
static void *s_active_recorder;
static unsigned service_offline_action_generation(void) {return 9;}
static bool service_offline_action_online_current(unsigned generation) {return online && generation==9;}
static void lingxin_record_discard_buffered(void *r) {}
static void lingxin_trace_set(const char *r) {}
static void lingxin_server_diag_timing_mark(const char *r,int n) {}
''' + c_function(source,'lingxin_audio_uplink_gate_open') + r'''
int main(void) {
 lingxin_audio_uplink_gate_open(5,"late");assert(!s_record_uplink_gate_open);
 online=true;lingxin_audio_uplink_gate_open(4,"old");assert(!s_record_uplink_gate_open);
 lingxin_audio_uplink_gate_open(5,"current");assert(s_record_uplink_gate_open);
 return 0;
}
''')

    def test_delayed_cloud_url_never_plays_after_owner_changes(self):
        source=(ROOT/'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY 0
#define ASYNC_PLAY_REQUEST_RESUME 1
typedef struct {int type,bypass_tts_gate,wait_mp3_ready;void *player;char url[256];uint32_t owner_generation;} async_play_request_t;
static void *tts_player=(void*)1,*s_async_play_queue;
static unsigned owner=7,plays,takes;
static bool s_tts_active;
static jmp_buf finished;
static bool service_offline_action_online_current(uint32_t g) {return owner==g;}
static bool voice_intent_photo_flow_should_gate_tts(void) {return false;}
static bool voice_player_url_is_mp3(const char *u) {return true;}
static int voice_player_wait_pushup_tts_mp3_ready(const char *u,uint32_t generation) {owner++;return 0;}
static int app_player_play(void *p,const char *u) {plays++;return 0;}
static int app_player_resume(void *p) {plays++;return 0;}
static int xQueueReceive(void *q,async_play_request_t *r,unsigned w) {
 if(takes++)longjmp(finished,1);
 *r=(async_play_request_t){.wait_mp3_ready=1,.player=(void*)1,.url="old.mp3",.owner_generation=7};return 1;
}
''' + c_function(source,'async_play_task') + r'''
int main(void) {
 if(!setjmp(finished))async_play_task(0);
 assert(plays==0 && owner==8);
 return 0;
}
''')

    def test_local_sequence_stops_on_generation_change_or_missing_resource(self):
        source = (ROOT/'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        name = 'voice_player_advance_local_sequence'
        production = c_function(source,name) if 'static bool '+name in source else 'static bool voice_player_advance_local_sequence(void) {return false;}'
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct {uint32_t local_generation;uint16_t tokens[32];unsigned token_count,token_index;} tone_play_request_t;
static tone_play_request_t s_current_local_request;
static uint32_t owner=7;
static unsigned played,done,failed,missing;
static uint16_t heard[32];
static bool service_offline_action_local_audio_current(uint32_t generation) {return generation==owner;}
static void service_offline_action_local_audio_finished(uint32_t generation,bool ok) {if(ok)++done;else++failed;}
static bool voice_player_start_tone_request(const tone_play_request_t *r) {
 if(r->tokens[r->token_index]==missing)return false;
 heard[played++]=r->tokens[r->token_index];s_current_local_request=*r;return true;
}
''' + production + r'''
int main(void) {
 s_current_local_request=(tone_play_request_t){7,{119,123,120,127,120,124},6,0};
 while(voice_player_advance_local_sequence()) {}
 const uint16_t rest[]={123,120,127,120,124};
 assert(played==5 && memcmp(heard,rest,sizeof(rest))==0 && done==1);
 s_current_local_request=(tone_play_request_t){7,{119,123,120,127},4,0};
 ++owner;
 assert(!voice_player_advance_local_sequence() && played==5 && failed==1);
 s_current_local_request=(tone_play_request_t){8,{119,123,120,127},4,0};
 missing=123;
 assert(!voice_player_advance_local_sequence() && played==5 && failed==2);
 assert(!voice_player_advance_local_sequence() && failed==2);
 return 0;
}
''')

    def test_network_tones_are_muted_only_by_explicit_offline_selection(self):
        source = (ROOT/'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        name = 'voice_player_network_tone_allowed'
        production = c_function(source,name) if 'static bool '+name in source else 'static bool voice_player_network_tone_allowed(const char *url) {return true;}'
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
static bool forced;
static bool service_offline_action_network_prompt_allowed(void) {return !forced;}
static const char *app_tone_get_url(unsigned n) {
 switch(n) {case 64:return "net-down";case 70:return "portal";case 72:return "configured";
 case 116:return "low-power";case 117:return "offline";default:return 0;}
}
''' + production + r'''
int main(void) {
 assert(voice_player_network_tone_allowed("net-down"));
 forced=true;
 assert(!voice_player_network_tone_allowed("net-down"));
 assert(voice_player_network_tone_allowed("portal"));
 assert(!voice_player_network_tone_allowed("configured"));
 assert(voice_player_network_tone_allowed("low-power"));
 assert(voice_player_network_tone_allowed("offline"));
 forced=false;assert(voice_player_network_tone_allowed("net-down"));
 return 0;
}
''')
