"""Run the actual websocket worker against a deterministic clock and peer."""
import unittest
from tools import test_offline_action as support
from tools.test_nopoll_control_runtime import function


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_NOPOLL_DELIVER_PONG_FRAME_ENABLED 1
#define WS_PINGPONG_RETRY_CNT 5
#define LISA_OK 0
#define LISA_NLOGI(...) ((void)0)
#define LISA_NLOGE(...) ((void)0)
typedef struct {char *m_msg;} lisa_ws_msg_t;
typedef struct {uint32_t send_max_ms,receive_max_ms;} Stats;
typedef struct {
 uint32_t last_ping_sent_ms,pong_max_time_ms;
 unsigned ping_pong_lost_cnt;
 struct {bool active;uint32_t since_ms;} pending_write;
 bool ws_stop;
 Stats transport;
 void *nopoll_conn,*nopoll_ctx,*tx_msg_queue;
 int pong_sem,exited_sem;
} lisa_ws_t;
static unsigned now_ms,pings,disconnect_at,peer_pong_due,pending_until;
static unsigned stop_at=30000,recv_delay,pong_delay;
static bool fail_ping,partial_ping,respond;
static uint32_t lisa_os_get_tick_ms(void) {return now_ms;}
static bool lisa_websocket_need_to_exit(lisa_ws_t *p) {(void)p;return now_ms>=stop_at;}
static bool nopoll_conn_is_ok(void *p) {(void)p;return true;}
static int lisa_websocket_msg_send_proc(lisa_ws_t *p,void *c) {(void)p;(void)c;now_ms+=20;return 0;}
static void lisa_websocket_msg_recv_proc(lisa_ws_t *p,void *c) {(void)p;(void)c;now_ms+=recv_delay;}
static void lisa_ws_note_max(uint32_t *p,uint32_t n) {if(n>*p)*p=n;}
static void lisa_thread_mdelay(unsigned n) {now_ms+=n;}
static int nopoll_conn_pending_write_bytes(void *p) {(void)p;return now_ms<pending_until?6:0;}
static int nopoll_conn_send_ping(void *p) {
 (void)p;assert(now_ms>=pending_until);++pings;
 if(fail_ping) return 0;
 if(partial_ping) {pending_until=now_ms+100;partial_ping=false;}
 if(respond)peer_pong_due=now_ms+pong_delay;
 return now_ms>=pending_until;
}
static int lisa_semaphore_take(int s,int t) {
 (void)s;(void)t;
 if(peer_pong_due && now_ms>=peer_pong_due) {peer_pong_due=0;return 0;}
 return -1;
}
static void lisa_semaphore_give(int s) {(void)s;}
static int lisa_queue_pop(void *q,void *d,unsigned sz,int t) {(void)q;(void)d;(void)sz;(void)t;return -1;}
#define lisa_mem_free free
static void nopoll_conn_close(void *p) {(void)p;}
static void nopoll_ctx_unref(void *p) {(void)p;}
static void _callback_ws_disconnect(lisa_ws_t *p) {(void)p;disconnect_at=now_ms;}
'''


class WebsocketHeartbeatRuntimeTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def run_worker(self, setup, checks):
        source = (support.ROOT/'arcs-sdk/components/lisa_websocket/lisa_websocket.c').read_text(encoding='utf-8')
        self.run_c(PREAMBLE + function(source, 'static int lisa_websocket_nopoll_send_recv_proc(') +
                   '\nint main(void) {lisa_ws_t ws={.pong_max_time_ms=3000,.nopoll_conn=(void *)1};' +
                   setup + '\nlisa_websocket_nopoll_send_recv_proc(&ws);' + checks + '\nreturn 0;}')

    def test_missing_pong_exhausts_bounded_retries(self):
        self.run_worker('', 'assert(disconnect_at<30000 && pings==5);')

    def test_normal_pong_and_slow_receive_keep_connection(self):
        self.run_worker('respond=true;pong_delay=200;recv_delay=500;',
                        'assert(disconnect_at>=30000 && pings>5 && !ws.ping_pong_lost_cnt);')

    def test_partial_ping_is_not_failure_or_overwritten(self):
        self.run_worker('respond=true;pong_delay=200;partial_ping=true;',
                        'assert(disconnect_at>=30000 && pings>5 && !ws.ping_pong_lost_cnt);')

    def test_pending_application_frame_delays_ping(self):
        self.run_worker('pending_until=3500;stop_at=2000;', 'assert(pings==0);')

    def test_unaccepted_ping_fails_without_claiming_a_response_cycle(self):
        self.run_worker('fail_ping=true;', 'assert(disconnect_at<4000 && pings==1);')


if __name__ == '__main__':
    unittest.main()
