"""Run the real portal scan/cache function with synthetic driver results.

No radio is used. Check band filtering before SSID deduplication, plus the
existing sorting and failed-scan cache behavior that the phone page relies on.
"""
import unittest

from tools.test_lingxin_round_races import COMMON, run_c
from tools.test_offline_portal import ROOT, function


class PortalScanBandRuntimeTest(unittest.TestCase):
    def test_only_24ghz_channels_enter_the_cache_before_deduplication(self):
        source = (ROOT / "src/middleware/netcfg_portal/app_netcfg_portal.c").read_text(encoding="utf-8")
        run_c(COMMON + r'''
#include <errno.h>
#define NETCFG_PORTAL_SCAN_MAX 24
#define NETCFG_PORTAL_SSID_MAX 32
#define pdMS_TO_TICKS(n) (n)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGI(...) ((void)0)
typedef struct {char ssid[32],bssid[18];int channel,rssi,encryption_mode;} wifi_mgr_scan_info_t;
typedef struct {char ssid[33],bssid[18];int rssi;bool secured;} netcfg_portal_ap_t;
static struct {netcfg_portal_ap_t aps[24];int ap_count;bool ap_cache_valid;} s_ctx;
static wifi_mgr_scan_info_t samples[] = {
 {"dual-band", "00:00:00:00:00:01",36,-20,1},
 {"dual-band", "00:00:00:00:00:02",6,-70,1},
 {"dual-band", "00:00:00:00:00:03",11,-50,0},
 {"five-only", "00:00:00:00:00:04",149,-10,1},
 {"bad-zero", "00:00:00:00:00:05",0,-10,1},
 {"bad-negative", "00:00:00:00:00:06",-1,-10,1},
 {"channel-one", "00:00:00:00:00:07",1,-60,1},
 {"channel-fourteen", "00:00:00:00:00:08",14,-40,1},
 {"", "00:00:00:00:00:09",6,-10,1},
 {"Name mentions 5G", "00:00:00:00:00:10",3,-80,1},
 {"bad-fifteen", "00:00:00:00:00:11",15,-10,1},
};
static int scan_mode,scan_calls,delays,depth;
static bool sta_up(void){return true;}
static void lock(void){assert(!depth);depth++;}
static void unlock(void){assert(depth==1);depth--;}
static void vTaskDelay(unsigned delay){assert(!depth);delays+=delay;}
static bool encryption_is_open(int mode){return mode==0;}
static int wifi_mgr_scan_ap(wifi_mgr_scan_info_t *out,unsigned max,bool async){
 assert(!depth && !async && max==24);scan_calls++;
 if(scan_mode==1)return -EIO;
 if(scan_mode==2)return 0;
 memcpy(out,samples,sizeof(samples));return sizeof(samples)/sizeof(samples[0]);
}
''' + function(source, "scan_and_cache") + r'''
int main(void){
 assert(scan_and_cache(false)==0 && s_ctx.ap_cache_valid && s_ctx.ap_count==4);
 assert(!strcmp(s_ctx.aps[0].ssid,"channel-fourteen") && s_ctx.aps[0].rssi==-40);
 assert(!strcmp(s_ctx.aps[1].ssid,"dual-band") && s_ctx.aps[1].rssi==-50);
 assert(!s_ctx.aps[1].secured && !strcmp(s_ctx.aps[1].bssid,"00:00:00:00:00:03"));
 assert(!strcmp(s_ctx.aps[2].ssid,"channel-one") && s_ctx.aps[2].secured);
 assert(!strcmp(s_ctx.aps[3].ssid,"Name mentions 5G"));
 scan_mode=1;
 assert(scan_and_cache(false)==-EIO && s_ctx.ap_count==4 && scan_calls==2 && !delays);
 assert(!strcmp(s_ctx.aps[1].ssid,"dual-band"));
 scan_mode=2;
 assert(scan_and_cache(false)==0 && !s_ctx.ap_count && scan_calls==3 && !depth);
 return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
