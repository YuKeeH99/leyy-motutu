"""Execute the real RGB layout, dim policy and actuator dispatch on the simulator."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN
from tools.test_servo_output_gate import function

DIR = ROOT / "apps/arcs-mini/services"


class EyesTest(unittest.TestCase):
    def test_event_bursts_cannot_accelerate_colour_transition(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_effect_policy.h"
int main(void){
 uint32_t pending=50;
 assert(service_v2_rgb_effect_frame_due(0,&pending));
 for(unsigned i=0;i<100;i++) assert(!service_v2_rgb_effect_frame_due(0,&pending));
 assert(!service_v2_rgb_effect_frame_due(49,&pending));
 assert(service_v2_rgb_effect_frame_due(1,&pending));
 assert(service_v2_rgb_effect_frame_due(1000,&pending));
 assert(!service_v2_rgb_effect_frame_due(0,&pending));
 return 0;
}
''', policy=True)

    def compile_run(self, code, policy=False):
        with tempfile.TemporaryDirectory() as tmp:
            source, elf = Path(tmp) / "test.c", Path(tmp) / "test.elf"
            source.write_text(code, encoding="utf-8")
            result = subprocess.run([str(CC), "--specs=sim.specs", "-std=c11", "-I", str(DIR), str(source),
                                     *([str(DIR / "service_v2_rgb_effect_policy.c")] if policy else []), "-o", str(elf)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_system_and_actions_use_eyes_manual_diagnostics_remain_uniform(self):
        source = (DIR / "service_v2_actuators.c").read_text(encoding="utf-8")
        self.compile_run(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT 100
typedef enum {SERVICE_V2_RGB_OWNER_SYSTEM, SERVICE_V2_RGB_OWNER_MANUAL, SERVICE_V2_RGB_OWNER_ACTION} service_v2_rgb_owner_t;
static struct {unsigned count,red,green,blue,brightness_percent,owner;} s_rgb_status = {.count=5};
static bool used_eyes; static unsigned writes;
static bool rgb_strip_params_valid(unsigned count,unsigned brightness){return count>0 && count<=64 && brightness<=100;}
static int ws2812_send_uniform_locked(unsigned n,unsigned r,unsigned g,unsigned b,unsigned level){used_eyes=false; writes++; return 0;}
static int ws2812_send_pattern_locked(unsigned n,unsigned r,unsigned g,unsigned b,unsigned level,bool eyes){used_eyes=eyes; writes++; return 0;}
''' + function(source, "rgb_strip_write_locked") + r'''
int main(void){
 assert(rgb_strip_write_locked(5,10,20,30,60,SERVICE_V2_RGB_OWNER_SYSTEM)==0); assert(used_eyes);
 assert(rgb_strip_write_locked(5,10,20,30,60,SERVICE_V2_RGB_OWNER_ACTION)==0); assert(used_eyes);
 assert(rgb_strip_write_locked(5,10,20,30,60,SERVICE_V2_RGB_OWNER_MANUAL)==0); assert(!used_eyes);
 unsigned old=writes; assert(rgb_strip_write_locked(0,10,20,30,60,SERVICE_V2_RGB_OWNER_SYSTEM)<0); assert(writes==old);
 return 0;
}
''')

    def test_all_operating_colours_stay_dim_including_power_and_alerts(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_effect_policy.h"
int main(void){
 service_v2_rgb_effect_policy_t p; service_v2_rgb_color_t c;
 service_v2_rgb_effect_policy_init(&p,1);
 for(unsigned state=0;state<3;state++) for(unsigned power=0;power<3;power++) for(unsigned emotion=0;emotion<7;emotion++){
  p.state=state;p.power_state=power;p.emotion=emotion;
  service_v2_rgb_effect_policy_color(&p,100,&c);
  assert(c.red<=160 && c.green<=160 && c.blue<=160);
 }
 service_v2_rgb_effect_policy_event(&p,SERVICE_V2_RGB_EVENT_DIZZY);
 service_v2_rgb_effect_policy_color(&p,100,&c);assert(c.red<=160 && c.green==0 && c.blue==0);
 unsigned previous=service_v2_rgb_effect_breath_percent(0);
 for(unsigned phase=1;phase<=64;phase++){
  unsigned level=service_v2_rgb_effect_breath_percent(phase);
  assert(level>=18 && level<=70);
  assert(level<=previous+4 && previous<=level+4);previous=level;
 }
 return 0;
}
''', policy=True)

    def test_five_pixel_layout_has_only_second_and_fourth_lit(self):
        header = DIR / "service_v2_rgb_layout.h"
        self.assertTrue(header.exists(), "missing per-pixel layout")
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_layout.h"
int main(void){
 const unsigned expected[]={0,0x123456,0,0x123456,0};
 for(unsigned i=0;i<5;i++){
  assert(service_v2_rgb_pixel_word(i,0x123456,true)==expected[i]);
  assert(service_v2_rgb_pixel_word(i,0x123456,false)==0x123456);
  assert(service_v2_rgb_pixel_word(i,0,true)==0);
 }
 assert(service_v2_rgb_pixel_word(5,0x123456,true)==0);
 return 0;
}
''')


if __name__ == "__main__": unittest.main()
