"""The end-of-input cue precedes the ACK and is never caused by that ACK."""
import unittest
from test_lingxin_round_races import COMMON, body, run_c, PROTOCOL
from test_upload_cue_diagnostics import CUE_SUPPORT, functions


class UploadCueTest(unittest.TestCase):
    def test_vad_and_timeout_share_one_epoch_boundary(self):
        code = COMMON + r'''
#define __atomic_compare_exchange_n(p,e,n,w,s,f) cas(p,e,n)
static uint32_t epoch=7,s_cloud_vad_boundary_epoch;
static bool isTaskStarted=true,s_end_audio_sent,switch_before_cas;
static unsigned failures;
static unsigned int voice_chat_current_epoch(void){return epoch;}
static bool cas(uint32_t *p,uint32_t *e,uint32_t n){
 if(switch_before_cas){switch_before_cas=false;epoch++;*p=epoch;}
 if(*p!=*e){*e=*p;return false;}*p=n;return true;
}
static void on_voice_chat_init_fail_for_epoch(unsigned e){if(e==epoch)failures++;}
static bool claim_cloud_vad_boundary(uint32_t epoch)
''' + body(PROTOCOL,'claim_cloud_vad_boundary') + r'''
void voice_chat_fail_waiting_cloud_vad(unsigned int epoch)
''' + body(PROTOCOL,'voice_chat_fail_waiting_cloud_vad') + r'''
int main(void){
 /* A successful VAD cannot be undone by the same-epoch timeout. */
 assert(claim_cloud_vad_boundary(7));voice_chat_fail_waiting_cloud_vad(7);assert(!failures);
 epoch=8;voice_chat_fail_waiting_cloud_vad(8);assert(failures==1);
 assert(!claim_cloud_vad_boundary(8));voice_chat_fail_waiting_cloud_vad(8);assert(failures==1);
 epoch=9;voice_chat_fail_waiting_cloud_vad(8);assert(failures==1);
 switch_before_cas=true;voice_chat_fail_waiting_cloud_vad(9);assert(epoch==10 && failures==1);
 epoch=11;isTaskStarted=false;voice_chat_fail_waiting_cloud_vad(11);assert(failures==1);
 isTaskStarted=true;s_end_audio_sent=true;voice_chat_fail_waiting_cloud_vad(11);assert(failures==1);
}
'''
        run_c(code,'-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX')

    def test_cloud_end_cues_before_stop_and_exit_only_cancels(self):
        code = COMMON + r'''
typedef enum {CHAT_EVENT_ON_VAD_END, CHAT_EVENT_ON_VAD_EXIT} ChatEventType;
typedef struct {char *taskId;} Config;
typedef struct {void *websocket;Config *chat_config;} Handler;
static Config cfg={"task"};
static Handler h={0,&cfg}, *globalHandler=&h;
static bool isTaskStarted=true, s_cloud_vad_upload_allowed=true, s_cloud_vad_close_requested;
static bool s_record_cue_suppressed;
static uint32_t s_recovery_epoch=7,s_diag_binary_audio_send_bytes=1280;
static unsigned cues,cancels,stops;
static uint32_t voice_chat_current_epoch(void){return 7;}
static bool claim_cloud_vad_boundary(uint32_t epoch){return !s_cloud_vad_close_requested;}
static void websocketGetSendBacklogOnReceive(void*p,unsigned*q,unsigned*r) {*q=*r=0;}
static void notify_recording_ended(uint32_t epoch) {
 assert(epoch==7 && !s_cloud_vad_upload_allowed && !stops);cues++;
}
static void voice_player_cancel_upload_cue(uint32_t epoch) {
 assert(epoch==7 && s_record_cue_suppressed);cancels++;
}
static void notify_all_listeners(ChatEventType e,const char*d,unsigned n) {
 assert(e==CHAT_EVENT_ON_VAD_END ? cues==1 : cancels==1);stops++;
}
static void cloud_vad_close_upload_once(ChatEventType close_event,const char *reason)
''' + body(PROTOCOL,'cloud_vad_close_upload_once') + r'''
int main(void) {
 cloud_vad_close_upload_once(CHAT_EVENT_ON_VAD_END,"vad_end");
 cloud_vad_close_upload_once(CHAT_EVENT_ON_VAD_END,"vad_end");
 assert(cues==1 && stops==1 && !cancels);
 s_cloud_vad_close_requested=false;s_cloud_vad_upload_allowed=true;
 cloud_vad_close_upload_once(CHAT_EVENT_ON_VAD_EXIT,"vad_exit");
 assert(cues==1 && stops==2 && cancels==1);
 return 0;
}
'''
        run_c(code,'-DCONFIG_BOARD_ARCS_MINI_V2_1','-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX')

    def test_cloud_boundary_deduplicates_and_rejects_empty_or_exit(self):
        code = COMMON + r'''
#define __atomic_compare_exchange_n(p,e,n,w,s,f) cas(p,e,n)
static bool cas(uint32_t *p,uint32_t *e,uint32_t n) {
 if(*p!=*e) {*e=*p;return false;} *p=n;return true;
}
static bool s_record_cue_suppressed;
static uint32_t s_record_cue_epoch;
static uint32_t s_recovery_epoch=7, s_diag_binary_audio_send_bytes=1280;
typedef struct {bool waitTerminate;} VoiceChatHandler;
static VoiceChatHandler h;
static VoiceChatHandler *globalHandler=&h;
static unsigned cues;
static void voice_player_notify_upload_complete(uint32_t epoch) {assert(epoch==7);cues++;}
static void notify_recording_ended(uint32_t epoch)
''' + body(PROTOCOL,'notify_recording_ended') + r'''
int main(void) {
 notify_recording_ended(6);assert(!cues && !s_record_cue_epoch);
 h.waitTerminate=true;notify_recording_ended(7);assert(!cues);
 h.waitTerminate=false;s_record_cue_suppressed=true;
 notify_recording_ended(7);assert(!cues);
 s_record_cue_suppressed=false;
 notify_recording_ended(7);assert(cues==1);
 /* Cue playback completion doesn't reopen this per-round claim. */
 notify_recording_ended(7);assert(cues==1);
 s_record_cue_epoch=0;s_diag_binary_audio_send_bytes=0;
 notify_recording_ended(7);assert(cues==1);
 notify_recording_ended(7);assert(cues==1);
 s_record_cue_epoch=0;s_record_cue_suppressed=true;
 notify_recording_ended(7);assert(cues==1);
 return 0;
}
'''
        run_c(code,'-DCONFIG_BOARD_ARCS_MINI_V2_1','-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX')

    def test_short_cue_prefills_player_and_rejects_stale_round(self):
        code = COMMON + CUE_SUPPORT + r'''
typedef struct {uint32_t round_epoch, owner_generation;} upload_cue_request_t;
static uint32_t s_upload_cue_epoch=7;
static uint32_t s_upload_cue_cancelled_epoch;
static const char s_upload_cue_tag[]="cue";
static const char *s_current_tone_url;
static bool s_tone_playing,s_tts_active,s_shutdown_prompt_in_progress,s_current_tone_prompt;
static uint32_t s_current_tone_wakeup_generation;
static void *tone_player=(void*)1;
static int played,ended,released;
static bool fail_write;
#define APP_PLAYER_OK 0
static bool voice_player_upload_epoch_current(uint32_t e) {return e==7;}
static bool callbacks_idle=true;
static bool app_player_callbacks_idle(void *p) {return callbacks_idle;}
static void voice_player_release_upload_cue(uint32_t e) {released++;}
static void voice_player_local_tone_gate_begin(void) {}
static void voice_player_local_tone_gate_end(void) {}
static int app_player_play_stream(void*p,int hz,int ch,int bits) {
 assert(hz==16000 && ch==1 && bits==16);played++;return 0;
}
static int app_player_write_stream(void*p,const uint8_t *data,uint32_t size,uint32_t timeout) {
 /* Real first write waits PREPARED before the caller can send EOF. */
 assert(size>=8192);
 const int16_t *samples=(const int16_t*)data;
 assert(samples[0]==0 && samples[1279]==0 && samples[4]!=0);
 for(unsigned i=1280;i<size/2;i++) assert(samples[i]==0);
 return fail_write ? size-2 : size;
}
static int app_player_finish_stream(void*p) {ended++;return 0;}
static int app_player_stop(void*p) {return 0;}
''' + functions('voice_player_upload_cue_note', 'voice_player_upload_cue_event') + r'''
static void voice_player_start_upload_cue(void *data,uint32_t len)
''' + body('apps/arcs-mini/voice_player.c','voice_player_start_upload_cue') + r'''
int main(void) {
 upload_cue_request_t r={6,1};
 voice_player_start_upload_cue(&r,sizeof r);assert(!played && released==1);
 r.round_epoch=7;s_upload_cue_cancelled_epoch=7;
 voice_player_start_upload_cue(&r,sizeof r);assert(!played && released==2);
 assert(s_upload_cue_stats.skipped_stale==2);
 s_upload_cue_cancelled_epoch=6;
 s_tone_playing=true;voice_player_start_upload_cue(&r,sizeof r);s_tone_playing=false;
 assert(s_upload_cue_stats.skipped_busy==1 && !played);
 s_tts_active=true;voice_player_start_upload_cue(&r,sizeof r);s_tts_active=false;
 assert(s_upload_cue_stats.skipped_tts==1 && !played);
 callbacks_idle=false;voice_player_start_upload_cue(&r,sizeof r);callbacks_idle=true;
 assert(s_upload_cue_stats.skipped_callbacks==1 && !played);
 voice_player_start_upload_cue(&r,sizeof r);
 assert(played==1 && ended==1 && s_tone_playing);
 assert(s_upload_cue_playing_epoch==7 && !s_upload_cue_stats.started);
 /* A short write is a failed cue, never a successful completion. */
 s_tone_playing=false;fail_write=true;
 voice_player_start_upload_cue(&r,sizeof r);
 assert(!s_tone_playing && !s_upload_cue_playing_epoch && s_upload_cue_stats.playback_failures==1);
 assert(ended==1 && !s_upload_cue_stats.completed);
 return 0;
}
'''
        run_c(code)

    def test_ack_does_not_trigger_cue(self):
        code = COMMON + r'''
static bool s_end_audio_sent, s_audio_ended_received;
static uint32_t s_diag_binary_audio_send_bytes, s_recovery_epoch=7;
static int cues;
typedef struct { bool waitTerminate; } VoiceChatHandler;
static void voice_player_notify_upload_complete(uint32_t epoch) {assert(epoch==7);cues++;}
static void notify_audio_upload_acked(VoiceChatHandler *handler)
''' + body(PROTOCOL,'notify_audio_upload_acked') + r'''
int main(void) {
 VoiceChatHandler h={0};
 notify_audio_upload_acked(&h); assert(!cues);
 s_audio_ended_received=false; s_end_audio_sent=true;
 notify_audio_upload_acked(&h); assert(!cues); /* No submitted voice PCM. */
 s_audio_ended_received=false;s_diag_binary_audio_send_bytes=256000;
 notify_audio_upload_acked(&h);notify_audio_upload_acked(&h);assert(cues==0);
 s_audio_ended_received=false;h.waitTerminate=true;
 notify_audio_upload_acked(&h);assert(cues==0);
 return 0;
}
'''
        run_c(code,'-DCONFIG_BOARD_ARCS_MINI_V2_1','-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX')
