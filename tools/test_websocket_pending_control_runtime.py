"""Real noPoll and websocket accounting across a deferred PONG frame boundary."""
import unittest
from tools import test_offline_action as support
from tools.test_nopoll_control_runtime import PREAMBLE, function, implementation


class WebsocketPendingControlRuntimeTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def harness(self):
        ws = (support.ROOT/'arcs-sdk/components/lisa_websocket/lisa_websocket.c').read_text(encoding='utf-8')
        nopoll = (support.ROOT/'arcs-sdk/modules/nopoll/src/nopoll_conn.c').read_text(encoding='utf-8')
        policy = support.ROOT/'arcs-sdk/components/lisa_websocket/lisa_websocket_pending_policy.h'
        tracker = (policy.parent/'lisa_websocket_pending_policy.c').read_text(encoding='utf-8')
        getter = (function(nopoll, 'unsigned int nopoll_conn_pending_write_sequence (')
                  if 'unsigned int nopoll_conn_pending_write_sequence (' in nopoll else '')
        return PREAMBLE + implementation() + getter + function(nopoll, 'int           nopoll_conn_pending_write_bytes (') + r'''
#include <stdbool.h>
#include <stdint.h>
''' + policy.read_text(encoding='utf-8') + function(tracker, 'lisa_ws_pending_decision_t lisa_ws_pending_tracker_update(') + r'''
#define LISA_NLOGI(...) ((void)0)
#define LISA_NLOGW(...) ((void)0)
#define LISA_NLOGE(...) ((void)0)
typedef struct {
 struct {uint32_t pending_bytes,written_bytes,completed_frames;} transport;
 lisa_ws_pending_tracker_t pending_write;
 bool pending_payload_frame;
 uint32_t pong_max_time_ms;
} lisa_ws_t;
static unsigned now_ms;
static uint32_t lisa_os_get_tick_ms(void) {return now_ms;}
static bool nopoll_conn_is_ok(noPollConn *c) {(void)c;return true;}
static bool hold_control=true;
static int ordered_socket(noPollConn *c,char *p,int len) {
 (void)c;
 if(hold_control && (unsigned char)p[0]!=0x58) {errno=EWOULDBLOCK;return -2;}
 memcpy(wire+wire_len,p,len);wire_len+=len;return len;
}
''' + function(ws, 'static int lisa_websocket_pending_write_proc(')

    def test_new_pong_gets_own_deadline_and_no_application_accounting(self):
        self.run_c(self.harness() + r'''
int main(void) {
 noPollConn c={.role=NOPOLL_ROLE_CLIENT,.nopoll_send=ordered_socket};
 c.pending_write=calloc(20,1);memset(c.pending_write,'X',20);c.pending_write_bytes=20;
 lisa_ws_t ws={.pending_write={.active=true,.since_ms=1},
               .pending_payload_frame=true,.pong_max_time_ms=3000};
 assert(nopoll_conn_send_pong(&c,1,"a"));
 now_ms=2900;
 assert(lisa_websocket_pending_write_proc(&ws,&c)==1);
 assert(ws.transport.written_bytes==20 && ws.transport.completed_frames==1);
 assert(!ws.pending_payload_frame && ws.pending_write.since_ms==2900);
 now_ms=3002;
 assert(lisa_websocket_pending_write_proc(&ws,&c)==1);
 hold_control=false;now_ms=3200;
 assert(lisa_websocket_pending_write_proc(&ws,&c)==0);
 assert(ws.transport.written_bytes==20 && ws.transport.completed_frames==1);
 assert(wire_len==27 && !c.pending_write);
 return 0;
}
''')

    def test_new_pong_still_has_bounded_own_stall_timeout(self):
        self.run_c(self.harness() + r'''
int main(void) {
 noPollConn c={.role=NOPOLL_ROLE_CLIENT,.nopoll_send=ordered_socket};
 c.pending_write=calloc(20,1);memset(c.pending_write,'X',20);c.pending_write_bytes=20;
 lisa_ws_t ws={.pending_write={.active=true,.since_ms=1},
               .pending_payload_frame=true,.pong_max_time_ms=3000};
 assert(nopoll_conn_send_pong(&c,0,NULL));
 now_ms=2900;assert(lisa_websocket_pending_write_proc(&ws,&c)==1);
 now_ms=5899;assert(lisa_websocket_pending_write_proc(&ws,&c)==1);
 now_ms=5900;assert(lisa_websocket_pending_write_proc(&ws,&c)==-1);
 assert(ws.transport.written_bytes==20 && ws.transport.completed_frames==1);
 free(c.pending_write);return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
