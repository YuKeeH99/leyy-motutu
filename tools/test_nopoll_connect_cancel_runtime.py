"""Production connect/close code with deterministic DNS, TLS and TCP boundaries."""
import unittest
from tools import test_offline_action as support
from tools.test_nopoll_control_runtime import function


PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>
#define NOPOLL_MBEDTLS 1
#define NOPOLL_LWIP 1
#define NOPOLL_NO_IPV6 1
#define NOPOLL_OS_FREERTOS 1
#define nopoll_true 1
#define nopoll_false 0
#define NOPOLL_INVALID_SOCKET (-1)
#define NOPOLL_TRANSPORT_IPV4 1
#define NOPOLL_ROLE_CLIENT 1
#define NOPOLL_CLOSE_FRAME 8
#define NOPOLL_EWOULDBLOCK EWOULDBLOCK
#define NOPOLL_EINPROGRESS EINPROGRESS
#define NOPOLL_ENOTCONN ENOTCONN
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_KEEPALIVE 9
#define IPPROTO_TCP 6
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6
#define SHUT_RDWR 2
#define MBEDTLS_SSL_IS_CLIENT 0
#define MBEDTLS_SSL_TRANSPORT_STREAM 0
#define MBEDTLS_SSL_PRESET_DEFAULT 0
#define MBEDTLS_SSL_VERIFY_NONE 0
#define MBEDTLS_SSL_VERIFY_REQUIRED 1
#define MBEDTLS_ERR_SSL_WANT_READ (-2)
#define MBEDTLS_ERR_SSL_WANT_WRITE (-3)
#define nopoll_log(...) ((void)0)
typedef int nopoll_bool,noPollTransport,NOPOLL_SOCKET;
typedef int fd_set;
typedef struct {int state,fd;} TLS;
typedef TLS mbedtls_net_context,mbedtls_ssl_context,mbedtls_ssl_config,mbedtls_x509_crt;
typedef TLS mbedtls_pk_context,mbedtls_entropy_context,mbedtls_ctr_drbg_context;
typedef struct noPollConn noPollConn;
typedef struct {int refs;long conn_connect_std_timeout;noPollConn *registered;} noPollCtx;
typedef struct {
 char *certificate,*chain_certificate,*ca_certificate,*private_key;
 int certificate_size,chain_certificate_size,ca_certificate_size,private_key_size;
 int disable_ssl_verify,reuse;
} noPollConnOpts;
typedef struct {char *websocket_key,*websocket_version,*websocket_accept,*expected_accept,*cookie;} Handshake;
struct noPollConn {
 int refs,id,session,role,peer_close_status,pending_ssl_connect,tls_on;
 void *ref_mutex,*handshake_mutex,*pending_msg,*previous_msg;
 noPollCtx *ctx;noPollConnOpts *opts;Handshake *handshake;
 char *host,*port,*host_name,*origin,*get_url,*protocols,*accepted_protocol,*x_real_ip_address;
 char *peer_close_reason,*certificate,*private_key,*chain_certificate,*pending_write;
 int certificate_size,private_key_size,chain_certificate_size;
 TLS *fd_ctx,*ssl,*ssl_conf,*ssl_cert,*ssl_ca_cert,*ssl_pkey,*ssl_entropy,*ssl_ctr_drbg;
 int (*receive)(noPollConn *,char *,int),(*nopoll_send)(noPollConn *,char *,int);
 void (*on_close)(noPollCtx *,noPollConn *,void *);void *on_close_data;
};
static int32_t cancel_flag;
static unsigned stage,now_us,dns_calls,socket_calls,socket_closes,tls_calls,send_calls,sleeps,options_freed;
static uint32_t tick_base;
static int64_t wall_offset_us;
#define configTICK_RATE_HZ 1000U
static uint32_t rtos_now(bool isr){(void)isr;return tick_base+now_us/1000U;}
static unsigned live;static void *allocations[128];
static void *tracked_alloc(size_t n) {
 void *p=calloc(1,n);assert(p);
 for(unsigned i=0;i<128;i++)if(!allocations[i]){allocations[i]=p;++live;return p;}
 assert(0);return NULL;
}
static void tracked_free(void *p) {
 if(!p)return;for(unsigned i=0;i<128;i++)if(allocations[i]==p){allocations[i]=NULL;--live;free(p);return;}
 assert(!"double or foreign free");
}
#define nopoll_new(t,n) ((t*)tracked_alloc(sizeof(t)*(n)))
#define nopoll_calloc(n,s) tracked_alloc((n)*(s))
#define nopoll_free tracked_free
static char *nopoll_strdup(const char *s){char *p=tracked_alloc(strlen(s)+1);strcpy(p,s);return p;}
static char *nopoll_strdup_printf(const char *f,const char *s){(void)f;return nopoll_strdup(s);}
static void *nopoll_mutex_create(void){return tracked_alloc(1);}
#define nopoll_mutex_lock(p) ((void)(p))
#define nopoll_mutex_unlock(p) ((void)(p))
#define nopoll_mutex_destroy tracked_free
static void nopoll_ctx_unref(noPollCtx *c){assert(c->refs>1);--c->refs;}
static void nopoll_conn_unref(noPollConn *c);
static void nopoll_conn_close(noPollConn *c);
static void nopoll_conn_shutdown(noPollConn *c);
static int nopoll_conn_ref_count(noPollConn *c){return c->refs;}
static int nopoll_ctx_register_conn(noPollCtx *x,noPollConn *c){assert(!x->registered);x->registered=c;++x->refs;++c->refs;return 1;}
static void nopoll_ctx_unregister_conn(noPollCtx *x,noPollConn *c){if(x->registered==c){x->registered=NULL;nopoll_conn_unref(c);}}
static void nopoll_msg_unref(void *p){(void)p;assert(0);}
static void nopoll_conn_opts_free(noPollConnOpts *p){tracked_free(p);++options_freed;}
static void __nopoll_conn_opts_release_if_needed(noPollConnOpts *p){if(p)nopoll_conn_opts_free(p);}
struct addrinfo {int ai_family,ai_socktype;void *ai_addr;unsigned ai_addrlen;};
static int getaddrinfo(const char *h,const char *p,const struct addrinfo *hint,struct addrinfo **r){
 (void)h;(void)p;(void)hint;++dns_calls;*r=tracked_alloc(sizeof(**r));if(stage==1)cancel_flag=1;return 0;
}
#define freeaddrinfo tracked_free
static int socket(int a,int b,int c){(void)a;(void)b;(void)c;++socket_calls;return 10;}
static int setsockopt(int a,int b,int c,const void *d,unsigned e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
static int connect(int s,const void *a,unsigned n){(void)s;(void)a;(void)n;if(stage==2)cancel_flag=1;return 0;}
static int shutdown(int s,int how){(void)s;(void)how;return 0;}
static void nopoll_close_socket(int s){assert(s==10);++socket_closes;}
#define nopoll_conn_set_sock_tcp_nodelay(...) ((void)0)
#define nopoll_conn_set_sock_block(...) ((void)0)
static void nopoll_sleep(unsigned us){now_us+=us;++sleeps;assert(sleeps<10000);if(stage==5)cancel_flag=1;if(stage==12)wall_offset_us-=3600000000LL;}
static void nopoll_freertos_gettimeofday(struct timeval *v,void *tz){(void)tz;int64_t wall=now_us+wall_offset_us;v->tv_sec=wall/1000000;v->tv_usec=wall%1000000;}
static void tls_init(TLS *p){p->state=1;}
static void tls_free(TLS *p){assert(p->state!=2);p->state=2;}
#define mbedtls_ssl_init tls_init
#define mbedtls_ssl_config_init tls_init
#define mbedtls_x509_crt_init tls_init
#define mbedtls_pk_init tls_init
#define mbedtls_entropy_init tls_init
#define mbedtls_ctr_drbg_init tls_init
#define mbedtls_ssl_free tls_free
#define mbedtls_ssl_config_free tls_free
#define mbedtls_x509_crt_free tls_free
#define mbedtls_pk_free tls_free
#define mbedtls_entropy_free tls_free
#define mbedtls_ctr_drbg_free tls_free
#define mbedtls_ssl_conf_dbg(...) ((void)0)
#define mbedtls_x509_crt_parse(...) 0
#define mbedtls_pk_parse_key(...) 0
#define mbedtls_ctr_drbg_seed(...) 0
#define mbedtls_ssl_config_defaults(...) 0
#define mbedtls_ssl_conf_authmode(...) ((void)0)
#define mbedtls_ssl_conf_rng(...) ((void)0)
#define mbedtls_ssl_conf_ca_chain(...) ((void)0)
#define mbedtls_ssl_conf_own_cert(...) 0
#define mbedtls_ssl_setup(...) 0
#define mbedtls_ssl_set_bio(...) ((void)0)
#define mbedtls_ssl_get_verify_result(...) 0
#define mbedtls_x509_crt_verify_info(...) ((void)0)
static int mbedtls_ssl_handshake(TLS *p){assert(p->state==1);++tls_calls;if(stage==3){cancel_flag=1;return -2;}if(stage==8)return -1;if(stage==9)return -2;return 0;}
static char sent[128];static unsigned sent_bytes;static char *want_ptr;static int want_len;
static int nopoll_conn_default_receive(noPollConn *c,char *b,int n){(void)c;(void)b;(void)n;return -2;}
static int nopoll_conn_tls_receive(noPollConn *c,char *b,int n){return nopoll_conn_default_receive(c,b,n);}
static int wire_send(noPollConn *c,char *b,int n){
 (void)c;++send_calls;assert(!cancel_flag);if(stage==4){cancel_flag=1;errno=EWOULDBLOCK;return -2;}
 if(stage==6){
  if(send_calls==1){memcpy(sent,b,3);sent_bytes=3;return 3;}
  if(send_calls==2){want_ptr=b;want_len=n;errno=EWOULDBLOCK;return -2;}
  if(send_calls==3){assert(b==want_ptr&&n==want_len);}
 }
 if(stage==7 || stage==10 || stage==11){now_us+=30000;if(stage==10)wall_offset_us+=3600000000LL;if(stage==11)wall_offset_us-=3600000000LL;memcpy(sent+sent_bytes,b,1);++sent_bytes;return 1;}
 memcpy(sent+sent_bytes,b,n);sent_bytes+=n;return n;
}
static int nopoll_conn_default_send(noPollConn *c,char *b,int n){return wire_send(c,b,n);}
static int nopoll_conn_tls_send(noPollConn *c,char *b,int n){return wire_send(c,b,n);}
static noPollConnOpts *last_options;
static char *__nopoll_conn_get_client_init(noPollConn *c,noPollConnOpts *p){(void)c;last_options=p;return nopoll_strdup("GET / HTTP/1.1\r\n\r\n");}
static int nopoll_conn_send_frame(noPollConn *c,int f,int m,int o,long n,void *p,long t){(void)c;(void)f;(void)m;(void)o;(void)n;(void)p;(void)t;return 0;}
static void nopoll_set_16bit(int v,char *p){(void)v;(void)p;}
static int nopoll_conn_is_ok(noPollConn *c){return c&&c->session!=-1;}
static int nopoll_conn_is_ready(noPollConn *c){return stage!=12&&nopoll_conn_is_ok(c)&&sleeps>=2;}
'''


def implementation():
    src = (support.ROOT/'arcs-sdk/modules/nopoll/src/nopoll_conn.c').read_text(encoding='utf-8')
    names = ['static nopoll_bool __nopoll_conn_cancelled (', 'static int64_t __nopoll_conn_now_us (',
             'static int64_t __nopoll_conn_clock (', 'static int64_t __nopoll_conn_elapsed_us (',
             'NOPOLL_SOCKET __nopoll_conn_sock_connect_opts_internal (',
             'noPollConn * __nopoll_conn_new_common (', 'noPollConn * nopoll_conn_new (',
             'noPollConn * nopoll_conn_new_with_ctrl (', 'noPollConn * nopoll_conn_tls_new (',
             'noPollConn * nopoll_conn_new_opts_with_ctrl (',
             'noPollConn * nopoll_conn_tls_new_with_ctrl (',
             'void          nopoll_conn_shutdown (', 'void nopoll_conn_unref (',
             'void          nopoll_conn_close_ext ', 'void          nopoll_conn_close ',
             'nopoll_bool      nopoll_conn_wait_until_connection_ready_with_ctrl (',
             'nopoll_bool      nopoll_conn_wait_until_connection_ready (']
    return '\n'.join(function(src, n).replace('void nopoll_conn_unref (','static void nopoll_conn_unref (')
                      .replace('void          nopoll_conn_shutdown (','static void nopoll_conn_shutdown (')
                      .replace('void          nopoll_conn_close  (','static void nopoll_conn_close  (')
                      for n in names if n in src)


class NopollConnectCancelRuntimeTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def run_stage(self, stage, assertions, tls=True):
        call = ('nopoll_conn_tls_new_with_ctrl(&ctx,nopoll_new(noPollConnOpts,1),"host","443",NULL,NULL,NULL,NULL,&cancel_flag)'
                if tls else 'nopoll_conn_new_with_ctrl(&ctx,"host","80",NULL,NULL,NULL,NULL,&cancel_flag)')
        self.run_c(PREAMBLE + implementation() + '\nint main(void) {\n'
                   f'noPollCtx ctx={{.refs=1,.conn_connect_std_timeout={2000000 if tls else 100000}}};stage={stage};'
                   f'noPollConn *c={call};' + assertions + r'''
 if(c){nopoll_conn_shutdown(c);nopoll_conn_close(c);}
 assert(ctx.refs==1 && !ctx.registered && !live);
 return 0;}
