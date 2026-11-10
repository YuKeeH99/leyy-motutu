"""Awake network policy must not revert to DTIM between conversation rounds."""
import unittest
from tools import test_offline_action as support


class WifiAwakeConnectionTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_portal_radio_handoff_does_not_blacklist_saved_hotspot(self):
        source = (support.ROOT/'src/system/sys_wifi.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
static struct {bool started,autoconnect_requested,ready,connected;} s_wifi;
static int raw_disconnects,blacklisted,cleared;
static void sys_wifi_unregister_netdev(void) {}
static bool sys_wifi_radio_suspended(void) {return false;}
static void sys_wifi_clear_ip_addr(void) {cleared++;}
static void sys_wifi_stop_autoconnect(void) {}
static int wifi_mgr_sta_disconnect(bool async) {blacklisted++;return 0;}
static int wifi_sta_disconnect(void) {raw_disconnects++;return 0;}
''' + support.c_function(source,'sys_wifi_stop') + r'''
int main(void) {
 s_wifi.ready=true;s_wifi.started=true;s_wifi.connected=true;
 assert(sys_wifi_stop()==0);
 assert(raw_disconnects==1 && blacklisted==0);
 assert(!s_wifi.started && !s_wifi.connected && cleared==1);
 return 0;
}
''')

    def test_chat_exit_keeps_radio_realtime_until_actual_standby(self):
        source = (support.ROOT/'src/system/sys_wifi.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define SERVICE_V2_VOICE_STANDBY_ENTERING 2
#define SERVICE_V2_VOICE_STANDBY_ASLEEP 3
static struct {bool realtime_mode_requested,realtime_mode_applied,ready;} s_wifi;
static int standby,policy_calls;
static int service_v2_voice_standby_get_state(void) {return standby;}
static int sys_wifi_apply_power_policy(const char *why) {
 ++policy_calls;s_wifi.realtime_mode_applied=s_wifi.realtime_mode_requested;return 0;
}
''' + support.c_function(source,'sys_wifi_set_realtime_mode') + r'''
int main(void) {
 s_wifi.ready=true;
 sys_wifi_set_realtime_mode(true);assert(s_wifi.realtime_mode_requested);
 for(int i=0;i<1000;++i) {
  sys_wifi_set_realtime_mode(false);assert(s_wifi.realtime_mode_requested);
  sys_wifi_set_realtime_mode(true);
 }
 assert(policy_calls==1);
 standby=1;sys_wifi_set_realtime_mode(false);assert(s_wifi.realtime_mode_requested);
 standby=2;sys_wifi_set_realtime_mode(false);assert(!s_wifi.realtime_mode_requested);
 standby=3;sys_wifi_set_realtime_mode(false);assert(!s_wifi.realtime_mode_requested);
 standby=4;sys_wifi_set_realtime_mode(false);assert(s_wifi.realtime_mode_requested);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
