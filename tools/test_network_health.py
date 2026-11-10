import unittest
from tools import test_offline_action as support


class NetworkHealthTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_tcp_probe_checks_socket_error_and_releases_resources(self):
        source = (support.ROOT / 'src/system/sys_network_health.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define AF_INET 2
#define SOCK_STREAM 1
#define FD_SETSIZE 64
#define F_SETFL 1
#define O_NONBLOCK 2
#define SOL_SOCKET 1
#define SO_ERROR 1
#define FIONBIO 3
#define SYS_NETWORK_BEARER_WIFI 1
typedef struct {int active_bearer;bool wifi_connected,switching;} sys_network_status_t;
typedef int socklen_t;
typedef int fd_set;
#define FD_ZERO(s) (*(s)=0)
#define FD_SET(fd,s) (*(s)=fd)
struct timeval {long tv_sec,tv_usec;};
struct addrinfo {int ai_family,ai_socktype,ai_protocol,ai_addrlen;void *ai_addr;};
static struct addrinfo address;
static int mode,closed,freed,live,file_closed,ioctls,waits;
static int sys_network_get_status(sys_network_status_t *s) {
 s->active_bearer=mode==9?2:1;s->wifi_connected=mode!=10;s->switching=mode==11;return 0;
}
static const char *lingxin_server_config_cloud_host(void) {return "business.example";}
static int lingxin_server_config_cloud_port(void) {return 443;}
static int getaddrinfo(const char *host,const char *port,struct addrinfo *h,struct addrinfo **a) {
 assert(!strcmp(host,"business.example") && !strcmp(port,"443"));*a=&address;return mode==1?-1:0;
}
static int socket(int a,int b,int c) {return mode==2?-1:3;}
/* Match this target: fcntl is libnosys; close addresses the file table. */
static int fcntl(int a,int b,int c) {errno=88;return -1;}
static int connect(int a,void *b,int c) {errno=mode==8?ENETUNREACH:EINPROGRESS;return mode==4?0:-1;}
static int select(int n,void *r,fd_set *w,void *e,struct timeval *t) {assert(t->tv_sec==3);return mode==5?0:1;}
static int getsockopt(int fd,int a,int b,int *error,socklen_t *size) {*error=mode==6?1:0;return mode==7?-1:0;}
static void close(int fd) {file_closed++;}
static void freeaddrinfo(struct addrinfo *a) {freed++;}
/* lwIP descriptors deliberately differ from SAL/file descriptors. */
static int lwip_getaddrinfo(const char *h,const char *p,struct addrinfo *i,struct addrinfo **a) {
 return getaddrinfo(h,p,i,a);
}
static void lwip_freeaddrinfo(struct addrinfo *a) {freeaddrinfo(a);}
static int lwip_socket(int a,int b,int c) {if(mode==2)return -1;live++;return 17;}
static int lwip_ioctl(int fd,long cmd,int *value) {
 assert(fd==17 && cmd==FIONBIO && *value==1);ioctls++;return mode==3?-1:0;
}
static int lwip_connect(int fd,void *a,int len) {assert(fd==17);return connect(fd,a,len);}
static int lwip_select(int n,void *r,fd_set *w,void *e,struct timeval *t) {
 assert(n==18 && *w==17);waits++;return select(n,r,w,e,t);
}
static int lwip_getsockopt(int fd,int a,int b,int *error,socklen_t *size) {
 assert(fd==17);return getsockopt(fd,a,b,error,size);
}
static int lwip_close(int fd) {assert(fd==17 && live==1);live--;closed++;return 0;}
''' + support.c_function(source, 'sys_network_business_reachable') + r'''
int main(void) {
 for(int repetition=0;repetition<100;repetition++) for(mode=0;mode<12;mode++) {
  closed=freed=file_closed=ioctls=waits=0;
  assert(sys_network_business_reachable()==(mode==0 || mode==4));
  assert(closed==(mode==1 || mode==2 || mode>=9?0:1));
  assert(freed==(mode==1 || mode>=9?0:1));
  assert(!live && !file_closed);
  assert(ioctls==(mode==1 || mode==2 || mode>=9?0:1));
  assert(waits==(mode==0 || mode==5 || mode==6 || mode==7?1:0));
 }
 return 0;
}
''')