''')

    def test_cancel_after_dns_does_not_open_socket(self):
        self.run_stage(1, 'assert(!c && dns_calls==1 && socket_calls==0 && options_freed==1);')

    def test_cancel_after_tcp_closes_socket_once(self):
        self.run_stage(2, 'assert(!c && socket_closes==1 && tls_calls==0 && options_freed==1);')

    def test_cancel_during_tls_reclaims_each_allocation_once(self):
        self.run_stage(3, 'assert(c && !nopoll_conn_is_ok(c) && tls_calls==1 && !send_calls && socket_closes==1);')

    def test_fatal_tls_failure_reclaims_each_allocation_once(self):
        self.run_stage(8, 'assert(c && !nopoll_conn_is_ok(c) && tls_calls==1 && !send_calls && socket_closes==1);')

    def test_tls_want_has_bounded_total_budget(self):
        self.run_stage(9, 'assert(c && !nopoll_conn_is_ok(c) && tls_calls<=101 && !send_calls && now_us<=2010000);')

    def test_cancel_during_stabilization_stops_before_tls(self):
        self.run_stage(5, 'assert(c && !nopoll_conn_is_ok(c) && !tls_calls && !send_calls && now_us<=10000);')

    def test_cancel_during_initial_write_never_retries_freed_buffer(self):
        self.run_stage(4, 'assert(c && !nopoll_conn_is_ok(c) && send_calls==1 && socket_closes==1);', tls=False)

    def test_http_partial_and_want_write_have_exact_wire_bytes(self):
        self.run_stage(6, 'assert(c && nopoll_conn_is_ok(c) && send_calls==3);assert(sent_bytes==18 && !memcmp(sent,"GET / HTTP/1.1\\r\\n\\r\\n",18));', tls=False)

    def test_http_progress_cannot_extend_total_connect_budget(self):
        self.run_stage(7, 'assert(c && !nopoll_conn_is_ok(c) && send_calls<=4 && sent_bytes<18);', tls=False)

    def test_wall_clock_jump_cannot_shorten_or_extend_connect_budget(self):
        for stage in (10,11):
            with self.subTest(stage=stage):
                self.run_stage(stage, 'assert(c && !nopoll_conn_is_ok(c) && send_calls==4 && sent_bytes==4);', tls=False)

    def test_connect_and_ready_deadlines_survive_tick_wrap(self):
        self.run_c(PREAMBLE + implementation() + r'''
