"""Exercise production PCM write loops and playback deadline policy."""
import unittest
from test_lingxin_round_races import body, run_c, COMMON

ADAPTER = "src/server/lingxin_server/adapter/lingxin_audio_adapter.c"
STUBS = COMMON + r'''
#define LOGE(...) ((void)0)
#define LOGI(...) ((void)0)
#define LINGXIN_PLAYBACK_PLAYER_TYPE 1
#define LINGXIN_STREAM_WRITE_TIMEOUT_MS 100
#define LINGXIN_STREAM_STALL_TIMEOUT_MS 3000U
#define VOICE_MSG_PLAYER_TTS_PLAYING 1
static bool s_stream_player_started, s_stream_suppress_stop_event, live=true;
static uint32_t now, s_stream_pcm_written_bytes, s_stream_last_progress_ms;
static uint8_t bytes[8];
static int calls, mode, offset;
static uint32_t lingxin_playback_now_ms(void) {return now;}
static bool lingxin_stream_writer_is_current(void) {return live;}
static void lisa_thread_mdelay(unsigned n) {now+=n;if(mode==2)live=false;}
static void voice_msg_pub_timeout(int a,void*b,int c,int d){}
static int lingxin_compat_player_put_stream_data(int p,uint8_t *data,unsigned size,unsigned wait){
 assert(data==bytes+offset); assert(size==8-offset); calls++;
 if(mode) {now+=wait;return 0;}
 int count=calls==1?3:calls==2?0:calls==3?2:3;
 offset+=count;return count;
}
static bool lingxin_stream_submit_pcm(const uint8_t *data,uint32_t pcm_bytes)
'''

