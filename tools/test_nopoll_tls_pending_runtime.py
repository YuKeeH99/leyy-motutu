"""TLS pending ownership is exercised through the current send/flush API."""
import unittest
from tools import test_offline_action as support
from tools.test_nopoll_control_runtime import PREAMBLE, implementation, function


class TlsPendingRuntimeTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_tls_want_codes_keep_same_plaintext_then_fatal_error_closes(self):
        source = (support.ROOT/'arcs-sdk/modules/nopoll/src/nopoll_conn.c').read_text(encoding='utf-8')
        preamble = PREAMBLE.replace('void *ctx;', 'void *ssl;void *ctx;')
        self.run_c(preamble + r'''
#define NOPOLL_MBEDTLS 1
#define MBEDTLS_ERR_SSL_WANT_READ (-0x6900)
#define MBEDTLS_ERR_SSL_WANT_WRITE (-0x6880)
static int tls_result,tls_calls,closed;
static unsigned char *first_buffer;
static int first_len;
static int mbedtls_ssl_write(void *ssl,unsigned char *buf,int len) {
 (void)ssl;++tls_calls;
 if(!first_buffer) {first_buffer=buf;first_len=len;}
 else {assert(buf==first_buffer && len==first_len);}
 return tls_result ? tls_result : len;
}
static void nopoll_conn_shutdown(noPollConn *c) {(void)c;++closed;}
''' + function(source, 'int nopoll_conn_tls_send (') + implementation() + r'''
int main(void) {
 noPollConn c={.role=NOPOLL_ROLE_CLIENT,.nopoll_send=nopoll_conn_tls_send};
 tls_result=MBEDTLS_ERR_SSL_WANT_WRITE;errno=0;
 assert(nopoll_conn_send_frame(&c,1,1,1,3,"abc",0)==-2);
 assert(tls_calls==1 && c.pending_write && c.pending_write_desp==0 && c.pending_write_bytes==9);
 tls_result=MBEDTLS_ERR_SSL_WANT_READ;errno=0;
 assert(nopoll_conn_complete_pending_write(&c)==-2);
 assert(tls_calls==2 && c.pending_write==(char *)first_buffer && c.pending_write_bytes==9);
 tls_result=0;
 assert(nopoll_conn_complete_pending_write(&c)==3 && !c.pending_write && !closed);
 first_buffer=NULL;tls_result=-0x004e;errno=EWOULDBLOCK;
 assert(nopoll_conn_send_frame(&c,1,1,1,3,"bad",0)==-1);
 assert(closed==1 && !c.pending_write);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
