"""Run the real capture buffer on the RISC-V simulator."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_v2_rgb_effect_contract import ROOT, CC, RUN
from test_recorder_manager_lifecycle_contract import function_body


class CleanCaptureTest(unittest.TestCase):
    def test_capture_diagnostics_distinguish_muted_silence_and_signal(self):
        from tools import test_offline_action as support
        directory = ROOT / 'src/server/lingxin_server/adapter'
        source = (directory / 'lingxin_clean_recorder.c').read_text(encoding='utf-8')
        self.assertIn('void lingxin_clean_capture_get_stats(', source)
        functions = ''.join(support.c_function(source, name) for name in (
            'lingxin_audio_capture_clean', 'lingxin_clean_capture_get_stats'))
        code = r'''
#include <assert.h>
#include <string.h>
#include "lingxin_capture_buffer.h"
#include "lingxin_clean_recorder.h"
#define CLEAN_FRAME_MAX 1280U
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LOGE(...) ((void)0)
static lingxin_capture_buffer_t s_capture;
static uint32_t s_owner=1;
static bool s_capture_frozen, s_limit_reached, gate;
static lingxin_capture_stats_t s_capture_stats[LINGXIN_CAPTURE_STATS_SLOTS];
static uint32_t clean_owner(void) {return 1;}
static bool lingxin_audio_uplink_gate_is_open(void *p) {return gate;}
''' + functions + r'''
int main(void) {
 int16_t samples[256]={0};
 lingxin_capture_stats_t stats[LINGXIN_CAPTURE_STATS_SLOTS];
 unsigned g=lingxin_capture_begin(&s_capture), i=g%LINGXIN_CAPTURE_STATS_SLOTS;
 samples[0]=-32768;
 lingxin_audio_capture_clean(samples,256); /* Speaker gate excludes it. */
 gate=true; memset(samples,0,sizeof samples);
 lingxin_audio_capture_clean(samples,256); /* Accepted silence is not speech. */
 samples[0]=-32768;
 lingxin_audio_capture_clean(samples,256);
 s_capture_frozen=true; lingxin_audio_capture_clean(samples,256);
 lingxin_clean_capture_get_stats(stats);
 assert(stats[i].frames==4 && stats[i].gate_drops==1 && stats[i].capture_drops==1);
 assert(stats[i].accepted_samples==512 && stats[i].accepted_peak==32768);
 assert(stats[i].accepted_signal_frames==1 && stats[i].gated_peak==32768);
 assert(stats[i].max_queued==1024 && s_capture.length==1024);
 /* Eight seconds of listening/silence must NOT freeze the stream. */
 s_capture_frozen=false;
 s_capture_stats[i].accepted_samples=127900;
 lingxin_audio_capture_clean(samples,256);
 lingxin_clean_capture_get_stats(stats);
 assert(stats[i].accepted_samples==128156 && !s_capture_frozen);
 assert(s_capture.length==1536);
 lingxin_audio_capture_clean(samples,256);
 assert(s_capture.length==2048);
 /* A later round must not erase the preceding round's evidence. */
 lingxin_capture_begin(&s_capture);s_capture_frozen=false;
 lingxin_audio_capture_clean(samples,256);
 lingxin_clean_capture_get_stats(stats);assert(stats[i].generation==g && stats[i].frames==6);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            test, elf = Path(tmp) / 'levels.c', Path(tmp) / 'levels.elf'
            test.write_text(code, encoding='utf-8')
            vad = ROOT / 'third_party/libfvad/src'
            sources = list((vad/'vad').glob('*.c')) + list((vad/'signal_processing').glob('*.c'))
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(directory), '-I', str(vad),
                            str(test), str(directory / 'lingxin_capture_buffer.c'),
                            str(directory / 'lingxin_speech_limit.c'), *map(str,sources), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)

    def test_cold_handshake_preserves_eight_second_prefix(self):
        directory = ROOT / 'src/server/lingxin_server/adapter'
        with tempfile.TemporaryDirectory() as tmp:
            test, elf = Path(tmp) / 'cold.c', Path(tmp) / 'cold.elf'
            test.write_text(r'''
#include <assert.h>
#include <string.h>
#include "lingxin_capture_buffer.h"
static lingxin_capture_buffer_t b;
int main(void) {
    unsigned char in[640], out[640];
    lingxin_capture_begin(&b);
    /* TLS/start_task can take longer than four seconds on a cold socket. */
    for (unsigned i=0; i<400; ++i) {
        memset(in, (unsigned char)i, sizeof(in));
        assert(lingxin_capture_put(&b, in, sizeof(in)));
    }
    for (unsigned i=0; i<400; ++i) {
        assert(lingxin_capture_take(&b, out, sizeof(out)));
        for (unsigned j=0; j<sizeof(out); ++j) assert(out[j]==(unsigned char)i);
    }
    assert(!b.length && !b.overflow);
    /* After catching up, a slow transport must not build a 16-second delay. */
    for (unsigned i=0; i<200; ++i)
        assert(lingxin_capture_put(&b, in, sizeof(in)));
    assert(!lingxin_capture_put(&b, in, sizeof(in)) && b.overflow);
}
''', encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(directory),
                            str(test), str(directory / 'lingxin_capture_buffer.c'), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)

    def test_uplink_uses_ap_clean_channel_at_unity_gain(self):
        source = (ROOT / 'src/server/lschat_server/wakeup/app_wakeup.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdint.h>
#define CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX 1
static unsigned frames;
static void lingxin_audio_capture_clean(const int16_t *pcm,uint32_t count) {
 assert(count==256);
 for(unsigned i=0;i<count;++i)assert(pcm[i]==(frames ? -20000 : 24000));
 ++frames;
}
void wakeup_out_stream_to_cloud(uint8_t *data,int len) {
''' + function_body(source, 'wakeup_out_stream_to_cloud') + r'''
}
int main(void) {
 short input[512][5];
 for(unsigned i=0;i<512;++i) {
  input[i][0]=111;input[i][1]=222;input[i][2]=333;
  input[i][3]=i<256 ? 24000 : -20000;input[i][4]=444;
 }
 wakeup_out_stream_to_cloud((uint8_t*)input,sizeof input);
 assert(frames==2);
 for(unsigned i=0;i<512;++i)assert(input[i][4]==input[i][3]);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            test, elf = Path(tmp) / 'route.c', Path(tmp) / 'route.elf'
            test.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(test), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)

    def test_prompt_hold_and_late_failsafe_preserve_capture_epoch(self):
        source = (ROOT / 'src/server/lingxin_server/adapter/lingxin_audio_adapter.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX 1
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LOGI(...) ((void)0)
static uint32_t s_record_uplink_gate_generation,s_record_uplink_gate_drop_count;
static bool s_record_uplink_gate_open=true;
static void *s_active_recorder;
static unsigned resets,arms;
static uint32_t service_offline_action_generation(void) {return 1;}
static bool service_offline_action_online_current(uint32_t g) {return true;}
static void lingxin_clean_capture_reset(void) {++resets;}
static void lingxin_clean_capture_arm(void) {++arms;}
static void lingxin_record_discard_buffered(void *p) {}
static void lingxin_trace_set(const char *p) {}
static void lingxin_server_diag_timing_mark(const char *p,int n) {}
'''
        for name, result, args in (
            ('lingxin_audio_uplink_gate_close', 'uint32_t', 'const char *reason'),
            ('lingxin_audio_uplink_gate_hold', 'uint32_t', 'const char *reason'),
            ('lingxin_audio_uplink_gate_open', 'void', 'uint32_t generation,const char *reason'),
        ):
            code += f'{result} {name}({args}) {{' + function_body(source, name) + '}\n'
        code += r'''
int main(void) {
 unsigned g=lingxin_audio_uplink_gate_close("accepted");assert(resets==1);
 assert(lingxin_audio_uplink_gate_hold("prompt")==g && resets==1);
 lingxin_audio_uplink_gate_open(g,"dac_drained");assert(s_record_uplink_gate_open && arms==1);
 lingxin_audio_uplink_gate_open(g,"old_failsafe");assert(arms==1);
 unsigned newer=lingxin_audio_uplink_gate_close("new_wake");assert(newer!=g && resets==2);
 lingxin_audio_uplink_gate_open(g,"late_old_complete");assert(!s_record_uplink_gate_open && arms==1);
 lingxin_audio_uplink_gate_open(newer,"drained");assert(arms==2);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            test, elf = Path(tmp) / 'gate.c', Path(tmp) / 'gate.elf'
            test.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(test), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)

    def test_normal_stop_drains_partial_tail_and_old_close_preserves_new_wake(self):
        directory = ROOT / 'src/server/lingxin_server/adapter'
        source = (directory / 'lingxin_clean_recorder.c').read_text(encoding='utf-8')
        prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "lingxin_capture_buffer.h"
#define CLEAN_FRAME_MAX 1280
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() test_exit_critical()
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
static lingxin_capture_buffer_t s_capture;
static uint32_t s_owner=4;
static bool s_capture_frozen;
typedef struct {
 uint32_t owner,generation,frame_size;
 volatile bool running,exited,draining,drained;
 bool opened,limit_stop_taken,tail_complete;
 uint32_t tail_bytes,tail_completed_at;
} clean_recorder_t;
#define CONFIG_LINGXIN_CLOUD_VAD_MAX_SENTENCE_SILENCE_MS 600
static unsigned xTaskGetTickCount(void) {return 0;}
static unsigned sent, attempts;
static unsigned failures;
static void voice_chat_fail_current_round(void) {failures++;}
static unsigned exits_after_first_frame;
static clean_recorder_t *drain_on_exit;
static void test_exit_critical(void) {
 if (drain_on_exit && sent==640 && ++exits_after_first_frame==2) {
  drain_on_exit->draining=true;drain_on_exit=NULL;s_capture_frozen=true;
 }
}
static unsigned char uploaded[1280];
static bool lingxin_clean_capture_current(uint32_t g) {return g==s_capture.generation;}
static bool module_record_is_server_ready(void) {return true;}
static bool lingxin_audio_uplink_gate_is_open(void *p) {return true;}
static void lisa_thread_mdelay(unsigned ms) {static unsigned polls; assert(++polls < 100);}
static bool lingxin_try_process_record_data(void *d,int n,uint32_t g) {
 assert(g==s_capture.generation);assert(n==640);
 if (++attempts==1) return false; /* First attempt sees backpressure. */
 assert(sent+n<=sizeof uploaded);memcpy(uploaded+sent,d,n);sent+=n;return true;
}
'''
        for name, result, args in (
            ('clean_sender', 'void', 'void *arg'),
            ('lingxin_clean_recorder_prepare_stop', 'void', 'void *recorder, bool drain'),
            ('clean_stop', 'bool', 'clean_recorder_t *ctx'),
        ):
            prefix += f'{result} {name}({args}) {{' + function_body(source, name) + '}\n'
        prefix += r'''
int main(void) {
 unsigned g=lingxin_capture_begin(&s_capture);
 unsigned char data[900];for(unsigned i=0;i<900;++i)data[i]=(unsigned char)i;
 assert(lingxin_capture_put(&s_capture,data,900));
 clean_recorder_t ctx={.owner=4,.generation=g,.frame_size=640,.opened=true,.running=true};
 lingxin_clean_recorder_prepare_stop(&ctx,true);assert(s_capture_frozen && ctx.draining);
 clean_sender(&ctx);assert(ctx.exited && ctx.drained && sent==1280 && attempts==3);
 assert(!memcmp(data,uploaded,900));for(unsigned i=900;i<1280;++i)assert(uploaded[i]==0);
 assert(clean_stop(&ctx));
 /* Same owner, different epoch: late close cannot clear newly captured prefix. */
 ctx.opened=true;ctx.draining=false;
 unsigned fresh=lingxin_capture_begin(&s_capture);
 assert(lingxin_capture_put(&s_capture,data,900));
 assert(clean_stop(&ctx));assert(s_capture.active && s_capture.length==900);
 /* Nor can the old sender consume that prefix, even while server-ready. */
 ctx.running=true;ctx.exited=false;ctx.draining=true;
 clean_sender(&ctx);assert(ctx.exited && s_capture.length==900 && sent==1280);
 assert(s_capture.generation==fresh);
 /* Stop arrives just after take rejected a partial frame in normal mode. */
 sent=attempts=0;ctx=(clean_recorder_t){.owner=4,.generation=fresh,.frame_size=640,.opened=true,.running=true};
 drain_on_exit=&ctx;
 clean_sender(&ctx);assert(ctx.exited && ctx.drained && sent==1280);
 assert(!memcmp(data,uploaded,900));for(unsigned i=900;i<1280;++i)assert(uploaded[i]==0);
 /* A capture overflow must abort its cloud round, not hang waiting for VAD. */
 fresh=lingxin_capture_begin(&s_capture);
 ctx=(clean_recorder_t){.owner=4,.generation=fresh,.frame_size=640,.opened=true,.running=true};
 lingxin_capture_begin(&s_capture);s_capture.overflow=true;
 clean_sender(&ctx);assert(ctx.exited && failures==1);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / 'lifecycle.c'
            test.write_text(prefix, encoding='utf-8')
            elf = Path(tmp) / 'lifecycle.elf'
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(directory),
                            str(test), str(directory / 'lingxin_capture_buffer.c'), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)

    def test_engine_backpressure_rejects_full_queue_and_stale_generation(self):
        source = (ROOT / 'third_party/AI2T_LingXinEngine/src/voice_chat/recorder_manager.c').read_text(encoding='utf-8')
        body = function_body(source, 'lingxin_try_process_record_data')
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / 'engine.c'
            test.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX 1
#define LINGXIN_RECORD_MAX_BYTES 1280
static int send_thread_mutex=1, send_uni_size=640, send_thread_running=1;
static int send_thread_stop_flag, send_flag=1;
static void *send_r_sem=(void*)1, *send_cbuf=(void*)1;
static bool full;
static unsigned puts, posts;
static unsigned char item[644];
static void lingxin_mutex_lock(int x) {}
static void lingxin_mutex_unlock(int x) {}
static bool lingxin_clean_capture_current(uint32_t g) {return g==9;}
static bool lingxin_cbuffer_full(void *b) {return full;}
static void lingxin_cbuffer_put(void *b,void *d) {memcpy(item,d,sizeof item);++puts;}
static void lingxin_semaphore_set_value(void *s,int n) {}
static void lingxin_semaphore_post(void *s) {++posts;}
bool lingxin_try_process_record_data(void *data,int len,uint32_t generation) {
''' + body + r'''
}
int main(void) {
 unsigned char data[640];memset(data,0x42,sizeof data);
 full=true;assert(!lingxin_try_process_record_data(data,640,9));assert(!puts);
 full=false;assert(!lingxin_try_process_record_data(data,640,8));assert(!puts);
 assert(!lingxin_try_process_record_data(data,512,9));assert(!puts);
 send_flag=0;assert(!lingxin_try_process_record_data(data,640,9));assert(!puts);
 send_flag=1;assert(lingxin_try_process_record_data(data,640,9));assert(puts==1 && posts==1);
 uint32_t g;memcpy(&g,item+640,4);assert(g==9 && item[0]==0x42);
 send_thread_stop_flag=1;assert(!lingxin_try_process_record_data(data,640,9));assert(puts==1);
 return 0;
}
''', encoding='utf-8')
            elf = Path(tmp) / 'engine.elf'
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(test), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)

    def test_prompt_boundary_delayed_ready_and_session_isolation(self):
        directory = ROOT / 'src/server/lingxin_server/adapter'
        source = directory / 'lingxin_capture_buffer.c'
        self.assertTrue(source.exists(), 'Missing bounded clean-audio capture buffer')
        with tempfile.TemporaryDirectory() as tmp:
            test = Path(tmp) / 'capture.c'
            test.write_text(r'''
#include <assert.h>
#include <string.h>
#include "lingxin_capture_buffer.h"
static lingxin_capture_buffer_t b;
int main(void) {
    unsigned char in[512], out[640];
    memset(in, 0x11, sizeof in);
    assert(!lingxin_capture_put(&b, in, sizeof in)); /* before wake */
    unsigned old = lingxin_capture_begin(&b);
    /* First audio immediately after the prompt must survive 1 s of setup. */
    for (int i=0; i<63; ++i) {
        in[0]=(unsigned char)i;
        assert(lingxin_capture_put(&b, in, sizeof in));
    }
    assert(lingxin_capture_take(&b, out, sizeof out));
    assert(out[0]==0 && out[512]==1);
    /* Late close from an older recorder cannot erase the new wake. */
    unsigned current=lingxin_capture_begin(&b);
    memset(in, 0x33, sizeof in);
    assert(lingxin_capture_put(&b, in, sizeof in));
    lingxin_capture_end(&b, old);
    assert(b.active && b.generation==current);
    assert(lingxin_capture_take(&b, out, sizeof in));
    assert(out[0]==0x33);
    assert(!lingxin_capture_take(&b, out, 1));
    /* Abort discards old partial frames. */
    assert(lingxin_capture_put(&b, in, 123));
    lingxin_capture_end(&b, current);
    assert(!b.active && b.length==0);
    lingxin_capture_begin(&b);
    /* Wraparound preserves order. */
    for (unsigned i=0; i<LINGXIN_CAPTURE_CAPACITY*2; ++i) {
        unsigned char v=(unsigned char)i, got=0;
        assert(lingxin_capture_put(&b, &v, 1));
        assert(lingxin_capture_take(&b, &got, 1));
        assert(v==got);
    }
    /* Overflow fails the whole capture; never upload a spliced sentence. */
    for (unsigned i=0; i<b.limit/512; ++i)
        assert(lingxin_capture_put(&b, in, sizeof in));
    old=b.generation;
    assert(!lingxin_capture_put(&b, in, sizeof in));
    assert(b.overflow && !b.active && b.length==0 && b.generation!=old);
    assert(!lingxin_capture_take(&b, out, sizeof out));
    lingxin_capture_begin(&b);
    assert(!b.overflow && b.active);
    return 0;
}
''', encoding='utf-8')
            elf = Path(tmp) / 'capture.elf'
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(directory), str(test), str(source), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)


if __name__ == '__main__':
    unittest.main()
