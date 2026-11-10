"""Execute portal's real client gate/takeover/submit code with RTOS boundaries faked.

Catches dropping an already-associated phone, manual portal takeover, accepting
credentials after a stop claim, and reporting success before portal termination.
"""
import subprocess
import re
import tempfile
import unittest
from pathlib import Path
from tools.test_conversation_restore import function as body
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


def function(source, name):
    match = re.search(r'^(?:static )?(?:bool|int|void) ' + name + r'\([^;]+?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('missing function: ' + name)
    return source[match.start():source.index('{', match.start())] + body(source, name)


class OfflinePortalTest(unittest.TestCase):
    def test_worker_silent_takeover_keeps_saved_network_reconnect(self):
        source = (ROOT / 'src/middleware/netcfg_portal/app_netcfg_portal.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define APP_NETCFG_PORTAL_REASON_USER_REQUEST 2
#define TONE_ID_70 70
#define TONE_ID_71 71
#define TONE_ID_72 72
#define TONE_ID_65 65
#define TONE_ID_60 60
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static struct {int reason;bool active,stop_requested,credential_active,offline_closing,offline_takeover,transaction_busy;void *submitted;} s_ctx;
static unsigned ap_calls,ap_fail_on,waits,start_calls,exit_tones;
static bool autoconnect, healthy, recover_in_scan;
static unsigned station_stops;
bool sys_wifi_is_connected(void) {return healthy;}
#define APP_NETCFG_PORTAL_REASON_CONNECT_FAILED 1
#define sys_network_diag_mark(...) ((void)0)
static void lock(void){}
static void unlock(void){}
static void play_tone(int id){if(id==71)exit_tones++;}
static void led_provisioning(void){}
static void led_off(void){}
static void led_success(void){}
static void scan_and_cache(bool force){(void)force;if(recover_in_scan)healthy=true;}
static void sys_wifi_stop(void){++station_stops;}
static int softap_up(void){ap_calls++;return ap_calls==ap_fail_on?-1:0;}
static void softap_down(void){}
static int netcfg_portal_http_start(void){return 0;}
static void netcfg_portal_http_stop(void){}
static void sta_up(void){}
static bool sys_wifi_is_started(void){return false;}
static void sys_wifi_start(bool connect){start_calls++;autoconnect=connect;}
static void portal_mark_inactive(void){s_ctx.active=false;}
static void vTaskDelete(void *p){(void)p;}
static void vTaskDelay(unsigned ms){(void)ms;}
static int xSemaphoreTake(void *s,unsigned ms){(void)s;(void)ms;assert(++waits<3);s_ctx.credential_active=true;return 1;}
static bool try_connect(void){return false;}
static void app_ble_netcfg_complete(void){}
''' + function(source, 'portal_task') + r'''
int main(void){
 s_ctx.active=true;s_ctx.reason=APP_NETCFG_PORTAL_REASON_CONNECT_FAILED;
 recover_in_scan=true;portal_task(NULL);
 assert(!s_ctx.active && station_stops==0 && ap_calls==0 && start_calls==0);
 recover_in_scan=false;healthy=false;
 s_ctx.active=true;s_ctx.stop_requested=true;s_ctx.offline_takeover=true;
 portal_task(NULL);
 assert(!s_ctx.active && start_calls==1 && autoconnect && exit_tones==0);
 s_ctx.active=true;s_ctx.stop_requested=true;s_ctx.offline_takeover=false;
 s_ctx.offline_closing=false;start_calls=0;
 portal_task(NULL);
 assert(!s_ctx.active && start_calls==1 && !autoconnect && exit_tones==0);
 s_ctx.active=true;s_ctx.stop_requested=false;s_ctx.offline_closing=false;
 ap_calls=0;ap_fail_on=2;start_calls=0;
 portal_task(NULL);
 assert(!s_ctx.active && waits==1 && start_calls==1 && autoconnect);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp) / 'worker.c', Path(temp) / 'worker.elf'
            cfile.write_text(code, encoding='utf-8')
            built = subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-Wall', '-Werror',
                                    str(cfile), '-o', str(elf)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_takeover_guards_clients_and_credentials(self):
        source = (ROOT / 'src/middleware/netcfg_portal/app_netcfg_portal.c').read_text(encoding='utf-8')
        self.assertTrue('int app_netcfg_portal_offline_takeover(' in source,
                      'automatic portal cannot yet hand over safely to offline voice')
        struct = source[source.index('typedef struct {'):source.index('} portal_ctx_t;') + len('} portal_ctx_t;')]
        header = (ROOT / 'src/middleware/netcfg_portal/app_netcfg_portal.h').read_text(encoding='utf-8')
        status_struct = header[header.index('typedef struct {'):header.index('} app_netcfg_portal_status_t;') + len('} app_netcfg_portal_status_t;')]
        retry = ''.join(function(source, name) for name in ('portal_retry_saved', 'app_netcfg_portal_retry_saved', 'app_netcfg_portal_retry_saved_background'))
        if 'int app_netcfg_portal_retry_saved_on_wake(' in source:
            retry += function(source, 'app_netcfg_portal_retry_saved_on_wake')
        else:
            retry += 'int app_netcfg_portal_retry_saved_on_wake(void) {return app_netcfg_portal_retry_saved_background();}'
        if 'static void portal_retry_trace(' in source:
            retry = function(source, 'portal_retry_trace') + retry
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define NETCFG_PORTAL_SSID_MAX 32
#define NETCFG_PORTAL_PWD_MAX 64
#define NETCFG_PORTAL_SCAN_MAX 24
#define pdMS_TO_TICKS(n) (n)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define sys_network_diag_mark(...) ((void)0)
#define PORTAL_TASK_STACK 12288
#define PORTAL_TASK_PRIO 5
#define pdPASS 1
typedef unsigned StackType_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int netcfg_portal_ap_t;
typedef enum { APP_NETCFG_PORTAL_REASON_NO_CREDENTIAL,
APP_NETCFG_PORTAL_REASON_CONNECT_FAILED, APP_NETCFG_PORTAL_REASON_USER_REQUEST } app_netcfg_portal_reason_t;
typedef int event_module_t;
enum {EVENT_WIFI=1, EVENT_WIFI_AP_STARTED, EVENT_WIFI_AP_STOPPED,
EVENT_WIFI_AP_STA_ADD, EVENT_WIFI_AP_STA_DEL};
typedef struct { uint8_t sta_idx; uint8_t mac[6]; } event_ap_sta_add_param_t;
typedef struct { uint8_t sta_idx; uint8_t mac[6]; } event_ap_sta_del_param_t;
static unsigned delays, wakes;
unsigned xTaskGetTickCount(void) {return 10000;}
static int delay_mode;
static void lock(void) {}
static void unlock(void) {}
static void xSemaphoreGive(void *unused) {(void)unused; wakes++;}
static int xSemaphoreTake(void *unused, unsigned wait) {(void)unused;(void)wait;return 0;}
static bool sys_wifi_radio_suspended(void) {return false;}
static bool healthy;
bool sys_wifi_is_connected(void) {return healthy;}
static void portal_task(void *arg) {(void)arg;}
static int xTaskCreate(void (*fn)(void*),const char *name,unsigned stack,void *arg,
                       unsigned priority,TaskHandle_t *task) {
 (void)fn;(void)name;(void)stack;(void)arg;(void)priority;(void)task;return pdPASS;
}
static void vTaskDelay(unsigned ms);
''' + status_struct + struct + '\nstatic portal_ctx_t s_ctx;\n' + '\n'.join(function(source, name) for name in (
            'secure_zero', 'portal_mark_inactive', 'portal_wifi_event',
            'portal_offline_blocked_locked', 'app_netcfg_portal_offline_blocked',
            'app_netcfg_portal_is_active', 'app_netcfg_portal_offline_takeover',
            'netcfg_portal_submit', 'app_netcfg_portal_start', 'app_netcfg_portal_stop',
            'app_netcfg_portal_get_status')) + retry + r'''
static void vTaskDelay(unsigned ms) {
 assert(ms==100); delays++;
 if(delay_mode==1) portal_mark_inactive();
 if(delay_mode==2 || delay_mode==3) {
   event_ap_sta_add_param_t p={.sta_idx=3};
   portal_wifi_event(NULL,EVENT_WIFI,EVENT_WIFI_AP_STA_ADD,&p);
   if(delay_mode==3 && s_ctx.stop_requested) portal_mark_inactive();
 }
}
static void reset(void) {
 memset(&s_ctx,0,sizeof(s_ctx)); delays=0; wakes=0; delay_mode=1;
 s_ctx.inited=true; s_ctx.active=true; s_ctx.ap_up=true;
 s_ctx.client_events_ready=true; s_ctx.ap_clients_known=true;
 s_ctx.reason=APP_NETCFG_PORTAL_REASON_CONNECT_FAILED;
}
int main(void) {
 reset();s_ctx.active=false;healthy=true;
 assert(app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_CONNECT_FAILED)==-EAGAIN && !s_ctx.active);
 healthy=false;
 reset();s_ctx.active=false;
 assert(app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST)==0);
 app_netcfg_portal_status_t entry={0};app_netcfg_portal_get_status(&entry);
 assert(entry.start_caller!=0 && entry.reason==APP_NETCFG_PORTAL_REASON_USER_REQUEST);
 uintptr_t entry_caller=entry.start_caller;
 portal_mark_inactive();app_netcfg_portal_get_status(&entry);
 assert(!entry.active && entry.start_caller==entry_caller);
 s_ctx.scan_busy=true;
 assert(app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_CONNECT_FAILED)==-EBUSY);
 app_netcfg_portal_get_status(&entry);assert(entry.start_caller==entry_caller);
 reset(); s_ctx.ap_clients_known=false;
 assert(app_netcfg_portal_retry_saved_on_wake()==-EINPROGRESS && s_ctx.stop_requested);
 assert(wakes==1 && app_netcfg_portal_retry_saved_on_wake()==-EINPROGRESS && wakes==1);
 reset(); s_ctx.ap_clients=1;
 assert(app_netcfg_portal_retry_saved_on_wake()==-EINPROGRESS);
 reset(); s_ctx.reason=APP_NETCFG_PORTAL_REASON_USER_REQUEST;
 assert(app_netcfg_portal_retry_saved_on_wake()==-EBUSY && !s_ctx.stop_requested);
 reset(); s_ctx.credential_active=true;
 assert(app_netcfg_portal_retry_saved_on_wake()==-EBUSY && !s_ctx.stop_requested);
 reset(); s_ctx.transaction_busy=true;
 assert(app_netcfg_portal_retry_saved_on_wake()==-EBUSY && !s_ctx.stop_requested);
 reset(); s_ctx.scan_busy=true;
 assert(app_netcfg_portal_retry_saved_on_wake()==-EBUSY && !s_ctx.stop_requested);
 reset(); s_ctx.ap_up=false;
 assert(app_netcfg_portal_retry_saved_on_wake()==-EBUSY && !s_ctx.stop_requested);
 reset(); s_ctx.reason=APP_NETCFG_PORTAL_REASON_USER_REQUEST;
 assert(app_netcfg_portal_retry_saved_background()==-EBUSY && !s_ctx.stop_requested);
 assert(app_netcfg_portal_retry_saved()==-EINPROGRESS && s_ctx.stop_requested);
 reset(); assert(app_netcfg_portal_retry_saved_background()==-EINPROGRESS && s_ctx.stop_requested);
 reset(); assert(app_netcfg_portal_retry_saved()==-EINPROGRESS && s_ctx.active);
 assert(s_ctx.stop_requested && delays==0 && wakes==1);
 assert(app_netcfg_portal_retry_saved()==-EINPROGRESS && delays==0);
 assert(app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST)==-EINPROGRESS);
 portal_mark_inactive(); assert(app_netcfg_portal_retry_saved()==0);
 reset(); s_ctx.ap_clients=1;
 assert(app_netcfg_portal_retry_saved_background()==-EBUSY && !s_ctx.stop_requested);
 assert(app_netcfg_portal_retry_saved()==-EINPROGRESS && s_ctx.stop_requested);
 reset(); s_ctx.credential_active=true;
 assert(app_netcfg_portal_retry_saved()==-EBUSY && s_ctx.active && !s_ctx.stop_requested);
 reset(); s_ctx.ap_clients_known=false;
 assert(app_netcfg_portal_retry_saved_background()==-EBUSY && !s_ctx.stop_requested);
 assert(app_netcfg_portal_retry_saved()==-EINPROGRESS && s_ctx.stop_requested);
 reset(); assert(!app_netcfg_portal_offline_blocked());
 assert(app_netcfg_portal_offline_takeover()==0 && s_ctx.active && wakes==0);
 reset(); s_ctx.reason=APP_NETCFG_PORTAL_REASON_USER_REQUEST;
 assert(!app_netcfg_portal_offline_blocked());
 assert(app_netcfg_portal_offline_takeover()==0 && wakes==0);
 reset(); assert(app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST)==-EALREADY);
 assert(app_netcfg_portal_offline_takeover()==0);
 reset(); s_ctx.stop_requested=true; s_ctx.offline_takeover=true;
 assert(app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST)==-EALREADY);
 assert(!s_ctx.stop_requested && !s_ctx.offline_takeover && !app_netcfg_portal_offline_blocked());
 reset(); s_ctx.client_events_ready=false;
 assert(app_netcfg_portal_offline_takeover()==0);
 reset(); portal_wifi_event(NULL,EVENT_WIFI,EVENT_WIFI_AP_STOPPED,NULL);
 assert(app_netcfg_portal_offline_takeover()==0);
 reset(); s_ctx.ap_clients_known=false;
 assert(app_netcfg_portal_offline_takeover()==0);
 reset(); s_ctx.ap_up=false;
 assert(app_netcfg_portal_offline_takeover()==-EBUSY);
 reset(); event_ap_sta_add_param_t p={.sta_idx=4};
 portal_wifi_event(NULL,EVENT_WIFI,EVENT_WIFI_AP_STA_ADD,&p);
 portal_wifi_event(NULL,EVENT_WIFI,EVENT_WIFI_AP_STA_ADD,&p);
 assert(app_netcfg_portal_offline_takeover()==0 && s_ctx.active);
 event_ap_sta_del_param_t d={.sta_idx=4};
 portal_wifi_event(NULL,EVENT_WIFI,EVENT_WIFI_AP_STA_DEL,&d);
 assert(!app_netcfg_portal_offline_blocked());
 reset(); p.sta_idx=40;
 portal_wifi_event(NULL,EVENT_WIFI,EVENT_WIFI_AP_STA_ADD,&p);
 assert(app_netcfg_portal_offline_takeover()==0);
 reset(); assert(netcfg_portal_submit("known-ap","pw"));
 assert(app_netcfg_portal_offline_takeover()==-EBUSY);
 reset(); s_ctx.stop_requested=true;
 assert(!netcfg_portal_submit("late-ap","pw") && wakes==0);
 reset(); delay_mode=0;
 assert(app_netcfg_portal_stop()==-EBUSY && delays==100 && s_ctx.active);
 reset(); delay_mode=2;
 assert(app_netcfg_portal_offline_takeover()==0 && s_ctx.active && !s_ctx.stop_requested);
 reset(); s_ctx.offline_takeover=true; s_ctx.stop_requested=true; delay_mode=3;
 assert(app_netcfg_portal_stop()==0 && !s_ctx.offline_takeover);
 reset(); s_ctx.active=false;
 assert(app_netcfg_portal_offline_takeover()==0 && wakes==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp) / 'portal.c', Path(temp) / 'portal.elf'
            cfile.write_text(code, encoding='utf-8')
            built = subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-Wall', '-Werror',
                                    str(cfile), '-o', str(elf)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
