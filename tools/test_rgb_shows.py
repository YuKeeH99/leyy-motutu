"""Run production show lifecycle and five-pixel frames on RISC-V simulator."""
from tools import test_rgb_eyes
from tools.test_servo_output_gate import function
import unittest
DIR = test_rgb_eyes.DIR


class ShowTest(unittest.TestCase):
    compile_run = test_rgb_eyes.EyesTest.compile_run

    def test_actual_tone_playing_and_terminal_callbacks_control_show(self):
        source = (DIR.parent / "voice_player.c").read_text(encoding="utf-8")
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_show.h"
typedef enum {APP_PLAYER_EVENT_PLAYING, APP_PLAYER_EVENT_COMPLETED, APP_PLAYER_EVENT_STOPPED,
 APP_PLAYER_EVENT_ERROR, APP_PLAYER_EVENT_PAUSED, APP_PLAYER_EVENT_PREPARED} app_player_event_t;
static bool s_rgb_boot_tone;
static service_v2_rgb_show_t show={.enabled=true};
static void service_v2_rgb_effect_show_start(service_v2_rgb_show_kind_t kind){service_v2_rgb_show_start(&show,kind,0);}
static void service_v2_rgb_effect_show_stop(service_v2_rgb_show_kind_t kind){service_v2_rgb_show_stop(&show,kind);}
''' + function(source, "voice_player_boot_rgb_event") + r'''
int main(void){
 voice_player_boot_rgb_event(APP_PLAYER_EVENT_PLAYING); assert(show.kind==SERVICE_V2_RGB_SHOW_NONE);
 for(unsigned end=APP_PLAYER_EVENT_COMPLETED;end<=APP_PLAYER_EVENT_PAUSED;end++) {
  show=(service_v2_rgb_show_t){.enabled=true}; s_rgb_boot_tone=true;
  voice_player_boot_rgb_event(APP_PLAYER_EVENT_PREPARED);assert(show.kind==SERVICE_V2_RGB_SHOW_NONE);
  voice_player_boot_rgb_event(APP_PLAYER_EVENT_PLAYING);assert(show.kind==SERVICE_V2_RGB_SHOW_BOOT);
  voice_player_boot_rgb_event(end);assert(show.kind==SERVICE_V2_RGB_SHOW_NONE);
  voice_player_boot_rgb_event(APP_PLAYER_EVENT_PLAYING);assert(show.kind==SERVICE_V2_RGB_SHOW_NONE);
 }
 return 0;
}
''')

    def test_worker_restores_eyes_respects_owner_and_clears_sleep_race(self):
        source = (DIR / "service_v2_rgb_effect.c").read_text(encoding="utf-8")
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_show.h"
#include "service_v2_actuators.h"
#define taskENTER_CRITICAL() enter_critical()
#define taskEXIT_CRITICAL() ((void)0)
#define LISA_LOGE(...) ((void)0)
#define LISA_LOGI(...) ((void)0)
#define portTICK_PERIOD_MS 1U
static unsigned now_ms=100;
static unsigned xTaskGetTickCount(void){return now_ms;}
static bool s_output_enabled=true,s_first_output_logged;
static uint32_t s_output_failure_count;
static service_v2_rgb_show_t s_show={.enabled=true};
static bool start_before_lock;
static void enter_critical(void) {
 if(start_before_lock) {
  start_before_lock=false;
  service_v2_rgb_show_start(&s_show,SERVICE_V2_RGB_SHOW_DANCE,++now_ms);
 }
}
static service_v2_rgb_status_t status={.count=5};
static unsigned frames,eyes,offs;static bool sleep_during_write;
int service_v2_actuators_rgb_get_status(service_v2_rgb_status_t *out){*out=status;return 0;}
int service_v2_actuators_rgb_set(uint8_t r,uint8_t g,uint8_t b){eyes++;return 0;}
int service_v2_actuators_rgb_off(void){offs++;return 0;}
int service_v2_actuators_rgb_system_frame(const service_v2_rgb_color_t p[5]) {
 frames++; assert(p[0].blue>0 && p[4].blue>0);
 if(sleep_during_write)s_output_enabled=false;
 return 0;
}
''' + function(source, "rgb_effect_apply") + r'''
int main(void){
 service_v2_rgb_effect_policy_t p;service_v2_rgb_effect_policy_init(&p,0);
 start_before_lock=true; /* Gait preempts just before snapshot lock. */
 rgb_effect_apply(&p,0);assert(frames==1 && eyes==0);
 status.owner=SERVICE_V2_RGB_OWNER_MANUAL;rgb_effect_apply(&p,0);assert(frames==1 && eyes==0);
 status.owner=SERVICE_V2_RGB_OWNER_SYSTEM;
 service_v2_rgb_show_stop(&s_show,SERVICE_V2_RGB_SHOW_DANCE);
 p.state=SERVICE_V2_RGB_EFFECT_LISTENING;
 rgb_effect_apply(&p,0);assert(eyes==1);
 service_v2_rgb_show_start(&s_show,SERVICE_V2_RGB_SHOW_DANCE,0);sleep_during_write=true;
 rgb_effect_apply(&p,0);assert(frames==2 && offs==1);
 rgb_effect_apply(&p,0);assert(frames==2 && eyes==1);
 return 0;
}
''', policy=True)

    def test_system_frame_calibrates_each_pixel_and_preserves_manual_owner(self):
        source = (DIR / "service_v2_actuators.c").read_text(encoding="utf-8")
        self.compile_run(r'''
#include <assert.h>
#include <errno.h>
#include "service_v2_actuators.h"
#define LISA_WAIT_FOREVER 0
static bool s_ready=true;
static int s_lock;
static service_v2_rgb_status_t s_rgb_status={.owner=SERVICE_V2_RGB_OWNER_SYSTEM};
static unsigned writes,calibrations;
static void lisa_mutex_lock(int lock,int wait){}
static void lisa_mutex_unlock(int lock){}
static uint32_t rgb_calibration_wire_word(uint8_t r,uint8_t g,uint8_t b,uint8_t level){
 assert(level==100);calibrations++;return (r<<16)|(g<<8)|b;
}
static int ws2812_send_pixels_locked(uint8_t count,const uint32_t *pixels){
 assert(count==5);for(unsigned i=0;i<5;i++)assert(pixels[i]==((i+1)<<16|2<<8|3));writes++;return 0;
}
''' + function(source, "service_v2_actuators_rgb_system_frame") + r'''
int main(void){
 service_v2_rgb_color_t p[]={{1,2,3},{2,2,3},{3,2,3},{4,2,3},{5,2,3}};
 assert(service_v2_actuators_rgb_system_frame(p)==0);assert(writes==1 && calibrations==5);
 s_rgb_status.owner=SERVICE_V2_RGB_OWNER_MANUAL;
 assert(service_v2_actuators_rgb_system_frame(p)==0);assert(writes==1 && calibrations==5);
 s_rgb_status.owner=SERVICE_V2_RGB_OWNER_ACTION;
 assert(service_v2_actuators_rgb_system_frame(p)==0);assert(writes==1);
 assert(service_v2_actuators_rgb_system_frame(0)==-EINVAL);
 return 0;
}
''')

    def test_lifecycle_timeout_sleep_and_late_boot_callbacks(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_show.h"
int main(void) {
 service_v2_rgb_show_t s = {.enabled=true};
 service_v2_rgb_show_start(&s,SERVICE_V2_RGB_SHOW_BOOT,100);
 assert(service_v2_rgb_show_current(&s,100)==SERVICE_V2_RGB_SHOW_BOOT);
 service_v2_rgb_show_stop(&s,SERVICE_V2_RGB_SHOW_BOOT);
 assert(service_v2_rgb_show_current(&s,150)==SERVICE_V2_RGB_SHOW_NONE);
 service_v2_rgb_show_start(&s,SERVICE_V2_RGB_SHOW_BOOT,200); /* late PLAYING */
 assert(service_v2_rgb_show_current(&s,200)==SERVICE_V2_RGB_SHOW_NONE);
 service_v2_rgb_show_start(&s,SERVICE_V2_RGB_SHOW_DANCE,300);
 service_v2_rgb_show_stop(&s,SERVICE_V2_RGB_SHOW_BOOT); /* old tone ends */
 assert(service_v2_rgb_show_current(&s,350)==SERVICE_V2_RGB_SHOW_DANCE);
 service_v2_rgb_show_enable(&s,false);
 service_v2_rgb_show_start(&s,SERVICE_V2_RGB_SHOW_DANCE,400);
 service_v2_rgb_show_enable(&s,true);
 assert(service_v2_rgb_show_current(&s,450)==SERVICE_V2_RGB_SHOW_NONE);
 service_v2_rgb_show_start(&s,SERVICE_V2_RGB_SHOW_DANCE,500);
 assert(service_v2_rgb_show_current(&s,120500)==SERVICE_V2_RGB_SHOW_NONE);
 service_v2_rgb_show_start(&s,SERVICE_V2_RGB_SHOW_DANCE,0xfffffff0U);
 assert(service_v2_rgb_show_current(&s,50)==SERVICE_V2_RGB_SHOW_DANCE);
 service_v2_rgb_show_cancel(&s);
 assert(service_v2_rgb_show_current(&s,100)==SERVICE_V2_RGB_SHOW_NONE);
 return 0;
}
''')

    def test_marquee_visits_five_pixels_and_dance_stays_symmetric_and_dim(self):
        self.compile_run(r'''
#include <assert.h>
#include "service_v2_rgb_show.h"
int main(void) {
 service_v2_rgb_color_t p[5], previous[5];
 const unsigned heads[]={0,1,2,3,4,3,2,1,0};
 for(unsigned step=0;step<9;step++) {
  service_v2_rgb_show_frame(SERVICE_V2_RGB_SHOW_BOOT,step*200,p);
  for(unsigned i=0;i<5;i++) {
   assert(p[i].red==0 && p[i].green<=112 && p[i].blue<=112);
   assert(p[heads[step]].green>=p[i].green);
  }
 }
 for(unsigned t=0;t<4000;t+=50) {
  service_v2_rgb_show_frame(SERVICE_V2_RGB_SHOW_DANCE,t,p);
  for(unsigned i=0;i<5;i++) {
   assert(p[i].red<=112 && p[i].green<=112 && p[i].blue<=112);
   assert(p[i].blue>0);
   assert(p[i].red==p[4-i].red && p[i].green==p[4-i].green && p[i].blue==p[4-i].blue);
   if(t) assert((int)p[i].blue-(int)previous[i].blue<=24 && (int)previous[i].blue-(int)p[i].blue<=24);
   previous[i]=p[i];
  }
 }
 return 0;
}
''')


if __name__ == "__main__":
    import unittest
    unittest.main()
