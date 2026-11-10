"""Execute the standby coordinator's timeout/accepted-wake arbitration in C."""
import unittest
from tools import test_offline_action as support
from tools.test_offline_action import c_function, ROOT


class StandbyAutoShutdownTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_idle_admission_and_explicit_upgrade(self):
        source = (ROOT/'src/middleware/power/power_manager.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef void *TaskHandle_t;
static TaskHandle_t shutdown_worker_task=(void*)1;
static bool shutdown_request_pending,shutdown_idle_only,usb;
static unsigned critical,notifications;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
static bool power_is_usb_plugged(void) {return usb;}
static void xTaskNotifyGive(TaskHandle_t worker) {assert(!critical);notifications++;}
''' + ''.join(c_function(source, name) for name in (
            'power_queue_shutdown', 'power_request_shutdown', 'power_request_idle_shutdown')) + r'''
int main(void) {
 usb=true;assert(!power_request_idle_shutdown());assert(!notifications);
 usb=false;assert(power_request_idle_shutdown());
 assert(shutdown_request_pending && shutdown_idle_only && notifications==1);
 usb=true;assert(power_request_shutdown()); /* User intent upgrades pending idle. */
 assert(shutdown_request_pending && !shutdown_idle_only && notifications==1);
 assert(!power_request_idle_shutdown());
 assert(!power_request_shutdown()); /* Duplicate explicit request stays coalesced. */
 shutdown_request_pending=false;
 assert(power_request_shutdown());assert(!shutdown_idle_only && notifications==2);
 assert(!critical);return 0;
}
''')

    def test_sleep_deadline_wake_race_and_retry(self):
        source = (ROOT/'apps/arcs-mini/services/service_v2_voice_standby.c').read_text(encoding='utf-8')
        functions = ''.join(c_function(source, name) for name in (
            'standby_publish_state', 'service_v2_voice_standby_note_activity',
            'service_v2_voice_standby_wake_pose',
            'service_v2_voice_standby_note_keyword',
            'service_v2_voice_standby_shutdown_cancelled', 'standby_poll_auto_shutdown'))
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include "service_v2_voice_standby_policy.h"
#define CONFIG_APP_V2_STANDBY_SHUTDOWN_TIMEOUT_MS 1200000U
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
#define LISA_LOGI(...) ((void)0)
#define STANDBY_NOTIFY_WAKE 32U
#define UBOOT_FEATURE_POWER_GUARD 1
#define pdPASS 1
#define eSetBits 1
static unsigned critical,now,requests,notifications;
static void *s_standby_task=(void*)1;
static service_v2_voice_standby_policy_t s_policy;
static service_v2_voice_standby_state_t s_public_state=SERVICE_V2_VOICE_STANDBY_ACTIVE;
static uint32_t s_sleep_started_ms;
static bool s_auto_shutdown_queued,s_wake_pending,s_manual_requested;
static uint8_t s_requested_pose;
static bool eligible=true,usb,guard=true,request_ok=true,wake_during_check;
static bool foreground;
static bool standby_has_foreground_work(void) {return foreground;}
static uint32_t standby_now_ms(void) {return now;}
static bool power_is_usb_plugged(void) {return usb;}
static bool uboot_features_has(int feature) {return guard;}
void service_v2_voice_standby_note_activity(void);
static bool standby_is_eligible(void) {
 assert(!critical);
 if(wake_during_check) {wake_during_check=false;service_v2_voice_standby_note_activity();}
 return eligible;
}
static bool power_request_idle_shutdown(void) {assert(critical);++requests;return request_ok;}
static bool power_request_shutdown(void) {return power_request_idle_shutdown();}
static int xTaskNotify(void *task,unsigned bits,int action) {++notifications;return pdPASS;}
''' + functions + r'''
static void enter_sleep(uint32_t t) {
 s_policy.state=SERVICE_V2_VOICE_STANDBY_ENTERING;standby_publish_state();
 now=t;s_policy.state=SERVICE_V2_VOICE_STANDBY_ASLEEP;standby_publish_state();
}
int main(void) {
 /* Awake idle must power off without first entering standby. */
 now=1199999;standby_poll_auto_shutdown();assert(!requests);
 now++;standby_poll_auto_shutdown();assert(requests==1 && s_auto_shutdown_queued);
 standby_poll_auto_shutdown();assert(requests==1);
 /* Active work spanning the deadline starts a fresh idle period at its end. */
 service_v2_voice_standby_shutdown_cancelled();
 foreground=true;now+=1200000;standby_poll_auto_shutdown();assert(requests==1);
 foreground=false;now+=1199999;standby_poll_auto_shutdown();assert(requests==1);
 now++;standby_poll_auto_shutdown();assert(requests==2);
 requests=1;
 service_v2_voice_standby_shutdown_cancelled();
 now+=1199999;standby_poll_auto_shutdown();assert(requests==1);
 service_v2_voice_standby_note_keyword();now+=1;
 standby_poll_auto_shutdown();assert(requests==1);
 requests=0;
 s_policy.state=SERVICE_V2_VOICE_STANDBY_ENTER_PENDING;standby_publish_state();
 standby_poll_auto_shutdown();assert(!requests);
 enter_sleep(3000000);now+=1199999;standby_poll_auto_shutdown();assert(!requests);
 now++;eligible=false;standby_poll_auto_shutdown();assert(!requests);
 eligible=true;wake_during_check=true;standby_poll_auto_shutdown();
 assert(!requests && s_wake_pending && notifications==1);
 s_wake_pending=false;s_policy.state=SERVICE_V2_VOICE_STANDBY_RESUMING;standby_publish_state();
 standby_poll_auto_shutdown();assert(!requests);
 enter_sleep(5000000);now+=1200000;
 usb=true;guard=false;standby_poll_auto_shutdown();assert(!requests);
 guard=true;now+=2400000;standby_poll_auto_shutdown();assert(!requests);
 usb=false;standby_poll_auto_shutdown();assert(!requests); /* Unplug starts fresh. */
 now+=1199999;standby_poll_auto_shutdown();assert(!requests);
 now++;
 usb=false;request_ok=false;standby_poll_auto_shutdown();assert(requests==1 && !s_auto_shutdown_queued);
 request_ok=true;standby_poll_auto_shutdown();assert(requests==2 && s_auto_shutdown_queued);
 for(int i=0;i<100;++i) standby_poll_auto_shutdown();assert(requests==2);
 /* A returned/cancelled power request restores the timer and can retry. */
 service_v2_voice_standby_shutdown_cancelled();assert(!s_auto_shutdown_queued);
 now+=1199999;standby_poll_auto_shutdown();assert(requests==2);
 now++;service_v2_voice_standby_note_keyword();
 standby_poll_auto_shutdown();assert(requests==2);
 assert(s_policy.state==SERVICE_V2_VOICE_STANDBY_ASLEEP); /* No early peripheral wake. */
 /* Next sleep gets its full duration, including across the tick wrap. */
 enter_sleep(UINT32_MAX-600000U);now+=1199999U;
 standby_poll_auto_shutdown();assert(requests==2);
 now++;usb=true;guard=true;standby_poll_auto_shutdown();assert(requests==2);
 usb=false;now+=1200000;standby_poll_auto_shutdown();assert(requests==3);
 enter_sleep(100);now+=1200000;
 assert(service_v2_voice_standby_wake_pose(false)==0);
 standby_poll_auto_shutdown();assert(requests==3 && s_wake_pending && s_requested_pose==1);
 assert(!critical);
 return 0;
}
''')

    def test_final_shutdown_usb_hotplug_guard(self):
        source = (ROOT/'src/middleware/power/power_manager.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define UBOOT_FEATURE_POWER_GUARD 1
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define POWER_EN_PIN 0
#define LISA_GPIO_CONFIG_OUTPUT_HIGH 1
static void *power_en_dev;
static bool usb,guard=true,shutdown_idle_only=true,gate,plug_in_callback,plug_in_latch;
static unsigned cut,restored,stage0,cancelled;
static bool power_is_usb_plugged(void) {return usb;}
static bool uboot_features_has(int feature) {return guard;}
static bool power_shutdown_gate_try_enter(void) {assert(!gate);return gate=true;}
static void power_shutdown_gate_leave(void) {gate=false;}
static void uboot_shutdown_request(void) {stage0++;}
static int power_latch_set(bool on) {
 if(on)restored++;else {cut++;if(plug_in_latch)usb=true;}return 0;
}
static int lisa_gpio_configure(void *d,int pin,int mode) {return 0;}
static void on_cancel(void) {cancelled++;}
static void (*shutdown_cancelled_cb)(void)=on_cancel;
static void on_shutdown(void) {if(plug_in_callback)usb=true;}
static void (*shutdown_cb)(void)=on_shutdown;
''' + c_function(source, 'power_restore_cancelled_shutdown') +
            c_function(source, 'power_shutdown') + r'''
int main(void) {
 usb=true;power_shutdown();assert(restored==1 && cancelled==1 && !cut && !stage0);
 usb=false;plug_in_callback=true;power_shutdown();
 assert(restored==2 && cancelled==2 && !cut && !stage0 && !gate);
 /* An explicit request on new boot still enters soft shutdown on USB. */
 shutdown_idle_only=false;power_shutdown();assert(stage0==1 && !cut && !gate);
 shutdown_idle_only=true;usb=false;plug_in_callback=false;plug_in_latch=true;
 power_shutdown();assert(cut==1 && restored==3 && cancelled==3 && !gate);
 usb=false;plug_in_latch=false;power_shutdown();assert(cut==2 && restored==3 && !gate);
 return 0;
}
''')

    def test_worker_consumes_wake_before_checking_shutdown(self):
        source = (ROOT/'apps/arcs-mini/services/service_v2_voice_standby.c').read_text(encoding='utf-8')
        task = c_function(source, 'standby_task')
        self.assertLess(task.index('standby_process_notifications(notifications)'),
                        task.index('standby_poll_auto_shutdown()'))
        process = c_function(source, 'standby_process_notifications')
        self.assertIn('s_wake_pending = false', process)

    def test_natural_keyword_and_cancelled_shutdown_are_wired(self):
        offline = (ROOT/'apps/arcs-mini/services/service_offline_action.c').read_text(encoding='utf-8')
        ingress = c_function(offline, 'service_offline_action_handle_result')
        self.assertIn('service_v2_voice_standby_note_keyword()', ingress)
        main = (ROOT/'apps/arcs-mini/main.c').read_text(encoding='utf-8')
        self.assertIn('power_set_shutdown_return_callback(app_shutdown_returned)', main)
        returned = c_function(main, 'app_shutdown_returned')
        self.assertIn('service_offline_action_shutdown_cancelled()', returned)
        self.assertIn('service_v2_voice_standby_shutdown_cancelled()', returned)
        source = (ROOT/'src/middleware/power/power_manager.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define pdTRUE 1
#define portMAX_DELAY 0
static bool shutdown_request_pending=true;
static bool shutdown_idle_only,usb,plug_during_prepare;
static bool upgrade_after_usb_check;
static void *shutdown_worker_task=(void*)1;
static unsigned retried;
static unsigned stopped,prepared,returned,waits;
static jmp_buf done;
static void xTaskNotifyGive(void *worker) {retried++;}
static bool power_is_usb_plugged(void) {
 if(upgrade_after_usb_check) {upgrade_after_usb_check=false;shutdown_idle_only=false;}
 return usb;
}
static void prepare(void) {prepared++;if(plug_during_prepare)usb=true;}
static void returned_cb(void) {returned++;}
static void (*shutdown_prepare_cb)(void)=prepare;
static void (*shutdown_return_cb)(void)=returned_cb;
static void ulTaskNotifyTake(int clear,int timeout) {if(waits++) longjmp(done,1);}
static void power_shutdown(void) {stopped++;}
''' + c_function(source, 'power_shutdown_worker') + r'''
int main(void) {
 if(!setjmp(done)) power_shutdown_worker(NULL);
 assert(prepared==1 && stopped==1 && returned==1 && !shutdown_request_pending);
 waits=0;usb=shutdown_idle_only=shutdown_request_pending=true;
 if(!setjmp(done)) power_shutdown_worker(NULL);
 assert(prepared==1 && stopped==1 && returned==2 && !shutdown_idle_only);
 waits=0;usb=false;plug_during_prepare=shutdown_idle_only=shutdown_request_pending=true;
 if(!setjmp(done)) power_shutdown_worker(NULL);
 assert(prepared==2 && stopped==1 && returned==3);
 waits=0;shutdown_request_pending=true;shutdown_idle_only=false;
 if(!setjmp(done)) power_shutdown_worker(NULL);
 assert(prepared==3 && stopped==2 && returned==4); /* Explicit request unaffected. */
 waits=0;shutdown_idle_only=shutdown_request_pending=upgrade_after_usb_check=true;
 if(!setjmp(done)) power_shutdown_worker(NULL);
 assert(retried==1 && shutdown_request_pending && !shutdown_idle_only);
 waits=0;
 if(!setjmp(done)) power_shutdown_worker(NULL);
 assert(prepared==4 && stopped==3 && !shutdown_request_pending && !shutdown_idle_only);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
