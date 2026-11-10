"""Business timeouts must not manufacture an offline/online network edge."""
import unittest
from tools import test_offline_action as support


class NetworkStatusFlapTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_repeated_refresh_keeps_inflight_probe_owner_and_one_pending_wakeup(self):
        source = (support.ROOT / 'src/system/sys_network_probe.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
static void *network_probe_sem;
static uint32_t s_probe_request=17;
static unsigned pending,gives;
static int lisa_semaphore_give(void *s) {
 assert(s);gives++;if(pending)return -1;pending=1;return 0;
}
''' + support.c_function(source, 'network_probe_refresh') + r'''
int main(void) {
 assert(network_probe_refresh()==-1 && !gives);
 network_probe_sem=&pending;
 for(int i=0;i<100;i++) assert(network_probe_refresh()==0);
 assert(s_probe_request==17 && pending==1 && gives==100);
 pending=0;assert(network_probe_refresh()==0 && pending==1);
}
''')

    def test_business_failure_only_requests_verification(self):
        source = (support.ROOT / 'src/system/sys_network_manager.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define SYS_NETWORK_BEARER_NONE 0
#define SYS_NETWORK_BEARER_WIFI 1
#define VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL 2
#define sys_network_diag_mark(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
static struct {bool inited,switching,network_connected;int active_bearer;} s_mgr;
static unsigned probes,events,syncs;
static int network_probe_refresh(void) {probes++;return 0;}
static void sys_network_sync_app_data(void) {syncs++;}
static int voice_msg_pub(int id,void *p,unsigned n) {events++;return 0;}
''' + support.c_function(source, 'sys_network_report_transport_failure') + r'''
int main(void) {
 sys_network_report_transport_failure();assert(!probes);
 s_mgr.inited=true;s_mgr.active_bearer=1;s_mgr.network_connected=true;
 for(int i=0;i<20;i++) {
  sys_network_report_transport_failure();
  assert(s_mgr.network_connected && !events && !syncs);
 }
 assert(probes==20);
 s_mgr.network_connected=false;sys_network_report_transport_failure();
 assert(!s_mgr.network_connected && probes==21 && !events);
 s_mgr.switching=true;sys_network_report_transport_failure();assert(probes==21);
 s_mgr.switching=false;s_mgr.active_bearer=0;
 sys_network_report_transport_failure();assert(probes==21);
}
''')


if __name__ == '__main__':
    unittest.main()
