"""Execute production noPoll serialization across partial/TLS/control writes."""
import unittest
from tools import test_offline_action as support


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


PREAMBLE = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef int nopoll_bool;
typedef int noPollOpCode;
typedef void *noPollPtr;
typedef struct noPollConn noPollConn;
struct noPollConn {
 void *ctx;int id,role,__force_stop_after_header;
 char *pending_write;int pending_write_bytes,pending_write_added_header,pending_write_desp;
 unsigned pending_write_sequence;
 char pending_pong[125];int pending_pong_bytes;int pending_pong_active;
 int (*nopoll_send)(noPollConn *,char *,int);
};
#define nopoll_true 1
#define nopoll_false 0
#define NOPOLL_ROLE_CLIENT 1
#define NOPOLL_PING_FRAME 9
#define NOPOLL_PONG_FRAME 10
#define NOPOLL_EWOULDBLOCK EWOULDBLOCK
#define nopoll_new(type,count) ((type *)calloc((count),sizeof(type)))
#define nopoll_free free
#define nopoll_log(...) ((void)0)
#define nopoll_sleep(...) ((void)0)
#define random rand
static void nopoll_set_bit(char *p,int n) {*p |= 1U<<n;}
static void nopoll_set_16bit(unsigned v,char *p) {p[0]=v>>8;p[1]=v;}
static void nopoll_set_32bit(unsigned v,char *p) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void nopoll_conn_mask_content(void *ctx,char *p,long len,char *mask,int offset) {
 (void)ctx;for(long i=0;i<len;i++)p[i]^=mask[(i+offset)%4];
}
static int nopoll_conn_send_frame(noPollConn *,nopoll_bool,nopoll_bool,noPollOpCode,long,noPollPtr,long);
static int nopoll_conn_complete_pending_write(noPollConn *);
static unsigned calls,wire_len;
static unsigned char wire[256];
static int mode;
static char *tls_original;
static int tls_length;
static int socket_send(noPollConn *c,char *p,int len) {
 (void)c;++calls;
 if(mode==1) {
  assert(p==tls_original && len==tls_length);errno=EWOULDBLOCK;return -2;
 }
 if(mode==2) {memcpy(wire+wire_len,p,1);++wire_len;return 1;}
 if(mode==3) {
  if(calls==2 || calls==4) {errno=EWOULDBLOCK;return -2;}
  int n=calls==1?3:calls==3?11:len;
  memcpy(wire+wire_len,p,n);wire_len+=n;return n;
 }
 memcpy(wire+wire_len,p,len);wire_len+=len;return len;
}
'''


def implementation():
    source = (support.ROOT / 'arcs-sdk/modules/nopoll/src/nopoll_conn.c').read_text(encoding='utf-8')
    code = ''
    # Optional during RED: no helper exists in the unfixed source.
    if 'static int __nopoll_conn_flush_pending_pong (' in source:
        code += function(source, 'static int __nopoll_conn_flush_pending_pong (')
    code += function(source, 'int __nopoll_conn_complete_pending_write_reduce_header (')
    code += function(source, 'int nopoll_conn_complete_pending_write (').replace(
        'int nopoll_conn_complete_pending_write (', 'static int nopoll_conn_complete_pending_write (', 1)
    code += function(source, 'int nopoll_conn_send_frame (').replace(
        'int nopoll_conn_send_frame (', 'static int nopoll_conn_send_frame (', 1)
    code += function(source, 'nopoll_bool      nopoll_conn_send_pong (')
    code += function(source, 'nopoll_bool      nopoll_conn_send_ping (')
    return code


class NopollControlRuntimeTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_partial_payload_finishes_before_queued_pong(self):
        self.run_c(PREAMBLE + implementation() + r'''
int main(void) {
 noPollConn c={.role=NOPOLL_ROLE_CLIENT,.nopoll_send=socket_send};
 c.pending_write=calloc(20,1);memset(c.pending_write,'X',20);c.pending_write_bytes=20;
 char *original=c.pending_write;
 mode=2;
 assert(nopoll_conn_send_pong(&c,0,NULL));
 assert(calls==0 && c.pending_write==original && c.pending_write_bytes==20);
 assert(nopoll_conn_complete_pending_write(&c)==1);
 assert(calls==1 && c.pending_write_bytes==19 && c.pending_write==original);
 mode=0;assert(nopoll_conn_complete_pending_write(&c)==19);
 assert(wire_len==26 && c.pending_write==NULL && !c.pending_pong_active);
 for(unsigned i=0;i<20;i++)assert(wire[i]=='X');
 assert(wire[20]==0x8a && wire[21]==0x80);
 return 0;
}
''')

    def test_tls_want_write_keeps_exact_buffer_until_ack_then_pong(self):
        self.run_c(PREAMBLE + implementation() + r'''
int main(void) {
 noPollConn c={.role=NOPOLL_ROLE_CLIENT,.nopoll_send=socket_send};
 c.pending_write=calloc(20,1);memset(c.pending_write,'T',20);c.pending_write_bytes=20;
 tls_original=c.pending_write;tls_length=20;mode=1;
 assert(nopoll_conn_complete_pending_write(&c)<0);
 assert(nopoll_conn_send_pong(&c,1,"a"));
 assert(nopoll_conn_send_pong(&c,1,"b")); /* Most recent unanswered ping. */
 assert(nopoll_conn_complete_pending_write(&c)<0);
 assert(c.pending_write==tls_original && c.pending_write_bytes==20 && c.pending_write_desp==0);
 mode=0;errno=EWOULDBLOCK;
 assert(nopoll_conn_complete_pending_write(&c)==20);
 assert(wire_len==27 && !c.pending_write && !c.pending_pong_active);
 for(unsigned i=0;i<20;i++)assert(wire[i]=='T');
 assert(wire[20]==0x8a && wire[21]==0x81 && (wire[26]^wire[22])=='b');
 return 0;
}
''')

    def test_new_frame_cannot_overwrite_partially_flushed_old_frame(self):
        self.run_c(PREAMBLE + implementation() + r'''
int main(void) {
 noPollConn c={.role=NOPOLL_ROLE_CLIENT,.nopoll_send=socket_send};
 c.pending_write=calloc(20,1);c.pending_write_bytes=20;char *original=c.pending_write;
 mode=2;
 assert(nopoll_conn_send_frame(&c,1,1,1,3,"new",0)<0);
 assert(calls==1 && c.pending_write_bytes==19 && c.pending_write==original);
 free(c.pending_write);return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
