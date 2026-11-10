"""Execute mute timing, alert priority and output gates on the real RGB policy."""
import unittest
from tools import test_rgb_eyes as eyes
from tools.test_offline_action import c_function, ROOT


class MuteIndicatorTest(unittest.TestCase):
    compile_run = eyes.EyesTest.compile_run

    def test_double_flash_priority_unmute_and_poll_does_not_restart(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_effect_policy.h"
/* Baseline without this feature must fail behaviorally, not at link time. */
__attribute__((weak)) void service_v2_rgb_effect_policy_mute(service_v2_rgb_effect_policy_t *p, bool muted) {(void)p;(void)muted;}
static service_v2_rgb_effect_policy_t p;
static service_v2_rgb_color_t pixels[5];
static void purple(bool on) {
 assert(service_v2_rgb_effect_policy_alert_frame(&p,pixels));
 for(unsigned i=0;i<5;i++) {
  if(on && (i==1 || i==3)) assert(pixels[i].red>0 && pixels[i].blue>pixels[i].red && pixels[i].green==0);
  else assert(!pixels[i].red && !pixels[i].green && !pixels[i].blue);
 }
}
int main(void) {
 service_v2_rgb_effect_policy_init(&p,0);
 service_v2_rgb_effect_policy_mute(&p,true); purple(true);
 service_v2_rgb_effect_policy_tick(&p,99); purple(true);
 service_v2_rgb_effect_policy_tick(&p,1); purple(false);
 service_v2_rgb_effect_policy_mute(&p,true); purple(false);
 service_v2_rgb_effect_policy_tick(&p,100); purple(true);
 service_v2_rgb_effect_policy_tick(&p,99); purple(true);
 service_v2_rgb_effect_policy_tick(&p,1); purple(false);
 service_v2_rgb_effect_policy_tick(&p,1699); purple(false);
 service_v2_rgb_effect_policy_tick(&p,1); purple(true);
 service_v2_rgb_effect_policy_event(&p,SERVICE_V2_RGB_EVENT_TTS_PLAYING); purple(true);
 service_v2_rgb_effect_policy_battery(&p,20,SERVICE_V2_RGB_POWER_DISCHARGING);
 assert(service_v2_rgb_effect_policy_alert_frame(&p,pixels) && pixels[1].red && !pixels[1].blue);
 service_v2_rgb_effect_policy_network(&p,false);
 assert(service_v2_rgb_effect_policy_alert_frame(&p,pixels) && pixels[0].red && pixels[0].green);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_ACTIVE);
 assert(service_v2_rgb_effect_policy_alert_frame(&p,pixels) && !pixels[0].red && pixels[1].green);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_SUCCESS);
 assert(service_v2_rgb_effect_policy_alert_frame(&p,pixels) && !pixels[1].red && pixels[1].green);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_INACTIVE);
 service_v2_rgb_effect_policy_network(&p,true);
 service_v2_rgb_effect_policy_battery(&p,50,SERVICE_V2_RGB_POWER_CHARGING); purple(true);
 service_v2_rgb_effect_policy_event(&p,SERVICE_V2_RGB_EVENT_SESSION_FINISHED);
 service_v2_rgb_effect_policy_mute(&p,false);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,pixels));
 service_v2_rgb_color_t color;
 service_v2_rgb_effect_policy_color(&p,50,&color);
 assert(color.red==100 && !color.green && !color.blue); /* Charging restores. */
 service_v2_rgb_effect_policy_tick(&p,150);
 service_v2_rgb_effect_policy_mute(&p,true); purple(true); /* Re-entry starts first flash. */
 return 0;
}
''', policy=True)

    def test_mute_replaces_show_but_obeys_sleep_and_manual_ownership(self):
        source = (ROOT/'apps/arcs-mini/services/service_v2_rgb_effect.c').read_text(encoding='utf-8')
        self.compile_run(r'''
#include <assert.h>
#include <string.h>
#include "service_v2_rgb_effect_policy.h"
__attribute__((weak)) void service_v2_rgb_effect_policy_mute(service_v2_rgb_effect_policy_t *p, bool muted) {(void)p;(void)muted;}
typedef struct {int owner,count,brightness_percent;} service_v2_rgb_status_t;
typedef int service_v2_rgb_show_kind_t;
#define SERVICE_V2_RGB_OWNER_SYSTEM 1
#define SERVICE_V2_RGB_SHOW_NONE 0
#define portTICK_PERIOD_MS 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static bool s_output_enabled=true,s_first_output_logged;
static unsigned s_output_failure_count,frames;
static int owner=1;
static struct {unsigned started_ms;} s_show;
static service_v2_rgb_color_t actual[5];
static int service_v2_actuators_rgb_get_status(service_v2_rgb_status_t *s) {s->owner=owner;return 0;}
static int service_v2_actuators_rgb_system_frame(service_v2_rgb_color_t *p) {memcpy(actual,p,sizeof(actual));frames++;return 0;}
static int service_v2_actuators_rgb_set(int r,int g,int b) {assert(0);return 0;}
static int service_v2_actuators_rgb_off(void) {return 0;}
static unsigned xTaskGetTickCount(void) {return 1000;}
static int service_v2_rgb_show_current(void *s,unsigned now) {assert(0);return 1;}
static void service_v2_rgb_show_frame(int kind,unsigned ms,service_v2_rgb_color_t *p) {assert(0);}
''' + c_function(source,'rgb_effect_apply') + r'''
int main(void) {
 service_v2_rgb_effect_policy_t p;service_v2_rgb_effect_policy_init(&p,1);
 service_v2_rgb_effect_policy_mute(&p,true);rgb_effect_apply(&p,0);
 assert(frames==1 && actual[1].blue && actual[3].blue && !actual[0].blue);
 s_output_enabled=false;rgb_effect_apply(&p,0);assert(frames==1);
 s_output_enabled=true;owner=2;rgb_effect_apply(&p,0);assert(frames==1);
 return 0;
}
''', policy=True)
