"""Execute real SDK lifecycle functions at deterministic cancellation points."""
import re
import unittest
from tools import test_offline_action as support


def function(source, name):
    match = re.search(r"^.*\b" + name + r"\([^;]+?\)\s*\{", source, re.M)
    assert match, name
    return source[match.start():source.index("\n}", match.end()) + 2]


ATOMIC_STUBS = r'''
/* Deterministic single-threaded scheduling; the simulator lacks AMOs. */
#define __atomic_load_n(p,o) (*(p))
#define __atomic_store_n(p,v,o) ((void)(*(p)=(v)))
#define __atomic_add_fetch(p,v,o) (*(p)+=(v))
#define __atomic_sub_fetch(p,v,o) (*(p)-=(v))
#define __atomic_exchange_n(p,v,o) ({__typeof__(*(p)) old=*(p);*(p)=(v);old;})
#define __atomic_compare_exchange_n(p,e,v,w,s,f) ({bool ok=*(p)==*(e);if(ok)*(p)=(v);else *(e)=*(p);ok;})
'''

SDK_PRE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define LISA_WS_OK 0
#define LISA_WS_COMMON_ERR 1
#define LISA_WS_BUSY 4
#define LISA_OK 0
#define LISA_NLOGI(...) ((void)0)
#define LISA_NLOGE(...) ((void)0)
typedef int lisa_ws_err_e;
typedef void *lisa_threadhandle_t;
typedef struct {lisa_threadhandle_t handle;} lisa_thread_t;
typedef struct {char*m_msg;} lisa_ws_msg_t;
typedef struct lisa_ws {
 bool ws_conn,ws_stop,worker_busy,cleanup_requested;
 int32_t connect_cancel;
 lisa_thread_t *thread;
 void*tx_msg_queue,*exiting_sem,*exited_sem,*pong_sem,*connect_sem;
} lisa_ws_t;
static unsigned now_ms,deleted,freed,sem_deleted,ack_at;
static lisa_ws_t *current;
static lisa_thread_t worker_thread={(void*)1};
static void *current_task;
static void*xTaskGetCurrentTaskHandle(void){return current_task;}
static void lisa_semaphore_reset(void*s){}
static void lisa_semaphore_give(void*s){}
static int lisa_semaphore_take(void*s,unsigned n){
 now_ms+=n;
 if(ack_at && now_ms>=ack_at){current->worker_busy=false;return 0;}
 return -1;
}
static unsigned lisa_os_get_tick_ms(void){return now_ms;}
static void lisa_thread_mdelay(unsigned n){now_ms+=n;}
static void lisa_semaphore_delete(void*s){sem_deleted++;}
static void lisa_thread_delete(void*t){assert(!current->worker_busy);deleted++;}
static int lisa_queue_pop(void*q,void*m,unsigned n,unsigned t){return -1;}
static void lisa_queue_delete(void*q){}
static void lisa_websocket_rx_msg_reset(lisa_ws_t*p){}
static void lisa_mem_free(void*p){assert(!current->worker_busy);freed++;}
'''


class WebsocketCleanupLifecycleTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def sdk_functions(self):
        source = (support.ROOT / "arcs-sdk/components/lisa_websocket/lisa_websocket.c").read_text(encoding="utf-8")
        return "\n".join(function(source, name) for name in (
            "lisa_ws_connect", "lisa_ws_disconnect", "lisa_ws_cleanup"))

    def test_connecting_cleanup_timeout_retains_owner_then_retry_reclaims(self):
        self.run_c(SDK_PRE + ATOMIC_STUBS + self.sdk_functions() + r'''
