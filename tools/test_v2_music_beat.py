"""Execute music beat poses and verify cancellation/command wiring."""
import unittest
from test_lingxin_round_races import ROOT, COMMON, body, run_c

PATH='apps/arcs-mini/services/service_v2_gait.c'

class MusicBeatTest(unittest.TestCase):
    def test_real_player_tickets_isolate_late_stop_and_wakeup(self):
        from tools.test_offline_action import c_function
        source=(ROOT/'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        funcs='\n'.join(c_function(source,name) for name in (
            'voice_player_set_beat_result',
            'voice_player_beat_music_event','voice_player_start_beat_music',
            'voice_player_beat_music_begin','voice_player_beat_music_position',
            'voice_player_stop_beat_music','voice_player_beat_music_end','voice_player_beat_music_cancel'))
        run_c(COMMON+r'''
#include <errno.h>
static unsigned critical;
#define taskENTER_CRITICAL() (++critical)
#define taskEXIT_CRITICAL() (--critical)
#define __atomic_load_n(p,o) (*(p))
#define __atomic_store_n(p,v,o) (*(p)=(v))
static bool exchange(uint32_t*p,uint32_t*e,uint32_t v){if(*p==*e){*p=v;return true;}*e=*p;return false;}
#define __atomic_compare_exchange_n(p,e,v,w,s,f) exchange(p,e,v)
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define CONFIG_VOICE_LINK_LINGXIN 1
#define APP_PLAYER_OK 0
#define APP_PLAYER_STATE_PLAYING 3
enum {APP_PLAYER_EVENT_PLAYING=1,APP_PLAYER_EVENT_COMPLETED,APP_PLAYER_EVENT_STOPPED,APP_PLAYER_EVENT_ERROR};
typedef int app_player_t;
typedef int app_player_event_t;
typedef struct {uint32_t ticket,generation;} beat_music_request_t;
static uint32_t s_beat_music_sequence,s_beat_music_ticket,s_beat_music_owner;
static int s_beat_music_result;
static bool s_tone_playing,s_shutdown_prompt_in_progress;
static int beat,music;
static app_player_t *beat_player=&beat,*music_player=&music;
static unsigned generation=1,plays,stops;
static unsigned drains;
static void voice_player_drain_wakeup_output(void){drains++;assert(s_beat_music_owner && !critical);}
static void (*event_cb)(app_player_t*,int,void*);
static void *event_context;
typedef struct {void(*cb)(void*,uint32_t);uint32_t words[2],len;} request_t;
static request_t q[32];static unsigned head,tail;
static int voice_invoke(void(*cb)(void*,uint32_t),const void*d,uint32_t len){
 assert(tail<32 && len<=8);q[tail].cb=cb;q[tail].len=len;memcpy(q[tail++].words,d,len);return 0;
}
static void drain(void){while(head<tail){request_t r=q[head++];r.cb(r.words,r.len);}}
static unsigned service_offline_action_generation(void){return generation;}
static const char *voice_player_beat_music_url(void){return "mem://song";}
static bool app_player_callbacks_idle(app_player_t*p){return true;}
static int app_player_get_state(app_player_t*p){return *p;}
static int app_player_get_position(app_player_t*p,unsigned*out){assert(p==beat_player);*out=500;return 0;}
static void lingxin_server_suppress_current_tts(const char*r){}
static void app_player_register_callback(app_player_t*p,void(*cb)(app_player_t*,int,void*),void*ctx){
 assert(p==beat_player);event_cb=cb;event_context=ctx;
}
static int app_player_play(app_player_t*p,const char*u){assert(p==beat_player);plays++;*p=3;event_cb(p,1,event_context);return 0;}
static int app_player_stop(app_player_t*p){
 assert(p==music_player || p==beat_player);*p=0;
 if(p==beat_player){stops++;event_cb(p,APP_PLAYER_EVENT_STOPPED,event_context);}return 0;
}
'''+funcs+r'''
int main(void){
 unsigned t=voice_player_beat_music_begin();voice_player_beat_music_cancel();drain();assert(!plays);
 t=voice_player_beat_music_begin();generation++;drain();assert(!plays && s_beat_music_result==-ECANCELED);
 t=voice_player_beat_music_begin();drain();assert(plays==1);
 unsigned p;assert(!voice_player_beat_music_position(t,&p) && p==500);
 voice_player_beat_music_end(t);drain();assert(stops==1 && !s_beat_music_owner && !drains);
 unsigned fresh=voice_player_beat_music_begin();drain();assert(plays==2 && fresh!=t);
 voice_player_beat_music_event(beat_player,APP_PLAYER_EVENT_STOPPED,(void*)(uintptr_t)t);
 voice_player_set_beat_result(t,-EIO); /* Late start failure cannot damage the new ticket. */
 voice_player_beat_music_end(t);drain();assert(stops==1 && !voice_player_beat_music_position(fresh,&p));
 beat=0;event_cb(beat_player,APP_PLAYER_EVENT_COMPLETED,event_context);
 assert(voice_player_beat_music_position(fresh,&p)==1);
 assert(drains==1);
 voice_player_beat_music_end(fresh);drain();assert(stops==1);
 assert(!critical);
 return 0;
}
''')

    def test_runner_tracks_playback_and_always_cleans_up(self):
        code=body(PATH,'run_music_beat')
        run_c(COMMON+r'''
#include <errno.h>
#define SERVICE_V2_GAIT_ACTION_MUSIC_BEAT 99
#define SERVICE_V2_SERVO_COUNT 4
#define SERVICE_V2_SERVO_EASING_SMOOTH 1
#define GAIT_TICK_MS 20U
static const uint16_t s_pose_stand[4]={2100,900,2050,870};
static unsigned now,poses,ended,disabled,homed,cancel_at;
static int mode,result;
static bool cmd_pending(void){return cancel_at && now>=cancel_at;}
static int prepare_normal_face_motion(const char*x,bool*s){*s=true;return 0;}
static unsigned voice_player_beat_music_begin(void){return 7;}
static int voice_player_beat_music_position(unsigned t,unsigned*p){
 assert(t==7);if(mode==1)return -EAGAIN;if(mode==2)return -EIO;
 if(now<100)return -EAGAIN;*p=now-100;return *p>=18800?1:0;
}
static void voice_player_beat_music_end(unsigned t){assert(t==7);ended++;}
static int hold_pose(unsigned ms){now+=ms;return cmd_pending()?-ECANCELED:0;}
static int play_frame(const uint16_t*f,unsigned ms,int e){poses++;now+=ms;return cmd_pending()?-ECANCELED:0;}
static int play_frame_settled(const uint16_t*f,unsigned ms,int e){homed++;return 0;}
static int service_v2_actuators_servo_disable_all(void){disabled++;return 0;}
static void status_set(bool b,int a,unsigned d,int r){result=r;}
static void music_beat_pose(uint32_t phase,uint16_t *frame)
'''+body(PATH,'music_beat_pose')+'\nstatic void run_music_beat(void)\n'+code+r'''
int main(void){
 run_music_beat();assert(ended==1 && homed==1 && !disabled && !result && poses>60 && now<20000);
 now=0;mode=1;run_music_beat();assert(ended==2 && disabled==1 && result==-ETIMEDOUT && now<=3100);
 now=0;mode=2;run_music_beat();assert(ended==3 && disabled==2 && result==-EIO && now==0);
 now=0;mode=0;cancel_at=1600;run_music_beat();assert(ended==4 && disabled==3 && homed==1 && !result);
 return 0;
}
''')

    def test_rear_paws_alternate_with_front_support_and_home_between_beats(self):
        code=body(PATH,'music_beat_pose')
        run_c(COMMON+r'''
static const uint16_t s_pose_stand[4]={2100,900,2050,870};
static void music_beat_pose(uint32_t phase,uint16_t *frame)
'''+code+r'''
int main(void) {
 uint16_t f[4];
 for(unsigned p=0;p<72;p++){
  music_beat_pose(p,f);assert(f[0]==2100 && f[1]==900);
  assert(f[2]>=2050 && f[2]<=2140 && f[3]>=780 && f[3]<=870);
  assert(f[2]==2050 || f[3]==870); /* Never lift both rear supports. */
  if(p%2)assert(!memcmp(f,s_pose_stand,8));
 }
 music_beat_pose(0,f);assert(f[2]==2140 && f[3]==870);
 music_beat_pose(2,f);assert(f[2]==2050 && f[3]==780);
 return 0;
}
''')

    def test_cloud_command_and_fixed_timing(self):
        action=(ROOT/'apps/arcs-mini/services/service_voice_action.c').read_text(encoding='utf-8')
        self.assertRegex(action,r'"DOG_PLAY_MUSIC",\s*SERVICE_V2_GAIT_ACTION_MUSIC_BEAT')
        policy=(ROOT/'apps/arcs-mini/services/service_voice_action_policy.c').read_text(encoding='utf-8')
        self.assertIn('case SERVICE_V2_GAIT_ACTION_MUSIC_BEAT:',policy)
        self.assertIn('voice_player_beat_music_end(ticket)',body(PATH,'run_music_beat'))
        self.assertIn('voice_player_beat_music_cancel()',action)

if __name__=='__main__': unittest.main()
