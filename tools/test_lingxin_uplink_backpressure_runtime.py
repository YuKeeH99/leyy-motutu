"""Fault-inject the production binary sender; no device or audio capture."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_task_continue_guard import ROOT, CC, RUN
from test_offline_action import c_function


class UplinkBackpressureRuntimeTest(unittest.TestCase):
    def test_sdk_full_queue_is_retryable_and_written_counters_wait_for_socket(self):
        directory = ROOT / 'arcs-sdk/components/lisa_websocket'
        source = (directory / 'lisa_websocket.c').read_text(encoding='utf-8')
        source = source.replace('lisa_ws_err_e lisa_ws_send_binary(', 'int lisa_ws_send_binary(')
        header = (directory / 'lisa_websocket.h').read_text(encoding='utf-8')
        start = header.index('typedef struct {\n    uint32_t enqueued_frames')
        stats = header[start:header.index('} lisa_ws_transport_stats_t;', start)+len('} lisa_ws_transport_stats_t;')]
        functions = ''.join(c_function(source, n)+'\n' for n in (
            'lisa_ws_note_max', 'lisa_websocket_msg_send_to_thread', 'lisa_ws_send_binary',
            'lisa_websocket_msg_normal_send', 'lisa_websocket_pending_write_proc'))
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
/* The ISA simulator does not model AMO accumulation reliably. This fixture
 * is single-threaded; exercise counter decisions with ordinary arithmetic. */
#define __atomic_add_fetch(p,n,order) (*(p) += (n))
#include "lisa_websocket_pending_policy.h"
#define LISA_WS_OK 0
#define LISA_WS_COMMON_ERR 1
#define LISA_WS_DISCONNECT 2
#define LISA_WS_WOULD_BLOCK 3
#define LISA_WS_TEXT 0
#define LISA_WS_BIN 1
#define LISA_OK 0
#define WS_QUEUE_COUNT 150
#define WS_SEND_RETRY_DELAY 10000
#define LISA_NLOGI(...) ((void)0)
#define LISA_NLOGW(...) ((void)0)
#define LISA_NLOGE(...) ((void)0)
typedef int lisa_ws_err_e, lisa_err_t;
''' + stats + r'''
typedef struct { int pending, result, flushed; bool ok; } noPollConn;
typedef struct { int m_type; char *m_msg; int m_size; unsigned flash_index; } lisa_ws_msg_t;
typedef struct { bool ws_conn, ws_stop; void *tx_msg_queue; lisa_ws_transport_stats_t transport;
 bool pending_payload_frame; lisa_ws_pending_tracker_t pending_write; unsigned pong_max_time_ms; } lisa_ws_t;
static unsigned queue_count, clock_ms; static bool allocation_failed, push_failed; static void *queued;
static void *lisa_mem_alloc(size_t n) {return allocation_failed?NULL:malloc(n);}
#define lisa_mem_free free
static int lisa_queue_waiting(void *q) {return queue_count;}
static int lisa_queue_push(void *q,const void *p,size_t n,unsigned wait) {
 if(push_failed) return -1; ++queue_count; queued=((lisa_ws_msg_t*)p)->m_msg; return 0;
}
static unsigned lisa_os_get_tick_ms(void) {return clock_ms;}
static int nopoll_conn_pending_write_bytes(noPollConn *c) {return c->pending;}
static unsigned int nopoll_conn_pending_write_sequence(noPollConn *c) {(void)c;return 0U;}
static int nopoll_conn_send_binary(noPollConn *c,const char *p,int n) {return c->result;}
static int nopoll_conn_send_text(noPollConn *c,const char *p,int n) {return c->result;}
static int nopoll_conn_complete_pending_write(noPollConn *c) {
 if(c->flushed>0)c->pending=0; return c->flushed;
}
static bool nopoll_conn_is_ok(noPollConn *c) {return c->ok;}
static void nopoll_sleep(unsigned us) {clock_ms += us/1000;}
''' + functions + r'''
int main(void) {
 lisa_ws_t ws={.ws_conn=true,.pong_max_time_ms=3000};
 queue_count=145;
 assert(lisa_ws_send_binary(&ws,"pcm",3)==LISA_WS_WOULD_BLOCK);
 assert(ws.transport.enqueued_frames==0 && ws.transport.queue_full==1);
 queue_count=0;allocation_failed=true;
 assert(lisa_ws_send_binary(&ws,"pcm",3)==LISA_WS_COMMON_ERR && ws.transport.enqueued_bytes==0);
 allocation_failed=false;push_failed=true;
 assert(lisa_ws_send_binary(&ws,"pcm",3)==LISA_WS_WOULD_BLOCK && ws.transport.enqueued_frames==0);
 push_failed=false;
 assert(lisa_ws_send_binary(&ws,"pcm",3)==LISA_WS_OK);
 assert(ws.transport.enqueued_frames==1 && ws.transport.enqueued_bytes==3 && ws.transport.queue_peak==1);
 assert(ws.transport.written_bytes==0); /* A queue acknowledgement is not a write. */
 free(queued);ws.ws_stop=true;
 assert(lisa_ws_send_binary(&ws,"pcm",3)==LISA_WS_DISCONNECT);
 lisa_ws_msg_t msg={.m_type=LISA_WS_BIN,.m_msg="pcm",.m_size=3};
 noPollConn c={.pending=11,.result=-2,.flushed=-26752,.ok=true};
 assert(lisa_websocket_msg_normal_send(&ws,&c,&msg)==0 && ws.transport.written_bytes==0);
 ws.pending_payload_frame=true;
 assert(lisa_websocket_pending_write_proc(&ws,&c)==1 && ws.transport.written_bytes==0);
 c.flushed=3;
 assert(lisa_websocket_pending_write_proc(&ws,&c)==0);
 assert(ws.transport.written_bytes==3 && ws.transport.completed_frames==1 && ws.transport.pending_bytes==0);
 assert(lisa_websocket_pending_write_proc(&ws,&c)==0 && ws.transport.completed_frames==1);
 c.result=3;
 assert(lisa_websocket_msg_normal_send(&ws,&c,&msg)==0 && ws.transport.written_bytes==6);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'sdk.c', Path(temp)/'sdk.elf'
            cfile.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(directory), str(cfile),
                            str(directory/'lisa_websocket_pending_policy.c'), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)

    def test_protocol_guard_rejects_closed_boundary_and_old_epoch(self):
        source = (ROOT/'third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c').read_text(encoding='utf-8')
        functions = c_function(source, 'voice_chat_audio_send_current') + c_function(source, 'voiceChatSendAudio')
        code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
#define CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX 1
#define WEBSOCKET_SEND_CANCELLED (-2)
#define lingxin_log_ut_with_args(...) ((void)0)
#define lingxin_server_diag_timing_mark(...) ((void)0)
typedef struct { void *websocket; } VoiceChatHandler;
static VoiceChatHandler handler;
static bool isTaskStarted=true,s_waiting_task_terminated,s_cloud_vad_upload_allowed=true,isAudioSending;
static uint32_t s_recovery_epoch=5,s_diag_binary_audio_drop_before_task_started,s_diag_start_task_seq;
static uint32_t s_diag_binary_audio_send_count,s_diag_binary_audio_send_bytes;
static long s_diag_start_task_sent_at_s;
static int leases,failed,mode;
static VoiceChatHandler *voice_chat_handler_acquire(void) {++leases;return &handler;}
static void voice_chat_handler_release(VoiceChatHandler *p) {--leases;}
static void on_voice_chat_init_fail_for_epoch(uint32_t epoch) {assert(epoch==5);++failed;}
static const char *getReqId(VoiceChatHandler *p) {return "";}
static long lingxin_get_timestamp_s(void) {return 0;}
static int hook_websocket_send_binary_guarded(void *w,void *data,int n,bool(*current)(void*),void *arg) {
 assert(current(arg));
 if(mode==1) {++s_recovery_epoch;assert(!current(arg));return WEBSOCKET_SEND_CANCELLED;}
 if(mode==2) {s_cloud_vad_upload_allowed=false;assert(!current(arg));return WEBSOCKET_SEND_CANCELLED;}
 if(mode==3)return 0;
 return n;
}
''' + functions + r'''
int main(void) {
 voiceChatSendAudio("pcm",3);assert(s_diag_binary_audio_send_count==1 && leases==0);
 mode=1;voiceChatSendAudio("pcm",3);assert(failed==0 && s_diag_binary_audio_send_count==1 && leases==0);
 s_recovery_epoch=5;mode=2;voiceChatSendAudio("pcm",3);assert(failed==0 && leases==0);
 s_cloud_vad_upload_allowed=true;mode=3;voiceChatSendAudio("pcm",3);assert(failed==1 && leases==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'epoch.c', Path(temp)/'epoch.elf'
            cfile.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(cfile), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)

    def test_transient_queue_full_preserves_pcm_and_cancels_before_retry(self):
        source = (ROOT / 'src/server/lingxin_server/adapter/lingxin_websocket_adapter.c').read_text(encoding='utf-8')
        functions = ''
        for name in ('lingxin_ws_note_wait', 'websocketSendBinaryGuarded', 'websocketSendBinary'):
            if name + '(' in source:
                functions += c_function(source, name) + '\n'
        if 'int websocketSendBinaryGuarded(' not in functions:
            functions += '''
int websocketSendBinaryGuarded(WebsocketClient *c, const char *p, size_t n,
 bool (*current)(void *), void *arg) { return websocketSendBinary(c,p,n); }
'''
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define LINGXIN_WS_SEND_RETRY_COUNT 4U
#define LINGXIN_WS_SEND_RETRY_DELAY_MS 20U
#define LINGXIN_WS_SEND_WAIT_MS 1500U
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define WEBSOCKET_SEND_CANCELLED (-2)
#define LISA_WS_OK 0
#define LISA_WS_COMMON_ERR 1
#define LISA_WS_DISCONNECT 2
#define LISA_WS_WOULD_BLOCK 3
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
typedef int lisa_ws_err_e;
typedef struct { bool closing; } WebsocketClient;
typedef struct { void *ws; bool connected; } lingxin_ws_ctx_t;
static lingxin_ws_ctx_t ctx = { (void *)1, true };
static unsigned now, attempts, busy_until, disconnect_at, cancel_at, wake_at, accepted, leases;
static int forced_error;
static const char *expected;
static uint32_t s_ws_binary_send_count, s_ws_binary_send_bytes, s_ws_binary_drop_count;
static uint32_t s_ws_wait_count, s_ws_wait_total_ms, s_ws_wait_max_ms, s_ws_wait_timeout, s_ws_wait_cancel;
static lingxin_ws_ctx_t *lingxin_ws_acquire(WebsocketClient *c) { ++leases; return &ctx; }
static void lingxin_ws_release(WebsocketClient *c) { --leases; }
static unsigned lisa_os_get_tick_ms(void) { return now; }
static unsigned service_offline_action_generation(void) {return 7;}
static bool service_offline_action_online_current(unsigned generation) {return generation==7 && (!wake_at || now<wake_at);}
static void lisa_thread_mdelay(unsigned ms) { now += ms; if(disconnect_at && now>=disconnect_at) ctx.connected=false; }
static void sys_network_diag_mark(const char *a,const char *b,int c,const char *d) {}
static lisa_ws_err_e lisa_ws_send_binary(void *ws,const char *p,unsigned n) {
 ++attempts; assert(p==expected && n==4 && !memcmp(p,"PCM!",4));
 if(!ctx.connected) return LISA_WS_DISCONNECT;
 if(forced_error) return forced_error;
 if(now<busy_until) return LISA_WS_WOULD_BLOCK;
 ++accepted; return LISA_WS_OK;
}
static bool current(void *p) { return !cancel_at || now<cancel_at; }
static void reset(void) {now=attempts=busy_until=disconnect_at=cancel_at=wake_at=accepted=leases=0;forced_error=0;ctx.connected=true;}
''' + functions + r'''
int main(void) {
 WebsocketClient c={0}; expected="PCM!";
 reset();busy_until=240;
 assert(websocketSendBinary(&c,expected,4)==4); /* Previously aborts at 80 ms. */
 assert(now==240 && accepted==1 && leases==0);
 reset();busy_until=10000;
 assert(websocketSendBinary(&c,expected,4)==0);
 assert(now==LINGXIN_WS_SEND_WAIT_MS && accepted==0 && leases==0);
 reset();busy_until=10000;cancel_at=60;
 assert(websocketSendBinaryGuarded(&c,expected,4,current,0)==WEBSOCKET_SEND_CANCELLED);
 assert(now==60 && attempts==3 && accepted==0 && leases==0);
 reset();busy_until=10000;wake_at=80;
 assert(websocketSendBinaryGuarded(&c,expected,4,current,0)==WEBSOCKET_SEND_CANCELLED);
 assert(now==80 && attempts==4 && accepted==0 && leases==0);
 reset();busy_until=10000;disconnect_at=40;
 assert(websocketSendBinary(&c,expected,4)==0 && now==40 && accepted==0 && leases==0);
 reset();forced_error=LISA_WS_COMMON_ERR;
 assert(websocketSendBinary(&c,expected,4)==0 && now==0 && attempts==1 && leases==0);
 reset();now=UINT32_MAX-99;busy_until=UINT32_MAX;forced_error=LISA_WS_WOULD_BLOCK;
 assert(websocketSendBinary(&c,expected,4)==0 && (uint32_t)(now-(UINT32_MAX-99))==1500);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'uplink.c', Path(temp)/'uplink.elf'
            cfile.write_text(harness, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(cfile), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
