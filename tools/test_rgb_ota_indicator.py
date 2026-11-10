"""Run OTA lamp timing and actual OTA-state mapping on the C simulator."""
import unittest
from tools import test_rgb_eyes as eyes
from tools.test_offline_action import ROOT, c_function


class OtaIndicatorTest(unittest.TestCase):
    compile_run = eyes.EyesTest.compile_run

    def test_cycle_priority_and_terminal_states(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_effect_policy.h"
static service_v2_rgb_effect_policy_t p;
static service_v2_rgb_color_t px[5];
static void color(int r,int g,int b) {
 assert(service_v2_rgb_effect_policy_alert_frame(&p,px));
 for(int i=0;i<5;i++) assert(px[i].red==r && px[i].green==g && px[i].blue==b);
}
int main(void) {
 service_v2_rgb_effect_policy_init(&p,1);
 service_v2_rgb_effect_policy_network(&p,false);
 service_v2_rgb_effect_policy_mute(&p,true);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_ACTIVE);
 service_v2_rgb_effect_policy_ota(&p,SERVICE_V2_RGB_OTA_UPDATING);
 color(80,0,0);
 service_v2_rgb_effect_policy_tick(&p,300); color(0,0,0);
 service_v2_rgb_effect_policy_ota(&p,SERVICE_V2_RGB_OTA_UPDATING);
 service_v2_rgb_effect_policy_tick(&p,100); color(0,80,0);
 service_v2_rgb_effect_policy_tick(&p,400); color(0,0,80);
 service_v2_rgb_effect_policy_tick(&p,400); color(80,0,0);
 service_v2_rgb_effect_policy_network(&p,true);
 service_v2_rgb_effect_policy_mute(&p,false);
 service_v2_rgb_effect_policy_provisioning(&p,SERVICE_V2_RGB_PROVISIONING_INACTIVE);
 service_v2_rgb_effect_policy_ota(&p,SERVICE_V2_RGB_OTA_SUCCESS); color(0,80,0);
 service_v2_rgb_effect_policy_tick(&p,1999); color(0,80,0);
 service_v2_rgb_effect_policy_tick(&p,1);
 service_v2_rgb_effect_policy_ota(&p,SERVICE_V2_RGB_OTA_SUCCESS);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,px)); /* no endless restart */
 service_v2_rgb_effect_policy_ota(&p,SERVICE_V2_RGB_OTA_UPDATING); color(80,0,0);
 service_v2_rgb_effect_policy_ota(&p,SERVICE_V2_RGB_OTA_FAILED); color(80,0,0);
 service_v2_rgb_effect_policy_tick(&p,300); color(0,0,0);
 service_v2_rgb_effect_policy_tick(&p,300); color(80,0,0);
 service_v2_rgb_effect_policy_tick(&p,2400);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,px));
 service_v2_rgb_effect_policy_ota(&p,SERVICE_V2_RGB_OTA_NONE);
 assert(!service_v2_rgb_effect_policy_alert_frame(&p,px));
 return 0;
}
''', policy=True)

    def test_real_mapping_keeps_app_staged_blinking_until_reboot(self):
        source = (ROOT / 'apps/arcs-mini/services/service_v2_rgb_effect.c').read_text(encoding='utf-8')
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_effect_policy.h"
#define CONFIG_OTA 1
enum {OTA_STATE_IDLE,OTA_STATE_CHECKING,OTA_STATE_PACKAGE_INFO,OTA_STATE_UPDATING,
      OTA_STATE_SUCCESSED,OTA_STATE_FAILED,OTA_STATE_UP_TO_DATE};
enum {OTA_TARGET_UNSPECIFIED,OTA_TARGET_APP,OTA_TARGET_PROMPT_TONE};
typedef struct {int state,target;} ota_state_t;
static ota_state_t snapshot;
static int ota_manager_get_state_snapshot(ota_state_t *s) {*s=snapshot;return 0;}
''' + c_function(source, 'rgb_effect_update_ota') + r'''
int main(void) {
 service_v2_rgb_effect_policy_t p; service_v2_rgb_effect_policy_init(&p,1);
 for(int i=OTA_STATE_IDLE;i<=OTA_STATE_PACKAGE_INFO;i++) {
  snapshot.state=i;rgb_effect_update_ota(&p);assert(p.ota_state==SERVICE_V2_RGB_OTA_NONE);
 }
 snapshot.state=OTA_STATE_UPDATING; snapshot.target=OTA_TARGET_APP;
 rgb_effect_update_ota(&p); assert(p.ota_state==SERVICE_V2_RGB_OTA_UPDATING);
 service_v2_rgb_effect_policy_tick(&p,400);
 snapshot.state=OTA_STATE_SUCCESSED;rgb_effect_update_ota(&p);
 assert(p.ota_state==SERVICE_V2_RGB_OTA_UPDATING && p.ota_elapsed_ms==400);
 snapshot.target=OTA_TARGET_PROMPT_TONE;rgb_effect_update_ota(&p);
 assert(p.ota_state==SERVICE_V2_RGB_OTA_SUCCESS);
 snapshot.state=OTA_STATE_FAILED;rgb_effect_update_ota(&p);
 assert(p.ota_state==SERVICE_V2_RGB_OTA_FAILED);
 snapshot.state=OTA_STATE_UP_TO_DATE;rgb_effect_update_ota(&p);
 assert(p.ota_state==SERVICE_V2_RGB_OTA_NONE);
 return 0;
}
''', policy=True)


if __name__ == '__main__':
    unittest.main()
