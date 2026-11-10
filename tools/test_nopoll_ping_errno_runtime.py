"""A completed empty ping must not inherit EWOULDBLOCK from an earlier read.

Executes the production frame serializer and ping wrapper; only the socket
boundary is simulated so full writes and backpressure are deterministic.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_task_continue_guard import ROOT, CC, RUN


class PingErrnoRuntimeTest(unittest.TestCase):
    def test_completed_ping_with_stale_errno_and_real_pending(self):
        source = (ROOT / 'arcs-sdk/modules/nopoll/src/nopoll_conn.c').read_text(encoding='utf-8')

        def function(signature):
            start = source.index(signature)
            return source[start:source.index('\n}', start) + 2]

        harness = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef int nopoll_bool;
typedef int noPollOpCode;
typedef void *noPollPtr;
typedef struct noPollConn noPollConn;
struct noPollConn {
 void *ctx; int id, role, __force_stop_after_header;
 char *pending_write; int pending_write_bytes, pending_write_added_header, pending_write_desp;
 unsigned pending_write_sequence;
 int (*nopoll_send)(noPollConn *, char *, int);
};
#define nopoll_true 1
#define nopoll_false 0
#define NOPOLL_ROLE_CLIENT 1
#define NOPOLL_PING_FRAME 9
#define NOPOLL_EWOULDBLOCK EWOULDBLOCK
#define nopoll_new(type,count) ((type *)calloc((count),sizeof(type)))
#define nopoll_free free
#define nopoll_log(...) ((void)0)
#define nopoll_sleep(...) ((void)0)
#define random rand
static void nopoll_set_bit(char *p, int n) { *p |= 1U << n; }
static void nopoll_set_16bit(unsigned v, char *p) { p[0]=v>>8; p[1]=v; }
static void nopoll_set_32bit(unsigned v, char *p) {
 p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
static void nopoll_conn_mask_content(void *ctx,char *p,long len,char *mask,int offset) {
 (void)ctx; for(long i=0;i<len;i++) p[i]^=mask[(i+offset)%4];
}
static int nopoll_conn_complete_pending_write(noPollConn *c) {
 assert(c->pending_write==NULL); return 0;
}
static int mode, wire_bytes;
static unsigned char wire[128];
static int socket_send(noPollConn *c,char *p,int len) {
 (void)c;
 if(mode==1) { errno=EWOULDBLOCK; return -26752; }
 if(mode==2) { memcpy(wire,p,2); wire_bytes+=2; errno=EWOULDBLOCK; return 2; }
 memcpy(wire,p,len); wire_bytes+=len; return len;
}
''' + function('int nopoll_conn_send_frame (') + '\n' + function('nopoll_bool      nopoll_conn_send_ping (') + r'''
int main(void) {
 noPollConn c={0}; c.role=NOPOLL_ROLE_CLIENT; c.nopoll_send=socket_send;
 errno=EWOULDBLOCK;
 assert(nopoll_conn_send_ping(&c));
 assert(wire_bytes==6 && wire[0]==0x89 && wire[1]==0x80);
 assert(c.pending_write==NULL && c.pending_write_bytes==0);
 errno=EWOULDBLOCK;
 assert(nopoll_conn_send_frame(&c,1,1,1,3,"abc",0)==3);
 assert(c.pending_write_bytes==0);
 mode=1; wire_bytes=0;
 assert(!nopoll_conn_send_ping(&c));
 assert(wire_bytes==0 && c.pending_write_bytes==6 && c.pending_write_desp==0);
 free(c.pending_write); c.pending_write=NULL;
 mode=2; wire_bytes=0;
 assert(!nopoll_conn_send_ping(&c));
 assert(wire_bytes==2 && c.pending_write_bytes==4 && c.pending_write_desp==2);
 free(c.pending_write);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cfile, elf = Path(directory) / 'ping.c', Path(directory) / 'ping.elf'
            cfile.write_text(harness, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(cfile), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)


if __name__ == '__main__':
    unittest.main()