int main(void){
 lisa_ws_t ws={.worker_busy=true,.thread=&worker_thread,.connect_sem=(void*)2,
   .exiting_sem=(void*)3,.exited_sem=(void*)4};current=&ws;
 assert(lisa_ws_cleanup(&ws)==LISA_WS_BUSY);
 assert(ws.worker_busy && ws.ws_stop && ws.connect_cancel && ws.thread);
 assert(!deleted && !freed && !sem_deleted && now_ms<=10000);
 /* DNS naturally completes later; its worker retires resources then ACKs. */
 ack_at=now_ms+20;
 assert(lisa_ws_cleanup(&ws)==LISA_WS_OK);
 assert(deleted==1 && freed==1 && sem_deleted==3);
 return 0;
}
''')

    def test_connected_disconnect_waits_for_ack_and_reconnect_reserves_worker(self):
        self.run_c(SDK_PRE + ATOMIC_STUBS + self.sdk_functions() + r'''
int main(void){
 lisa_ws_t ws={.thread=&worker_thread,.connect_sem=(void*)2,
   .exiting_sem=(void*)3,.exited_sem=(void*)4};current=&ws;
 assert(lisa_ws_connect(&ws)==LISA_WS_OK && ws.worker_busy);
 assert(lisa_ws_connect(&ws)!=LISA_WS_OK); /* no duplicate pending handshake */
 ws.ws_conn=true;ack_at=20;
 assert(lisa_ws_disconnect(&ws)==LISA_WS_OK && !ws.worker_busy && now_ms>=20);
 assert(!deleted && !freed);
 ws.ws_conn=false;
 assert(lisa_ws_connect(&ws)==LISA_WS_OK && !ws.ws_stop && !ws.connect_cancel);
 ack_at=now_ms+20;
 assert(lisa_ws_cleanup(&ws)==LISA_WS_OK && deleted==1 && freed==1);
 return 0;
}
''')

    def test_callback_close_never_waits_for_its_own_worker_ack(self):
        self.run_c(SDK_PRE + ATOMIC_STUBS + self.sdk_functions() + r'''
