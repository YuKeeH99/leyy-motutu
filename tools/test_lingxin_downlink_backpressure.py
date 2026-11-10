"""Exercise production TTS producer throttling with a paced, bounded consumer."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_offline_action import c_function
from test_task_continue_guard import ROOT, CC, RUN


class DownlinkBackpressureTest(unittest.TestCase):
    def test_long_answer_cancellation_and_stalled_consumer(self):
        source = (ROOT / 'src/server/lingxin_server/adapter/lingxin_audio_adapter.c').read_text(encoding='utf-8')
        constants = '\n'.join(re.findall(
            r'^#define LINGXIN_STREAM_(?:QUEUE_MAX_BYTES|THROTTLE_[A-Z_]+)\s+.+$', source, re.M))
        helper = (c_function(source, 'lingxin_stream_throttle_producer')
                  if 'static void lingxin_stream_throttle_producer(' in source else '')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define lingxin_server_diag_timing_mark(...) ((void)0)
''' + constants + r'''
static uint32_t s_playback_generation=1,s_stream_owner_generation=7;
static uint32_t s_stream_queue_bytes,s_stream_queue_peak_bytes;
static uint32_t s_stream_chunk_count,s_stream_bytes_total;
static bool s_stream_response_ready=true,s_stream_audio_ended,s_stream_write_failed;
static bool s_stream_writer_running=true;
static int s_tts_suppress_policy;
static bool lifecycle_locked,owner_current=true,critical_locked;
static uint32_t now_ms,consumer_bps=16384,consumer_fraction,total_accepted;
static unsigned failures,sleeps;
static int cancel_after_ms,cancel_mode;
static void critical_enter(void) {assert(!critical_locked);critical_locked=true;}
static void critical_leave(void) {assert(critical_locked);critical_locked=false;}
#define taskENTER_CRITICAL() critical_enter()
#define taskEXIT_CRITICAL() critical_leave()
static bool service_offline_action_online_current(uint32_t owner) {return owner==7 && owner_current;}
static uint32_t lingxin_playback_now_ms(void) {return now_ms;}
static void advance(unsigned ms) {
    now_ms+=ms;
    uint32_t available=consumer_bps*ms+consumer_fraction;
    uint32_t consumed=available/1000;
    consumer_fraction=available%1000;
    if(consumed>s_stream_queue_bytes) consumed=s_stream_queue_bytes;
    s_stream_queue_bytes-=consumed;
    if(cancel_after_ms>0) {
        cancel_after_ms-=(int)ms;
        if(cancel_after_ms<=0) {
            if(cancel_mode==1) ++s_playback_generation;
            if(cancel_mode==2) owner_current=false;
            if(cancel_mode==3) s_stream_write_failed=true;
            if(cancel_mode==4) s_stream_response_ready=false;
        }
    }
}
static void lisa_thread_mdelay(unsigned ms) {
    /* Waiting with either lock held blocks cancellation / the consumer. */
    assert(!lifecycle_locked && !critical_locked);
    assert(ms>0 && ms<=10);
    ++sleeps;advance(ms);
}
static bool lingxin_playback_lifecycle_lock(void) {assert(!lifecycle_locked);lifecycle_locked=true;return true;}
static void lingxin_playback_lifecycle_unlock(void) {assert(lifecycle_locked);lifecycle_locked=false;}
static bool lingxin_tts_suppress_should_drop_audio(void *p) {return false;}
static bool lingxin_stream_start_playback(uint32_t generation) {return generation==s_playback_generation;}
static bool lingxin_stream_enqueue_copy(void *p,uint32_t n,bool end,uint32_t generation) {
    assert(lifecycle_locked && generation==s_playback_generation);
    if(n>LINGXIN_STREAM_QUEUE_MAX_BYTES-s_stream_queue_bytes) return false;
    s_stream_queue_bytes+=n;total_accepted+=n;
    if(s_stream_queue_bytes>s_stream_queue_peak_bytes) s_stream_queue_peak_bytes=s_stream_queue_bytes;
    return true;
}
static void lingxin_stream_fail(uint32_t generation,const char *reason) {
    if(generation!=s_playback_generation) return;
    ++failures;s_stream_write_failed=true;
}
''' + helper + '\n' + c_function(source, 'module_bufferPlay_data') + r'''
static void reset(void) {
    s_playback_generation=1;s_stream_owner_generation=7;
    s_stream_queue_bytes=s_stream_queue_peak_bytes=s_stream_chunk_count=s_stream_bytes_total=0;
    s_stream_response_ready=s_stream_writer_running=owner_current=true;
    s_stream_audio_ended=s_stream_write_failed=lifecycle_locked=critical_locked=false;
    now_ms=consumer_fraction=total_accepted=failures=sleeps=0;
    consumer_bps=16384;cancel_after_ms=cancel_mode=0;
}
int main(void) {
    char dummy=0;
    reset();
    /* Server can immediately supply ~3.3 KiB every 20ms. Audible playback
     * consumes 16KiB/s; producer waiting must propagate back to socket reads. */
    for(unsigned i=0;i<1400;i++) {
        unsigned before=now_ms;
        module_bufferPlay_data(&dummy,3379);
        assert(!failures && !s_stream_write_failed);
        assert((uint32_t)(now_ms-before)<=500);
        assert(s_stream_queue_peak_bytes<=1024U*1024U+3379U);
        advance(20);
    }
    assert(total_accepted>3U*1024U*1024U && now_ms>180000 && sleeps>0);
    assert(s_stream_queue_bytes<=LINGXIN_STREAM_QUEUE_MAX_BYTES);
    /* A blocked or terminated decoder must not hold the WS callback forever. */
    reset();consumer_bps=0;s_stream_queue_bytes=1024U*1024U;
    module_bufferPlay_data(&dummy,3379);
    assert(now_ms==500 && !failures && !lifecycle_locked);
    /* Wake generation, local owner, stream failure and readiness cancellation. */
    for(int mode=1;mode<=4;mode++) {
        reset();consumer_bps=0;s_stream_queue_bytes=1024U*1024U;
        cancel_after_ms=1;cancel_mode=mode;
        module_bufferPlay_data(&dummy,3379);
        assert(now_ms<=10 && !failures && !lifecycle_locked);
    }
    reset();consumer_bps=0;s_stream_queue_bytes=1024U*1024U;
    now_ms=UINT32_MAX-4;
    uint32_t before=now_ms;
    module_bufferPlay_data(&dummy,3379);
    assert((uint32_t)(now_ms-before)==500 && !failures);
    /* Oversize messages remain an explicit bounded failure, never an
     * unlimited allocation or an unbounded wait to disguise the limit. */
    reset();module_bufferPlay_data(&dummy,LINGXIN_STREAM_QUEUE_MAX_BYTES+1);
    assert(failures==1 && !now_ms && !s_stream_queue_bytes);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cfile, elf = Path(directory)/'downlink.c', Path(directory)/'downlink.elf'
            cfile.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(cfile), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
