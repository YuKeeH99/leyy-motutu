"""Exercise physical masks and exact blink boundaries on the real policy."""
import unittest
from tools import test_rgb_eyes as eyes
from tools.test_offline_action import c_function, ROOT


class NetworkAlertTest(unittest.TestCase):
    compile_run = eyes.EyesTest.compile_run

    def test_explicit_offline_is_pink_without_recolouring_network_loss(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_effect_policy.h"
__attribute__((weak)) void service_v2_rgb_effect_policy_offline(service_v2_rgb_effect_policy_t *p, bool value) {(void)p;(void)value;}
static service_v2_rgb_effect_policy_t p;
static service_v2_rgb_color_t px[5];
static void frame(unsigned mask,int r,int g,int b) {
 assert(service_v2_rgb_effect_policy_alert_frame(&p,px));
 for(unsigned i=0;i<5;i++) {
  bool on=(mask&(1U<<i))!=0;
  assert(px[i].red==(on?r:0) && px[i].green==(on?g:0) && px[i].blue==(on?b:0));
 }
}
int main(void) {
 service_v2_rgb_effect_policy_init(&p,1);
 service_v2_rgb_effect_policy_offline(&p,true);frame(31,120,25,65);
 service_v2_rgb_effect_policy_tick(&p,299);frame(31,120,25,65);
 service_v2_rgb_effect_policy_tick(&p,1);frame(0,0,0,0);
 service_v2_rgb_effect_policy_offline(&p,true);frame(0,0,0,0);
 service_v2_rgb_effect_policy_network(&p,false);frame(0,0,0,0);
 service_v2_rgb_effect_policy_tick(&p,699);frame(0,0,0,0);
 service_v2_rgb_effect_policy_tick(&p,1);frame(31,120,25,65);
 service_v2_rgb_effect_policy_network(&p,true);frame(31,120,25,65);
 service_v2_rgb_effect_policy_battery(&p,20,SERVICE_V2_RGB_POWER_DISCHARGING);
 frame(10,120,0,0); /* low battery remains visible while deliberately offline */
 service_v2_rgb_effect_policy_battery(&p,80,SERVICE_V2_RGB_POWER_CHARGING);
 frame(31,120,25,65);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_ACTIVE);
 frame(10,80,30,0); /* explicit provisioning indication retains priority */
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_INACTIVE);
 frame(31,120,25,65);
 service_v2_rgb_effect_policy_offline(&p,false);
 service_v2_rgb_effect_policy_network(&p,true);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,px));
 service_v2_rgb_effect_policy_network(&p,false);frame(31,80,30,0);
 service_v2_rgb_effect_policy_network(&p,true);
 service_v2_rgb_effect_policy_offline(&p,true);frame(31,120,25,65);
 service_v2_rgb_effect_policy_offline(&p,false);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,px));
 return 0;
}
''', policy=True)

    def test_alert_replaces_show_and_clears_non_eye_pixels(self):
        source = (ROOT/'apps/arcs-mini/services/service_v2_rgb_effect.c').read_text(encoding='utf-8')
        self.compile_run(r'''
