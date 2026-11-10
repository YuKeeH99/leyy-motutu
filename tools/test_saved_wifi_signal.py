import unittest
from tools import test_offline_action as support


class SavedWifiSignalTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_only_valid_scan_without_saved_match_means_no_signal(self):
        source = (support.ROOT / 'src/system/sys_wifi.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define SEARCH_BY_BSSID 1
#define SEARCH_BY_SSID 2
#define SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP 1
#define VOICE_MSG_WIFI_DISCONNECTED 1
#define LISA_LOGI(...) ((void)0)
#define TAG "test"
typedef struct {char ssid[33],bssid[18];} wifi_mgr_scan_info_t;
typedef wifi_mgr_scan_info_t wifi_mgr_sta_config_t;
static struct {bool started,connected,autoconnect_requested,autoconnect_enabled,saved_scan_absent;} s_wifi;
static bool suspended,exhausted,has_saved=true;
static int storage_error;
static unsigned disconnects;
static bool sys_wifi_radio_suspended(void) {return suspended;}
static bool sys_wifi_has_ap(void) {return has_saved;}
static bool sys_wifi_reconnect_saved_exhausted(void) {return exhausted && !suspended && !s_wifi.connected;}
static int wifi_mgr_storage_search_ap(wifi_mgr_sta_config_t *ap,int max,int mode,void *target) {
 if(storage_error)return -1;
 if(mode==SEARCH_BY_SSID && !strcmp(target,"saved")) {strcpy(ap->ssid,"saved");return 1;}
 return 0;
}
static void sys_network_diag_mark(const char*a,const char*b,int c,const char*d) {}
static void sys_wifi_set_connect_result(int result) {}
static void voice_msg_pub(int id,void *data,int size) {++disconnects;}
''' + support.c_function(source, 'wifi_mgr_scan_done_cb') +
                    support.c_function(source, 'sys_wifi_reconnect_saved_no_signal') + r'''
int main(void) {
 wifi_mgr_scan_info_t found[2]={{"other","00:11:22:33:44:55"},{"saved","00:11:22:33:44:66"}};
 s_wifi.started=true;s_wifi.autoconnect_enabled=true;
 wifi_mgr_scan_done_cb(found,2,NULL);exhausted=true;
 assert(!sys_wifi_reconnect_saved_no_signal() && !disconnects); /* Signal exists, even if auth failed. */
 wifi_mgr_scan_done_cb(found,1,NULL);exhausted=false;
 assert(!sys_wifi_reconnect_saved_no_signal()); /* A callback alone cannot end an in-flight cycle. */
 exhausted=true;assert(sys_wifi_reconnect_saved_no_signal());
 wifi_mgr_scan_done_cb(NULL,-1,NULL);assert(!sys_wifi_reconnect_saved_no_signal());
 wifi_mgr_scan_done_cb(NULL,0,NULL);assert(!sys_wifi_reconnect_saved_no_signal());
 storage_error=1;wifi_mgr_scan_done_cb(found,1,NULL);assert(!sys_wifi_reconnect_saved_no_signal());
 storage_error=0;wifi_mgr_scan_done_cb(found,0,NULL);assert(sys_wifi_reconnect_saved_no_signal());
 suspended=true;assert(!sys_wifi_reconnect_saved_no_signal());
 suspended=false;s_wifi.connected=true;assert(!sys_wifi_reconnect_saved_no_signal());
 return 0;
}
''')