int main(void) {
 noPollCtx ctx={.refs=1,.conn_connect_std_timeout=100000};
 tick_base=UINT32_MAX-49;stage=7;
 noPollConn *c=nopoll_conn_new_with_ctrl(&ctx,"host","80",NULL,NULL,NULL,NULL,&cancel_flag);
 assert(c && !nopoll_conn_is_ok(c) && send_calls==4 && now_us==120000);
 nopoll_conn_close(c);assert(!live && ctx.refs==1);
 noPollConn ready={.session=10};stage=12;now_us=0;sleeps=0;
 assert(!nopoll_conn_wait_until_connection_ready_with_ctrl(&ready,1,&cancel_flag));
 assert(now_us==1000000 && sleeps==100);
 return 0;
}
''')

    def test_null_ctrl_remains_compatible(self):
        code = PREAMBLE + implementation() + r'''
int main(void) {
 noPollCtx ctx={.refs=1,.conn_connect_std_timeout=2000000};
 noPollConn *c=nopoll_conn_tls_new(&ctx,nopoll_new(noPollConnOpts,1),"host","443",NULL,NULL,NULL,NULL);
 assert(c && nopoll_conn_is_ok(c) && tls_calls==1 && send_calls==1);
 nopoll_conn_shutdown(c);nopoll_conn_close(c);
 assert(ctx.refs==1 && !ctx.registered && !live && socket_closes==1);return 0;
}
'''
        self.run_c(code)

    def test_plain_connection_preserves_options_and_precancel_skips_dns(self):
        self.run_c(PREAMBLE + implementation() + r'''
