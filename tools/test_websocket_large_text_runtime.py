"""A large text message owns one serialized frame across all short writes."""
import unittest
from tools import test_offline_action as support
from tools.test_nopoll_control_runtime import PREAMBLE, implementation, function


class WebsocketLargeTextRuntimeTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_over_512_bytes_is_sent_once_without_fragment_retry_loop(self):
        source = (support.ROOT/'arcs-sdk/components/lisa_websocket/lisa_websocket.c').read_text(encoding='utf-8')
        self.run_c(PREAMBLE.replace('wire[256]', 'wire[4096]') + implementation() + r'''
#include <stdint.h>
#define LISA_WS_TEXT 0
#define LISA_WS_BIN 1
#define LISA_WEBSOCKET_FRAME_MAX_SIZE 512
#define NOPOLL_TEXT_FRAME 1
#define LISA_NLOGI(...) ((void)0)
#define LISA_NLOGE(...) ((void)0)
typedef struct {struct {unsigned written_bytes;} transport;} lisa_ws_t;
typedef struct {int m_type;char *m_msg;unsigned m_size;} lisa_ws_msg_t;
typedef enum {LISA_WS_SEND_FAILED=-1,LISA_WS_SEND_COMPLETE=0,LISA_WS_SEND_PENDING=1} lisa_ws_send_outcome_t;
static lisa_ws_send_outcome_t lisa_ws_classify_send_result(int n,int p) {
 if(n==-2)return p>0?LISA_WS_SEND_PENDING:LISA_WS_SEND_FAILED;
 if(n<0)return LISA_WS_SEND_FAILED;
 return p>0?LISA_WS_SEND_PENDING:LISA_WS_SEND_COMPLETE;
}
static int nopoll_conn_pending_write_bytes(noPollConn *c) {return c->pending_write?c->pending_write_bytes:0;}
static int nopoll_conn_send_text(noPollConn *c,char *p,int n) {return nopoll_conn_send_frame(c,1,1,1,n,p,0);}
static int nopoll_conn_send_binary(noPollConn *c,char *p,int n) {(void)c;(void)p;(void)n;assert(0);return -1;}
static int lisa_websocket_fragment_send(noPollConn *c,int op,int size,char *p,int n) {
 (void)c;(void)op;(void)size;(void)p;(void)n;assert(!"legacy fragment retry path");return -1;
}
''' + function(source, 'static int lisa_websocket_msg_normal_send(') +
function(source, 'static int lisa_websocket_msg_send(') + r'''
int main(void) {
 noPollConn c={.role=NOPOLL_ROLE_CLIENT,.nopoll_send=socket_send};lisa_ws_t ws={0};
 char payload[1024];for(unsigned i=0;i<sizeof(payload);i++)payload[i]='a'+(i%26);
 lisa_ws_msg_t msg={LISA_WS_TEXT,payload,sizeof(payload)};mode=3;
 assert(lisa_websocket_msg_send(&ws,&c,&msg)==0);
 assert(calls==1 && c.pending_write && wire_len==3);
 unsigned acknowledged=ws.transport.written_bytes;
 for(unsigned i=0;c.pending_write && i<8;i++) {
  int n=nopoll_conn_complete_pending_write(&c);if(n>0)acknowledged+=n;
 }
 assert(!c.pending_write && calls==5 && wire_len==1032 && acknowledged==1024);
 assert(wire[0]==0x81 && wire[1]==0xfe && wire[2]==4 && wire[3]==0);
 for(unsigned i=0;i<sizeof(payload);i++)assert((wire[8+i]^wire[4+(i%4)])==(unsigned char)payload[i]);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
