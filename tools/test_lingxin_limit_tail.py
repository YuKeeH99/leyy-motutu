"""Execute frozen PCM drain, paced silence, cancellation and timeout."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_lingxin_round_races import ROOT, CC, RUN, COMMON, body, run_c

REC = 'src/server/lingxin_server/adapter/lingxin_clean_recorder.c'


class LimitTailTest(unittest.TestCase):
    def test_tail_keeps_human_prefix_and_waits_for_cloud(self):
        code = COMMON + r'''
#include "lingxin_capture_buffer.h"
#define CLEAN_FRAME_MAX 1280
#define CLEAN_CLOUD_VAD_WAIT_MS 20000U
#define CONFIG_LINGXIN_CLOUD_VAD_MAX_SENTENCE_SILENCE_MS 600
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define pdMS_TO_TICKS(n) (n)
typedef unsigned TickType_t;
typedef struct {
 uint32_t owner,generation,frame_size;
 volatile bool running,exited,draining,drained;
 bool opened,limit_stop_taken;
 uint32_t tail_bytes;TickType_t limit_started_at;bool limit_timeout_taken;
} clean_recorder_t;
static lingxin_capture_buffer_t s_capture;
static uint32_t s_owner=7, ticks=100, accepted, attempts, delays;
static bool s_capture_frozen=true,s_limit_reached=true, gate;
static clean_recorder_t *ctx;
static uint32_t clean_owner(void){return 7;}
static TickType_t xTaskGetTickCount(void){return ticks;}
static bool lingxin_audio_uplink_gate_is_open(void *p){return gate;}
static bool module_record_is_server_ready(void){return true;}
static void voice_chat_fail_current_round(void){assert(0);}
static void lisa_thread_mdelay(unsigned ms) {
 ticks+=ms;if(ms==40)delays++;
 if(delays==60 || ticks>4000)ctx->running=false;
 assert(ticks<10000);
}
static bool lingxin_try_process_record_data(void *d,int n,uint32_t g) {
 assert(g==s_capture.generation && n==1280);
 if(++attempts==1)return false;
 unsigned char *p=d;
 for(int i=0;i<n;i++) assert(p[i]==(accepted==0 && i<900 ? 0x42 : 0));
 accepted+=n;return true;
}
''' + '\nbool lingxin_clean_capture_current(uint32_t generation)\n' + body(REC,'lingxin_clean_capture_current') + '\nstatic void clean_sender(void *arg)\n' + body(REC,'clean_sender') + '\nbool lingxin_clean_recorder_limit_timed_out(void *recorder)\n' + body(REC,'lingxin_clean_recorder_limit_timed_out') + r'''
int main(void) {
 unsigned char input[900];memset(input,0x42,sizeof input);
 unsigned g=lingxin_capture_begin(&s_capture);
 assert(lingxin_capture_put(&s_capture,input,sizeof input));
 clean_recorder_t r={.owner=7,.generation=g,.frame_size=1280,.running=true,
 .opened=true,.limit_stop_taken=true};ctx=&r;
 assert(lingxin_clean_capture_current(g)); /* Cue gate closed, frozen frames valid. */
 clean_sender(&r);
 assert(accepted==1280+60*1280 && delays==60 && !s_capture.length);
 assert(!r.drained && r.opened);
 assert(!lingxin_clean_recorder_limit_timed_out(&r));
 r.limit_started_at=100;r.tail_bytes=0;ticks=20099;
 assert(!lingxin_clean_recorder_limit_timed_out(&r));
 ticks=20100;assert(lingxin_clean_recorder_limit_timed_out(&r));
 assert(!lingxin_clean_recorder_limit_timed_out(&r));
 r.limit_timeout_taken=false;lingxin_capture_begin(&s_capture);
 assert(!lingxin_clean_recorder_limit_timed_out(&r));
 assert(!lingxin_clean_capture_current(g));
 r.running=true;r.exited=false;clean_sender(&r);assert(r.exited && accepted==1280+60*1280);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp)/'tail.c', Path(tmp)/'tail.elf'
            src.write_text(code)
            directory = ROOT/'src/server/lingxin_server/adapter'
            subprocess.run([str(CC),'--specs=sim.specs','-std=c11','-I',str(directory),str(src),
                            str(directory/'lingxin_capture_buffer.c'),'-o',str(exe)],check=True)
            subprocess.run([str(RUN),str(exe)],check=True,timeout=20)

    def test_speaker_scales_once_without_mutating_input(self):
        run_c(COMMON + r'''
#define LOGE(...) ((void)0)
typedef int lisa_device_t;
static int dev,sent;
static int16_t output[600];
static lisa_device_t *lisa_device_get(const char *n){return &dev;}
static bool lisa_device_ready(void*d){return true;}
static int lisa_audio_play_write(void*d,const int16_t*p,int n) {
 assert(n<=256);memcpy(output+sent,p,n*2);sent+=n;return n;
}
void voice_player_pcm_output(char *data,int size)
''' + body('apps/arcs-mini/voice_player.c','voice_player_pcm_output') + r'''
int main(void) {
 int16_t input[600];for(int i=0;i<600;i++)input[i]=i%2?-32768:32767;
 voice_player_pcm_output((char*)input,sizeof input);assert(sent==600);
 for(int i=0;i<600;i++) {
  assert(input[i]==(i%2?-32768:32767));assert(output[i]==(i%2?-19660:19660));
 }
}
''')