int main(void) {
 noPollCtx ctx={.refs=1,.conn_connect_std_timeout=2000000};
 cancel_flag=1;
 noPollConn *c=nopoll_conn_new_opts_with_ctrl(&ctx,nopoll_new(noPollConnOpts,1),"host","80",NULL,NULL,NULL,NULL,&cancel_flag);
 assert(!c && !dns_calls && !socket_calls && options_freed==1 && !live);
 cancel_flag=0;
 noPollConnOpts *opts=nopoll_new(noPollConnOpts,1);
 c=nopoll_conn_new_opts_with_ctrl(&ctx,opts,"host","80",NULL,NULL,NULL,NULL,&cancel_flag);
 assert(c && nopoll_conn_is_ok(c) && last_options==opts && options_freed==2);
 nopoll_conn_shutdown(c);nopoll_conn_close(c);
 assert(ctx.refs==1 && !ctx.registered && !live && socket_closes==1);return 0;
}
''')

    def test_ready_wait_cancellation_and_default_compatibility(self):
        self.run_c(PREAMBLE + implementation() + r'''
int main(void) {
 noPollConn c={.session=10};
 stage=5;
 assert(!nopoll_conn_wait_until_connection_ready_with_ctrl(&c,10,&cancel_flag));
 assert(sleeps==1 && now_us==10000);
 assert(nopoll_conn_wait_until_connection_ready(&c,10));
 assert(sleeps==2);
 cancel_flag=1;
 assert(!nopoll_conn_wait_until_connection_ready_with_ctrl(&c,10,&cancel_flag));
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