class PlaybackProgressTest(unittest.TestCase):
    def test_callback_registration_keeps_packet_owner_during_new_wakeup(self):
        run_c(COMMON + r'''
#define LOGE(...) ((void)0)
#define PLAYER_T_CLOUD 1
#define APP_PLAYER_OK 0
typedef unsigned player_t;
typedef int app_player_t;
typedef int (*player_status_cb)(uint16_t);
typedef struct {app_player_t *player;player_status_cb status_cb;bool callback_registered;} lingxin_player_compat_t;
static lingxin_player_compat_t ctx;
static app_player_t player;
static uint32_t s_playback_generation=8,registered;
static lingxin_player_compat_t *lingxin_compat_get_ctx(player_t t){return &ctx;}
static app_player_t *lingxin_compat_resolve_player(player_t t){return &player;}
static void lingxin_cloud_player_event_cb(void){}
static void lingxin_compat_player_event_cb(void){}
static int app_player_register_callback(app_player_t*p,void(*cb)(void),void*arg){registered=(unsigned)(uintptr_t)arg;s_playback_generation++;return 0;}
static bool lingxin_compat_prepare_player(player_t type,player_status_cb cb,lingxin_player_compat_t**out_ctx,uint32_t generation)
''' + body(ADAPTER, "lingxin_compat_prepare_player") + r'''
static int cb(uint16_t st){return 0;}
int main(void){lingxin_player_compat_t *out=NULL;assert(lingxin_compat_prepare_player(1,cb,&out,7));
assert(registered==7&&s_playback_generation==9&&out==&ctx);}
''')

    def test_action_suppression_after_eof_finishes_new_playback_generation_once(self):
        run_c(COMMON + r'''
#define LOGI(...) ((void)0)
#define LINGXIN_PLAYBACK_PLAYER_TYPE 1
#define VOICE_MSG_PLAYER_TTS_STOPED 1
#define Lingxin_Download_Audio_PlayEnd 2
static uint32_t s_playback_generation=4,s_stream_epoch=3;
static bool s_tts_suppress_policy,s_stream_playing=true,s_stream_mode_started=true,s_stream_player_started=true;
static bool s_stream_suppress_stop_event,s_stream_write_failed,s_stream_audio_ended=true;
static int lock,events,stops,frees;
static bool lingxin_tts_suppress_is_active(bool*p){return *p;}
static void lingxin_tts_suppress_request(bool*p){*p=true;}
static unsigned lingxin_playback_advance_generation(void){return ++s_playback_generation;}
static bool lingxin_playback_lifecycle_lock(void){lock++;return true;}
static void lingxin_playback_lifecycle_unlock(void){lock--;}
static void voice_chat_fail_epoch(unsigned e){assert(0);}
static bool lingxin_stream_writer_stop(void){return true;}
static bool lingxin_compat_player_stop_sync(int p){stops++;return true;}
static void lingxin_stream_pcm_prefill_reset(void){frees++;}
static void lingxin_playback_release_input(unsigned gen){assert(gen==s_playback_generation&&lock);}
static void voice_msg_pub_timeout(int a,void*b,int c,int d){}
static bool lingxin_post_playback_event_async(int e,const char*r,unsigned gen){assert(gen==s_playback_generation&&!lock);events++;return true;}
void lingxin_audio_suppress_current_tts(const char*reason)
''' + body(ADAPTER, "lingxin_audio_suppress_current_tts") + r'''
int main(void){lingxin_audio_suppress_current_tts("dance");assert(events==1&&stops==1&&frees==1&&!s_stream_playing);
lingxin_audio_suppress_current_tts("dance");assert(events==1&&stops==1&&s_playback_generation==5);
s_tts_suppress_policy=false;s_stream_audio_ended=false;s_stream_playing=true;
lingxin_audio_suppress_current_tts("music");assert(events==1&&stops==2&&!lock);}
''')

    def test_late_cleanup_completes_exit_and_old_cleanup_cannot_finish_new_round(self):
        run_c(COMMON + r'''
static uint32_t s_playback_generation=4;
static bool s_stream_playing=true,s_stream_mode_started=true,s_stream_player_started=true,s_stream_audio_ended,s_player_cleanup_started;
static int lock,stops,resets,events,frees,delays,mode;
#define LINGXIN_PLAYBACK_PLAYER_TYPE 1
#define VOICE_MSG_PLAYER_TTS_STOPED 1
#define Lingxin_Download_Audio_TerminateEnd 2
static bool lingxin_playback_lifecycle_lock(void){assert(!lock);lock=1;return true;}
static void lingxin_playback_lifecycle_unlock(void){assert(lock);lock=0;}
static bool lingxin_stream_writer_stop(void){assert(lock);return ++stops>=3;}
static bool lingxin_compat_player_stop_sync(int p){assert(lock);return ++resets>=2;}
static void lingxin_stream_pcm_prefill_reset(void){frees++;}
static void lingxin_playback_release_input(unsigned gen){assert(gen==s_playback_generation&&lock);}
static void voice_msg_pub_timeout(int a,void*b,int c,int d){}
static bool lingxin_post_playback_event_async(int e,const char*r,unsigned gen){assert(!lock&&gen==4);events++;return true;}
static void lisa_thread_mdelay(unsigned n){assert(!lock);delays++;if(mode)s_playback_generation++;}
static void lingxin_stream_terminate_thread(void *arg)
''' + body(ADAPTER, "lingxin_stream_terminate_thread") + r'''
int main(void){lingxin_stream_terminate_thread((void*)4);assert(events==1&&frees==1&&delays==3&&s_stream_audio_ended&&!lock);
mode=1;stops=resets=events=frees=delays=0;s_stream_playing=true;s_stream_audio_ended=false;
lingxin_stream_terminate_thread((void*)4);assert(delays==1&&!events&&!frees&&s_stream_playing&&!s_stream_audio_ended);}
''')

    def test_old_packet_and_old_eof_cannot_mutate_new_response(self):
        run_c(COMMON + r'''
static uint32_t s_playback_generation=4,s_stream_response_id=4,s_stream_epoch=4,s_stream_chunk_count,s_stream_bytes_total;
static bool s_stream_response_ready=true,s_stream_audio_ended,s_stream_write_failed,s_stream_mode_started=true;
static int s_tts_suppress_policy,lock,mutations,mode;
#define Lingxin_Download_Audio_PlayEnd 1
static bool lingxin_playback_lifecycle_lock(void){lock++;s_playback_generation++;if(!mode)s_stream_response_id++;return true;}
static void lingxin_playback_lifecycle_unlock(void){lock--;}
static void lingxin_stream_fail(unsigned gen,const char*r){mutations++;}
static bool lingxin_tts_suppress_should_drop_audio(void*p){return false;}
static bool lingxin_tts_suppress_is_active(void*p){return mode!=0;}
static bool lingxin_stream_start_playback(unsigned gen){mutations++;return true;}
static bool lingxin_stream_enqueue_copy(const void*p,unsigned len,bool end,unsigned gen){mutations++;return true;}
static void lingxin_stream_throttle_producer(unsigned gen){assert(!lock);}
static bool lingxin_stream_writer_stop(void){mutations++;return true;}
static void lingxin_stream_pcm_prefill_reset(void){mutations++;}
static void voice_chat_fail_epoch(unsigned epoch){mutations++;}
static void lingxin_post_playback_event_async(int e,const char*r,unsigned gen){mutations++;}
void module_bufferPlay_data(void*buf,int rlen)
''' + body(ADAPTER, "module_bufferPlay_data") + r'''
void module_bufferPlay_audioEnd(void)
''' + body(ADAPTER, "module_bufferPlay_audioEnd") + r'''
int main(void){char pcm[8];module_bufferPlay_data(pcm,8);assert(!mutations&&!s_stream_chunk_count&&!lock);
module_bufferPlay_audioEnd();assert(!s_stream_audio_ended&&!mutations&&!lock);
mode=1;module_bufferPlay_audioEnd();assert(s_stream_audio_ended&&mutations==3&&!lock);}
''')

    def test_progressing_ninety_second_answer_is_not_cut_off(self):
        run_c(COMMON + r'''
typedef struct {uint32_t generation,epoch;} lingxin_playback_watchdog_ctx_t;
static uint32_t s_playback_generation=4,s_stream_pcm_written_bytes,now;
static bool s_stream_playing=true,s_stream_write_failed;
static int mode,failures;
#define LINGXIN_PLAYBACK_STALL_MS 10000U
#define lisa_mem_free free
static unsigned lingxin_playback_now_ms(void){return now;}
static void lisa_thread_mdelay(unsigned n){now+=n;
 if(mode==0){s_stream_pcm_written_bytes+=6400;if(now>=90000)s_stream_playing=false;}
 if(mode==2&&now>=400)s_playback_generation++;
}
static void lingxin_stream_fail(unsigned gen,const char *reason){assert(gen==4);failures++;s_stream_write_failed=true;}
static void lingxin_playback_watchdog_thread(void *arg)
''' + body(ADAPTER, "lingxin_playback_watchdog_thread") + r'''
static void run(void){lingxin_playback_watchdog_ctx_t *ctx=calloc(1,sizeof(*ctx));ctx->generation=4;lingxin_playback_watchdog_thread(ctx);}
int main(void){run();assert(now==90000&&!failures);
 mode=1;now=0;s_stream_playing=true;run();assert(failures==1&&now==10000);
 mode=2;now=0;failures=0;s_stream_write_failed=false;run();assert(!failures&&now==400);}
''')

    def test_queue_byte_limit_reclaims_consumed_packets_and_cancelled_copy(self):
        run_c(COMMON + r'''
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LINGXIN_STREAM_QUEUE_MAX_BYTES (1536U*1024U)
#define LINGXIN_STREAM_CHUNK_MAX_BYTES 8192U
#define LISA_OK 0
typedef int lisa_queue_t;
typedef struct {uint8_t *data;uint32_t len;bool end;} lingxin_stream_write_item_t;
static lisa_queue_t q,*s_stream_write_queue=&q;
static uint32_t s_playback_generation=4,s_stream_queue_bytes,s_stream_queue_peak_bytes,s_stream_enqueue_active;
static bool s_stream_writer_running=true,s_stream_write_failed,cancel;
static lingxin_stream_write_item_t packets[2048];
static unsigned n,allocated,freed;static char data[16384];
static void *lisa_mem_alloc(unsigned size){allocated++;if(cancel)s_playback_generation++;return malloc(size);}
static void lisa_mem_free(void*p){freed++;free(p);}
static int lisa_queue_push(lisa_queue_t*q,const void*data,unsigned size,unsigned wait){assert(wait==0);packets[n++]=*(const lingxin_stream_write_item_t*)data;return 0;}
static void lingxin_stream_write_item_free(lingxin_stream_write_item_t *item)
''' + body(ADAPTER, "lingxin_stream_write_item_free") + r'''
static bool lingxin_stream_enqueue_copy(const void*data,uint32_t len,bool end,uint32_t generation)
''' + body(ADAPTER, "lingxin_stream_enqueue_copy") + r'''
int main(void){for(int i=0;i<96;i++)assert(lingxin_stream_enqueue_copy(data,sizeof(data),false,4));
assert(s_stream_queue_bytes==LINGXIN_STREAM_QUEUE_MAX_BYTES&&n==192);
assert(!lingxin_stream_enqueue_copy(data,1,false,4));
lingxin_stream_write_item_free(&packets[0]);assert(lingxin_stream_enqueue_copy(data,8192,false,4));
assert(lingxin_stream_enqueue_copy(NULL,0,true,4));
for(unsigned i=0;i<n;i++)lingxin_stream_write_item_free(&packets[i]);
assert(!s_stream_queue_bytes&&allocated==freed&&!s_stream_enqueue_active);
cancel=true;assert(!lingxin_stream_enqueue_copy(data,100,false,4));
assert(allocated==freed&&!s_stream_queue_bytes&&!s_stream_enqueue_active);}
''')

    def test_short_write_preserves_tail_and_zero_write_retries(self):
        run_c(STUBS + body(ADAPTER, "lingxin_stream_submit_pcm") + r'''
int main(void){assert(lingxin_stream_submit_pcm(bytes,8));assert(offset==8&&calls==4);assert(s_stream_pcm_written_bytes==8);}
''')

    def test_zero_progress_is_bounded_and_cancel_is_prompt(self):
        run_c(STUBS + body(ADAPTER, "lingxin_stream_submit_pcm") + r'''
int main(void){mode=1;assert(!lingxin_stream_submit_pcm(bytes,8));assert(now>=3000&&now<=3200);
now=0;calls=0;mode=2;live=true;assert(!lingxin_stream_submit_pcm(bytes,8));assert(calls==1&&now<200);}
''')

if __name__ == "__main__": unittest.main()
