"""Exercise production portal recovery using bounded network fault injection.

No real credentials, sockets, radio or flash are used by these tests.
"""
import re
import unittest

from tools.test_lingxin_round_races import COMMON, run_c
from tools.test_offline_portal import ROOT, function


SOURCE = ROOT / "src/middleware/netcfg_portal/app_netcfg_portal.c"


class PortalFailureRuntimeTest(unittest.TestCase):
    def test_credentials_retry_cancel_and_cleanup(self):
        source = SOURCE.read_text(encoding="utf-8")
        constants = "\n".join(re.findall(
            r"^#define (?:NETCFG_PORTAL_(?:SSID|PWD)_MAX|PORTAL_CONNECT_\w+)[^\n]*",
            source, re.M))
        run_c(COMMON + constants + r'''
#define NETCFG_PORTAL_SSID_MAX 32
#define NETCFG_PORTAL_PWD_MAX 64
#define pdMS_TO_TICKS(n) (n)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
enum {SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD=1,SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP,
 SYS_WIFI_CONNECT_RESULT_FAIL_IP,SYS_WIFI_CONNECT_RESULT_FAIL_OTHER};
typedef int sys_wifi_connect_result_t;
static struct {char req_ssid[33],req_pwd[65];bool stop_requested,transaction_busy;} s_ctx;
static int scenario, attempts, saved_stops, ap_stops, http_stops, resets, locks;
static unsigned delays;
static void lock(void){assert(!locks);locks=1;}
static void unlock(void){assert(locks);locks=0;}
static void secure_zero(void *p,size_t size){memset(p,0,size);}
static void softap_down(void){ap_stops++;}
static void netcfg_portal_http_stop(void){http_stops++;}
static bool sta_up(void){return scenario!=1;}
static int sys_wifi_start(bool auto_connect){assert(!auto_connect);return scenario==2?-1:0;}
static void sys_wifi_reconnect_saved_once_stop(void){saved_stops++;}
static void sys_wifi_reset_connect_result(void){resets++;}
static int sys_network_connect_wifi(const char *ssid,const char *pwd,const char *bssid){
 assert(!strcmp(ssid,"test-only") && !strcmp(pwd,"test-pass") && !bssid);
 assert(s_ctx.transaction_busy && !locks);attempts++;
 if(scenario==8)s_ctx.stop_requested=true;
 return scenario==3?-1:0;
}
static bool sys_wifi_is_connected(void){
 return scenario==0 || (scenario==7 && attempts==PORTAL_CONNECT_ATTEMPTS);
}
static sys_wifi_connect_result_t sys_wifi_get_connect_result(void){
 return scenario==4?SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD:
        scenario==5?SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP:
        scenario==7?SYS_WIFI_CONNECT_RESULT_FAIL_IP:0;
}
static void vTaskDelay(unsigned ms){
 assert(!locks);delays+=ms;
 assert(delays <= PORTAL_CONNECT_ATTEMPTS*(PORTAL_CONNECT_TIMEOUT_MS+PORTAL_CONNECT_RETRY_MS));
}
''' + function(source, "try_connect") + r'''
int main(void){
 for(unsigned repetition=0;repetition<100;repetition++)for(scenario=0;scenario<=8;scenario++){
  memset(&s_ctx,0,sizeof(s_ctx));strcpy(s_ctx.req_ssid,"test-only");strcpy(s_ctx.req_pwd,"test-pass");
  attempts=saved_stops=ap_stops=http_stops=resets=delays=0;
  bool success=try_connect();
  assert(success==(scenario==0||scenario==7));
  assert(!locks && !s_ctx.transaction_busy);
  for(unsigned i=0;i<sizeof(s_ctx.req_pwd);i++)assert(!s_ctx.req_pwd[i]);
  assert(ap_stops==1 && http_stops==1);
  unsigned expected=(scenario==1||scenario==2)?0:(scenario==0||scenario==8)?1:PORTAL_CONNECT_ATTEMPTS;
  assert(attempts==expected && resets==expected);
  assert(saved_stops==((scenario==1||scenario==2)?0:expected+1));
 }
}
''')

    def test_portal_recovers_after_ap_http_and_credential_failures(self):
        source = SOURCE.read_text(encoding="utf-8")
        run_c(COMMON + r'''
#define APP_NETCFG_PORTAL_REASON_USER_REQUEST 2
#define APP_NETCFG_PORTAL_REASON_CONNECT_FAILED 1
#define TONE_ID_70 70
#define TONE_ID_72 72
#define TONE_ID_65 65
#define TONE_ID_60 60
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define sys_network_diag_mark(...) ((void)0)
static struct {int reason;bool active,stop_requested,credential_active,offline_closing,offline_takeover,transaction_busy;void *submitted;} s_ctx;
static unsigned scenario,ap_calls,http_calls,ap_downs,http_stops,attempts,restarts,waits,success_tones,fail_tones,deleted;
static bool reconnect;
static void lock(void){}
static void unlock(void){}
static bool sys_wifi_is_connected(void){return false;}
static void play_tone(int id){if(id==72)success_tones++;if(id==60||id==65)fail_tones++;}
static void led_provisioning(void){}
static void led_off(void){}
static void led_success(void){}
static void scan_and_cache(bool force){}
static void sys_wifi_stop(void){}
static int softap_up(void){ap_calls++;return scenario==0?-1:0;}
static void softap_down(void){ap_downs++;}
static int netcfg_portal_http_start(void){http_calls++;return scenario==1||(scenario==3&&http_calls==2)?-1:0;}
static void netcfg_portal_http_stop(void){http_stops++;}
static bool sta_up(void){return true;}
static bool sys_wifi_is_started(void){return false;}
static void sys_wifi_start(bool auto_connect){restarts++;reconnect=auto_connect;}
static void portal_mark_inactive(void){s_ctx.active=false;}
static void vTaskDelete(void *p){deleted++;}
static void vTaskDelay(unsigned ms){}
static int xSemaphoreTake(void *s,unsigned ms){assert(++waits<=4);s_ctx.credential_active=true;return 1;}
static bool try_connect(void){attempts++;return scenario==2&&attempts==3;}
static void app_ble_netcfg_complete(void){}
''' + function(source, "portal_task") + r'''
int main(void){
 for(scenario=0;scenario<4;scenario++){
  memset(&s_ctx,0,sizeof(s_ctx));s_ctx.active=true;s_ctx.reason=APP_NETCFG_PORTAL_REASON_USER_REQUEST;
  ap_calls=http_calls=ap_downs=http_stops=attempts=restarts=waits=success_tones=fail_tones=deleted=0;
  reconnect=false;portal_task(NULL);
  assert(!s_ctx.active && deleted==1 && restarts==1 && reconnect);
  assert(ap_downs==1 && http_stops==1);
  assert(success_tones==(scenario==2));
  assert(attempts==(scenario==2?3:scenario==3?1:0));
  assert(fail_tones==(scenario==2?2:1));
 }
}
''')


if __name__ == "__main__":
    unittest.main()