int main(void){
 lisa_ws_t ws={.worker_busy=true,.thread=&worker_thread,.connect_sem=(void*)2,
   .exiting_sem=(void*)3,.exited_sem=(void*)4};current=&ws;current_task=(void*)1;
 assert(lisa_ws_cleanup(&ws)==LISA_WS_BUSY);
 assert(!now_ms && !deleted && !freed && ws.connect_cancel && ws.ws_stop);
 return 0;
}
''')

    def test_adapter_close_busy_preserves_callback_owner_and_allows_retry(self):
        source = (support.ROOT / "src/server/lingxin_server/adapter/lingxin_websocket_adapter.c").read_text(encoding="utf-8")
        header = (support.ROOT / "third_party/AI2T_LingXinEngine/inc/system_adapter/lingxin_websocket.h").read_text(encoding="utf-8")
        context = re.search(r"typedef struct \{.*?\} lingxin_ws_ctx_t;", source, re.S).group()
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LOGW(...) ((void)0)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LISA_WS_OK 0
#define LISA_WS_BUSY 4
#define LINGXIN_WS_CLOSE_DRAIN_MS 2500U
typedef int lisa_ws_t,lisa_ws_err_e;
''' + ATOMIC_STUBS + header + context + r'''
static WebsocketTransportStats s_last_transport;
static unsigned frees,now,cleanup_calls;
static int cleanup_result=LISA_WS_BUSY;
static unsigned lisa_os_get_tick_ms(void){return now;}
static void lisa_thread_mdelay(unsigned n){now+=n;}
static void lisa_mem_free(void*p){assert(p);frees++;}
static void sys_network_diag_mark(const char*a,const char*b,int n,const char*d){}
static void lingxin_ws_snapshot(lingxin_ws_ctx_t*c,WebsocketTransportStats*s){s->connected=c->connected;}
static int lisa_ws_cleanup(lisa_ws_t*w){assert(w);cleanup_calls++;return cleanup_result;}
''' + function(source, "closeWebsocket") + function(source, "destroyWebsocketClient") + r'''
int main(void){
 lisa_ws_t ws=0;WebsocketConfig cfg={0};
 lingxin_ws_ctx_t ctx={.ws=&ws,.config=&cfg};
 WebsocketClient client={.clientHandler=&ctx,.config=&cfg};
 assert(!closeWebsocket(&client));
 assert(client.closing && client.clientHandler==&ctx && ctx.ws==&ws && ctx.config==&cfg);
 assert(!frees && cleanup_calls==1);
 destroyWebsocketClient(&client);assert(!frees);
 cleanup_result=LISA_WS_OK;
 assert(closeWebsocket(&client));assert(!client.clientHandler && frees==1);
 assert(closeWebsocket(&client));assert(frees==1 && cleanup_calls==2);
 destroyWebsocketClient(&client);assert(frees==2);
 /* A stalled API owner is retained rather than an unbounded caller wait. */
 client=(WebsocketClient){.clientHandler=&ctx,.active_operations=1};
 assert(!closeWebsocket(&client) && now==2500 && client.clientHandler==&ctx);
 assert(cleanup_calls==2 && frees==2);
 return 0;
}
''')

    def test_async_close_retries_once_per_client_and_creation_failure_is_retryable(self):
        source = (support.ROOT / "third_party/AI2T_LingXinEngine/src/websocket/lingxin_hook_websocket.c").read_text(encoding="utf-8")
        header = (support.ROOT / "third_party/AI2T_LingXinEngine/inc/system_adapter/lingxin_websocket.h").read_text(encoding="utf-8")
        self.assertIn("bool hook_websocket_close_async(", source)
        job = re.search(r"typedef struct \{.*?\} websocket_close_job_t;", source, re.S).group()
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
''' + ATOMIC_STUBS + header + job + r'''
typedef int lingxin_tid_t;
typedef struct {const char*name;int priority,stack_size;} lingxin_thread_param_t;
static bool fail_alloc,fail_thread;
static unsigned allocs,frees,threads,closes,completed,sleeps;
static void*(*entry)(void*);static void*argument;
static void*lingxin_calloc(size_t n,size_t s){if(fail_alloc)return NULL;allocs++;return calloc(n,s);}
static void lingxin_free(void*p){frees++;free(p);}
static int lingxin_thread_create(lingxin_tid_t*t,const lingxin_thread_param_t*p,void*(*fn)(void*),void*a){
 threads++;if(fail_thread)return -1;entry=fn;argument=a;return 0;
}
static void lingxin_thread_sleep(unsigned n){assert(n==20);sleeps++;}
bool closeWebsocket(WebsocketClient*c){if(!c)return true;assert(c->closing);return ++closes>=3;}
static void complete(void*p){assert(p==(void*)7);completed++;}
''' + function(source, "websocket_close_worker") + function(source, "hook_websocket_close_async") + r'''
int main(void){
 WebsocketClient client={0};fail_alloc=true;
 assert(!hook_websocket_close_async(&client,complete,(void*)7));
 assert(!client.close_retry_pending && !completed);
 fail_alloc=false;fail_thread=true;
 assert(!hook_websocket_close_async(&client,complete,(void*)7));
 assert(!client.close_retry_pending && client.closing && allocs==frees && !completed);
 fail_thread=false;
 assert(hook_websocket_close_async(&client,complete,(void*)7));
 assert(client.closing && client.close_retry_pending && !completed);
 assert(hook_websocket_close_async(&client,complete,(void*)7) && threads==2);
 entry(argument);
 assert(closes==3 && sleeps==2 && completed==1 && allocs==frees);
 assert(hook_websocket_close_async(&client,complete,(void*)7) && threads==2 && completed==1);
 assert(hook_websocket_close_async(NULL,complete,(void*)7));entry(argument);
 assert(completed==2 && threads==3 && allocs==frees);
 return 0;
}
''')

    def test_real_worker_acks_every_handshake_cancel_and_normal_connected_exit(self):
        source = (support.ROOT / "arcs-sdk/components/lisa_websocket/lisa_websocket.c").read_text(encoding="utf-8")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#define CONFIG_NOPOLL_DELIVER_PONG_FRAME_ENABLED 0
#define LISA_WS_OK 0
#define LISA_WS_COMMON_ERR 1
#define LISA_WS_BUSY 4
#define LISA_WS_ON_CONNECTING 3
#define LISA_OK 0
#define nopoll_false 0
#define LISA_NLOGI(...) ((void)0)
#define LISA_NLOGE(...) ((void)0)
typedef int lisa_ws_err_e;
typedef void *lisa_threadhandle_t;
typedef struct {lisa_threadhandle_t handle;} lisa_thread_t;
typedef struct {char*m_msg;} lisa_ws_msg_t;
typedef struct {int what;void*user;} lisa_ws_event_t;
typedef struct {int unused;} noPollCtx,noPollConn,noPollConnOpts;
typedef struct lisa_ws {
 struct {char scheme[8],host[16],port[8],path[16];} u_info;
 bool ws_conn,ws_stop,worker_busy,cleanup_requested;int32_t connect_cancel;
 unsigned timeout;const char*extra_header;void*user;
 lisa_thread_t *thread;
 void*tx_msg_queue,*exiting_sem,*exited_sem,*pong_sem,*connect_sem;
 noPollCtx*nopoll_ctx;noPollConn*nopoll_conn;
 void(*inter_on_event)(lisa_ws_event_t*);
 struct {unsigned send_max_ms,receive_max_ms;} transport;
 struct {bool active;unsigned since_ms;} pending_write;
 unsigned last_ping_sent_ms,ping_pong_lost_cnt;
} lisa_ws_t;
''' + ATOMIC_STUBS + r'''
static lisa_ws_t ws;
static lisa_thread_t worker_thread={(void*)1};
static void*current_task;
static noPollCtx np_ctx;static noPollConn np_conn;static noPollConnOpts np_opts;
static unsigned phase,now_ms,connect_token,deleted,freed,ack,connected,disconnected;
static unsigned live_ctx,live_conn,live_opts,init_events;
static bool cancel_seen;
static jmp_buf parked;
lisa_ws_err_e lisa_ws_cleanup(lisa_ws_t*);
static void*xTaskGetCurrentTaskHandle(void){return current_task;}
static unsigned lisa_os_get_tick_ms(void){return now_ms;}
static void lisa_semaphore_reset(void*s){if(s==ws.connect_sem)connect_token=0;}
static void lisa_semaphore_give(void*s){if(s==ws.connect_sem)connect_token++;if(s==ws.exited_sem)ack++;}
static int lisa_semaphore_take(void*s,unsigned n){
 if(s==ws.connect_sem){if(connect_token){connect_token--;return 0;}
  if(!ws.worker_busy)longjmp(parked,1);}
 now_ms+=n;return -1;
}
static void lisa_thread_mdelay(unsigned n){now_ms+=n;}
static void lisa_thread_delete(lisa_thread_t*t){assert(!ws.worker_busy);deleted++;}
static void lisa_semaphore_delete(void*s){}
static int lisa_queue_pop(void*q,void*m,unsigned n,unsigned t){return -1;}
static void lisa_queue_delete(void*q){}
static void lisa_mem_free(void*p){assert(p==&ws);assert(!ws.worker_busy);freed++;}
static void lisa_websocket_rx_msg_reset(lisa_ws_t*p){}
static void cancel_on_other_task(void){
 void*saved=current_task;current_task=NULL;
 unsigned started=now_ms;
 assert(lisa_ws_cleanup(&ws)==LISA_WS_BUSY);
 assert(!deleted && !freed && now_ms-started==10000);
 assert(ws.connect_cancel && ws.worker_busy);cancel_seen=true;current_task=saved;
}
static noPollCtx*nopoll_ctx_new(void){live_ctx++;return &np_ctx;}
static noPollConnOpts*nopoll_conn_opts_new(void){live_opts++;return &np_opts;}
static void nopoll_conn_opts_set_extra_headers(noPollConnOpts*p,const char*s){}
static int nopoll_conn_opts_set_ssl_certs(noPollConnOpts*p,...){return 1;}
static void nopoll_conn_opts_ssl_peer_verify(noPollConnOpts*p,int n){}
static void nopoll_conn_opts_unref(noPollConnOpts*p){assert(live_opts);live_opts--;}
static noPollConn*nopoll_conn_tls_new_with_ctrl(noPollCtx*c,noPollConnOpts*o,
 const char*h,const char*p,const char*hn,const char*url,const char*pr,const char*origin,const int32_t*ctrl){
 if(phase==1)cancel_on_other_task(); /* DNS returns after cancellation */
 if(*ctrl){nopoll_conn_opts_unref(o);return NULL;}
 live_conn++;
 if(phase==2)cancel_on_other_task(); /* TLS returns its shutdown connection */
 nopoll_conn_opts_unref(o);return &np_conn;
}
static noPollConn*nopoll_conn_new_opts_with_ctrl(noPollCtx*c,noPollConnOpts*o,
 const char*h,const char*p,const char*hn,const char*url,const char*pr,const char*origin,const int32_t*ctrl){
 return nopoll_conn_tls_new_with_ctrl(c,o,h,p,hn,url,pr,origin,ctrl);
}
static bool nopoll_conn_wait_until_connection_ready_with_ctrl(noPollConn*c,unsigned n,const int32_t*ctrl){
 if(phase==3)cancel_on_other_task();return !*ctrl && phase==4;
}
static void nopoll_conn_close(noPollConn*c){assert(live_conn);live_conn--;}
static void nopoll_ctx_unref(noPollCtx*c){assert(live_ctx);live_ctx--;}
static void nopoll_conn_on_close(void){}
static void nopoll_conn_set_on_close(noPollConn*c,void(*fn)(void),void*p){}
static bool nopoll_conn_is_ok(noPollConn*c){return true;}
static void on_event(lisa_ws_event_t*e){init_events++;}
static void _callback_ws_connected(lisa_ws_t*p){assert(!p->connect_cancel);p->ws_conn=true;connected++;}
static void _callback_ws_disconnect(lisa_ws_t*p){p->ws_conn=false;disconnected++;}
static int lisa_websocket_msg_send_proc(lisa_ws_t*p,noPollConn*c){
 assert(p->ws_conn && phase==4);cancel_on_other_task();return 0;
}
static void lisa_websocket_msg_recv_proc(lisa_ws_t*p,noPollConn*c){}
static void lisa_ws_note_max(uint32_t*p,uint32_t n){if(n>*p)*p=n;}
''' + "\n".join(function(source, name) for name in (
            "lisa_websocket_need_to_exit", "lisa_websocket_nopoll_conn",
            "lisa_websocket_nopoll_send_recv_proc", "lisa_websocket_thread",
            "lisa_ws_connect", "lisa_ws_disconnect", "lisa_ws_cleanup")) + r'''
int main(void){
 for(phase=0;phase<=5;phase++){
  ws=(lisa_ws_t){.timeout=20,.thread=&worker_thread,.connect_sem=(void*)2,
   .exiting_sem=(void*)3,.exited_sem=(void*)4,.pong_sem=(void*)5,.inter_on_event=on_event};
  strcpy(ws.u_info.scheme,phase==2?"ws":"wss");
  now_ms=deleted=freed=ack=connected=disconnected=connect_token=init_events=0;
  cancel_seen=false;current_task=NULL;
  assert(lisa_ws_connect(&ws)==LISA_WS_OK && ws.worker_busy);
  if(phase==5)cancel_on_other_task(); /* cancel queued connect before dispatch */
  current_task=(void*)1;
  if(!setjmp(parked))lisa_websocket_thread(&ws);
  assert(!ws.worker_busy && ack==1 && !live_ctx && !live_conn && !live_opts);
  assert(cancel_seen==(phase!=0));
  assert(connected==(phase==4));
  assert(disconnected==(phase<5));
  assert(init_events==(phase<5));
  current_task=NULL;
  assert(lisa_ws_cleanup(&ws)==LISA_WS_OK && deleted==1 && freed==1);
 }
 return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