#include <assert.h>
#include <string.h>
#include "service_v2_rgb_effect_policy.h"
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
static unsigned s_output_failure_count,frames,shows;
static int owner=1;
static struct {unsigned started_ms;} s_show;
static service_v2_rgb_color_t actual[5];
static int service_v2_actuators_rgb_get_status(service_v2_rgb_status_t *s) {s->owner=owner;return 0;}
static int service_v2_actuators_rgb_system_frame(service_v2_rgb_color_t *p) {memcpy(actual,p,sizeof(actual));frames++;return 0;}
static int service_v2_actuators_rgb_set(int r,int g,int b) {assert(0);return 0;}
static int service_v2_actuators_rgb_off(void) {memset(actual,0,sizeof(actual));return 0;}
static unsigned xTaskGetTickCount(void) {return 1000;}
static int service_v2_rgb_show_current(void *s,unsigned now) {shows++;return 1;}
static void service_v2_rgb_show_frame(int kind,unsigned ms,service_v2_rgb_color_t *p) {assert(0);}
''' + c_function(source,'rgb_effect_apply') + r'''
int main(void) {
 service_v2_rgb_effect_policy_t p;service_v2_rgb_effect_policy_init(&p,1);
 service_v2_rgb_effect_policy_network(&p,false);rgb_effect_apply(&p,0);
 assert(frames==1 && !shows);
 for(int i=0;i<5;i++) assert(actual[i].red && actual[i].green);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_ACTIVE);
 rgb_effect_apply(&p,0);assert(frames==2 && !shows);
 for(int i=0;i<5;i++) assert((actual[i].red!=0)==(i==1||i==3));
 service_v2_rgb_effect_policy_tick(&p,300);rgb_effect_apply(&p,0);
 for(int i=0;i<5;i++) assert(!actual[i].red && !actual[i].green);
 s_output_enabled=false;rgb_effect_apply(&p,0);assert(frames==3);
 s_output_enabled=true;owner=2;rgb_effect_apply(&p,0);assert(frames==3);
 return 0;
}
''', policy=True)

    def test_alert_priority_masks_cadence_and_restore(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_effect_policy.h"
static service_v2_rgb_effect_policy_t p;
static service_v2_rgb_color_t pixels[5];
static void check(unsigned mask, int orange) {
 assert(service_v2_rgb_effect_policy_alert_frame(&p,pixels));
 for(unsigned i=0;i<5;i++){
  if(mask&(1U<<i)) {
   assert(pixels[i].red>0 && pixels[i].blue==0);
   assert(orange ? (pixels[i].green>0 && pixels[i].red>pixels[i].green) : pixels[i].green==0);
  } else assert(!pixels[i].red && !pixels[i].green && !pixels[i].blue);
 }
}
int main(void){
 service_v2_rgb_effect_policy_init(&p,1);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,pixels));
 service_v2_rgb_effect_policy_network(&p,false);check(31,1);
 service_v2_rgb_effect_policy_event(&p,SERVICE_V2_RGB_EVENT_TTS_PLAYING);check(31,1);
 service_v2_rgb_effect_policy_tick(&p,299);check(31,1);
 service_v2_rgb_effect_policy_tick(&p,1);check(0,1);
 service_v2_rgb_effect_policy_network(&p,false);check(0,1); /* poll must not restart blink */
 service_v2_rgb_effect_policy_tick(&p,699);check(0,1);
 service_v2_rgb_effect_policy_tick(&p,1);check(31,1);
 service_v2_rgb_effect_policy_battery(&p,20,SERVICE_V2_RGB_POWER_DISCHARGING);check(31,1);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_ACTIVE);check(10,1);
 service_v2_rgb_effect_policy_tick(&p,300);check(0,1);
 service_v2_rgb_effect_policy_tick(&p,700);check(10,1);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_INACTIVE);check(31,1);
 service_v2_rgb_effect_policy_network(&p,true);check(10,0);
 service_v2_rgb_effect_policy_tick(&p,300);check(0,0);
 service_v2_rgb_effect_policy_tick(&p,700);check(10,0);
 service_v2_rgb_effect_policy_battery(&p,24,SERVICE_V2_RGB_POWER_DISCHARGING);check(10,0);
 service_v2_rgb_effect_policy_battery(&p,25,SERVICE_V2_RGB_POWER_DISCHARGING);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,pixels));
 service_v2_rgb_effect_policy_event(&p,SERVICE_V2_RGB_EVENT_DISCONNECTED);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,pixels)); /* cloud end isn't network loss */
 service_v2_rgb_effect_policy_battery(&p,10,SERVICE_V2_RGB_POWER_DISCHARGING);check(10,0);
 service_v2_rgb_effect_policy_battery(&p,10,SERVICE_V2_RGB_POWER_CHARGING);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,pixels));
 return 0;
}
''', policy=True)
