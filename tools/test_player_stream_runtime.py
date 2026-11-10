"""Run the real player write/reset functions with deterministic driver faults."""
import unittest
from tools import test_offline_action as support


class PlayerStreamRuntimeTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_stream_start_and_resume_acquire_focus_inside_operation_lock(self):
        source = (support.ROOT / 'arcs-sdk/components/app_player/app_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_APP_PLAYER_AUDIO_FOCUS 1
#define LISA_OS_WAIT_FOREVER (-1)
#define APP_PLAYER_OK 0
#define APP_PLAYER_ERR_INVALID_PARAM (-1)
#define APP_PLAYER_ERR_INVALID_STATE (-2)
#define APP_PLAYER_ERR_NOT_SUPPORTED (-3)
#define APP_PLAYER_ERR_IO (-4)
#define APP_PLAYER_FOCUS_FOREGROUND 1
#define PLAYER_OK 0
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
typedef int PlayerErr;
typedef int app_player_focus_state_t;
typedef enum {PLAYER_ST_IDLE,PLAYER_ST_PLAYING,PLAYER_ST_PAUSED,PLAYER_ST_PREPARED} PlayerState;
typedef struct {int operation_lock,hld;const char *name;
 bool is_stream_mode,is_preparing,drain_in_progress,paused_by_focus;
} app_player_t;
static unsigned locks,acquires,starts,resumes;
static int focus_state=APP_PLAYER_FOCUS_FOREGROUND,core_result;
#define PLAYER_MUTEX_LOCK(...) (++locks)
#define PLAYER_MUTEX_UNLOCK(...) (--locks)
static void app_player_focus_set_paused_by_focus(app_player_t *p,bool b) {p->paused_by_focus=b;}
static int app_player_focus_acquire(app_player_t *p) {(void)p;assert(locks==1);++acquires;return 0;}
static int app_player_focus_get_state(app_player_t *p) {(void)p;return focus_state;}
static PlayerState lisa_player_get_state(int h) {(void)h;return PLAYER_ST_IDLE;}
static int app_player_core_stop_sync(app_player_t *p) {(void)p;assert(0);return 0;}
static int lisa_player_play(int h) {(void)h;assert(0);return 0;}
static int lisa_player_stop_sync(int h) {(void)h;assert(0);return 0;}
static int app_player_core_play_stream(app_player_t *p,uint32_t hz,uint8_t ch,uint8_t bits) {
 (void)p;assert(locks==1 && hz==16000 && ch==1 && bits==16);++starts;return core_result;
}
static int app_player_core_resume_sync(app_player_t *p) {(void)p;assert(locks==1);++resumes;return core_result;}
''' + support.c_function(source, 'app_player_play_stream') +
support.c_function(source, 'app_player_resume') + r'''
int main(void) {
 app_player_t player={.name="cloud"};
 assert(app_player_play_stream(&player,16000,1,16)==APP_PLAYER_OK && starts==1 && !locks);
 assert(app_player_resume(&player)==APP_PLAYER_OK && resumes==1 && !locks);
 focus_state=0;
 assert(app_player_play_stream(&player,16000,1,16)==APP_PLAYER_OK && starts==1 && !locks);
 assert(app_player_resume(&player)==APP_PLAYER_OK && resumes==1 && !locks && player.paused_by_focus);
 focus_state=APP_PLAYER_FOCUS_FOREGROUND;core_result=-1;
 assert(app_player_play_stream(&player,16000,1,16)==APP_PLAYER_ERR_IO && !locks);
 assert(app_player_resume(&player)==APP_PLAYER_ERR_INVALID_STATE && !locks);
 assert(acquires==6);
 return 0;
}
''')

    def test_reset_cleanup_and_callback_owner_precede_new_playback(self):
        source = (support.ROOT / 'arcs-sdk/components/app_player/app_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_APP_PLAYER_AUDIO_FOCUS 1
#define CONFIG_APP_PLAYER_PA_OFF_DELAY_MS 200
#define LISA_OS_WAIT_FOREVER (-1)
#define APP_PLAYER_ERR_INVALID_PARAM (-1)
#define APP_PLAYER_ERR_INVALID_STATE (-2)
#define APP_PLAYER_OK 0
#define APP_PLAYER_STATE_IDLE 0
#define PLAYER_OK 0
#define PLAYER_OP_FAIL (-2)
#define PLAYER_ST_NONE 1
#define PLAYER_ST_READY_TO_PLAY 2
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
typedef int PlayerErr;
typedef enum {PLAYER_EVT_INIT,PLAYER_EVT_PLAYING,PLAYER_EVT_PAUSED,PLAYER_EVT_STOPED,
 PLAYER_EVT_PLAYBACK_COMPLETE,PLAYER_EVT_ERROR} PlayerEvt;
typedef PlayerEvt app_player_event_t;
#define APP_PLAYER_EVENT_STOPPED PLAYER_EVT_STOPED
typedef struct app_player_s app_player_t;
typedef void (*app_player_event_cb_t)(app_player_t *,app_player_event_t,void *);
struct app_player_s {
 int operation_lock,hld,state;
 unsigned id;
 const char *name;
 bool pa_is_on,is_stream_mode,is_preparing,prepare_error,pause_preparing;
 bool stop_preparing_requested,wait_prepare_intercepted,user_initiated;
 PlayerEvt last_evt;
 struct {int lock;app_player_event_cb_t callback;void *user_data;} cb_map;
};
static unsigned locks[3],critical_depth,pa_refs,focus_owner,event_owner;
static bool start_on_unlock,start_from_focus_callback;
static unsigned owner_a=1,owner_b=2;
static app_player_t *active;
static void __app_player_upper_callback_handler(app_player_t *,PlayerEvt,bool);
static void callback(app_player_t *p,app_player_event_t e,void *owner) {(void)p;(void)e;(void)owner;}
static void new_playback(void) {
 active->cb_map.callback=callback;active->cb_map.user_data=&owner_b;
 focus_owner=2;
 __app_player_upper_callback_handler(active,PLAYER_EVT_PLAYING,false);
}
static void unlock(int lock) {
 assert(locks[lock]);--locks[lock];
 if(lock==1 && !locks[1] && start_on_unlock) {start_on_unlock=false;new_playback();}
}
#define PLAYER_MUTEX_LOCK(lock,timeout) (++locks[lock])
#define PLAYER_MUTEX_UNLOCK(lock) unlock(lock)
#define taskENTER_CRITICAL() (++critical_depth)
#define taskEXIT_CRITICAL() (--critical_depth)
static void app_player_focus_set_user_initiated(app_player_t *p,bool v) {p->user_initiated=v;}
static bool app_player_focus_is_user_initiated(app_player_t *p) {return p->user_initiated;}
static int app_player_focus_release(app_player_t *p,bool user) {
 (void)p;assert(user);focus_owner=0;
 if(start_from_focus_callback) {start_from_focus_callback=false;new_playback();}
 return 0;
}
static int pa_manager_control(int on,unsigned delay) {
 (void)delay;assert(!critical_depth);
 if(on) ++pa_refs;else {assert(pa_refs);--pa_refs;}
 return 0;
}
static PlayerErr lisa_player_reset(int h) {(void)h;return 0;}
static int lisa_player_get_state(int h) {(void)h;return 4;}
static app_player_event_t __convert_player_evt_to_app_event(PlayerEvt e) {return e;}
static void __enqueue_callback_event_owned(app_player_t *p,app_player_event_t e,
 app_player_event_cb_t cb,void *owner) {
 (void)p;assert(e==PLAYER_EVT_STOPED && cb==callback);event_owner=*(unsigned *)owner;
}
static void __enqueue_callback_event(app_player_t *p,app_player_event_t e) {
 __enqueue_callback_event_owned(p,e,p->cb_map.callback,p->cb_map.user_data);
}
''' + support.c_function(source, '__app_player_upper_callback_handler') +
support.c_function(source, 'app_player_reset') + r'''
int main(void) {
 app_player_t player={.operation_lock=1,.pa_is_on=true,.is_stream_mode=true,.name="cloud",
  .cb_map={.lock=2,.callback=callback,.user_data=&owner_a}};
 active=&player;pa_refs=1;focus_owner=1;start_on_unlock=true;
 assert(app_player_reset(&player)==APP_PLAYER_OK);
 assert(player.pa_is_on && pa_refs==1 && focus_owner==2);
 assert(event_owner==1 && !locks[1] && !locks[2]);
 /* Focus notification may synchronously register/start another playback on
  * the same recursive operation mutex. The old event must keep owner A. */
 player.cb_map.user_data=&owner_a;focus_owner=1;event_owner=0;
 start_from_focus_callback=true;
 assert(app_player_reset(&player)==APP_PLAYER_OK);
 assert(player.pa_is_on && pa_refs==1 && focus_owner==2);
 assert(event_owner==1 && !locks[1] && !locks[2]);
 return 0;
}
''')

    def test_short_first_write_and_zero_write_do_not_wait_for_prepared(self):
        source = (support.ROOT / 'arcs-sdk/components/app_player/app_player_core.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LISA_OS_WAIT_FOREVER (-1)
#define SEMAPHORE_WAIT_TIMEOUT_MS 10000
#define PLAYER_OK 0
#define PLAYER_OP_FAIL (-2)
#define LISA_OK 0
#define APP_PLAYER_EVENT_ERROR 9
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static unsigned locks;
#define PLAYER_MUTEX_LOCK(...) (++locks)
#define PLAYER_MUTEX_UNLOCK(...) (--locks)
typedef int PlayerErr;
typedef enum {PLAYER_EVT_PREPARED,PLAYER_EVT_PLAYBACK_COMPLETE,PLAYER_EVT_PAUSED,PLAYER_EVT_ERROR} PlayerEvt;
typedef struct app_player_s {
 int core_lock,hld,preparing_sem,pause_sem;
 unsigned id;
 const char *name;
 bool is_stream_mode,is_preparing,prepare_error,pause_preparing;
 bool stop_preparing_requested,wait_prepare_intercepted,drain_in_progress;
 void *core_upper_callback;
 PlayerEvt last_evt;
} app_player_t;
typedef void (*app_player_core_callback_t)(app_player_t *,PlayerEvt,bool);
static app_player_t *active;
static unsigned writes,waits,resets,errors,plays,signals,notifications;
static int accepted;
static bool fail_during_write;
static app_player_t *__find_player_by_id(int id) {(void)id;return active;}
static int lisa_player_put_stream_data(int h,uint8_t *data,size_t size,unsigned timeout) {
 (void)h;(void)data;assert(size==128 && timeout==25);++writes;
 if(fail_during_write) active->prepare_error=true;
 return accepted;
}
static int lisa_semaphore_take(int sem,int timeout) {(void)sem;(void)timeout;++waits;return -1;}
static void lisa_semaphore_give(int sem) {(void)sem;++signals;}
static int lisa_player_reset(int h) {(void)h;++resets;return 0;}
static int lisa_player_play(int h) {(void)h;++plays;return 0;}
static void __enqueue_callback_event(app_player_t *p,int event) {(void)p;(void)event;++errors;}
static void upper(app_player_t *p,PlayerEvt event,bool notify) {
 assert(p==active && event==PLAYER_EVT_PREPARED && notify);++notifications;
}
''' + support.c_function(source, 'app_player_core_stream_write') +
support.c_function(source, '__lisa_player_core_callback_handler') + r'''
int main(void) {
 app_player_t player={.is_stream_mode=true,.is_preparing=true,.name="cloud"};
 active=&player;player.core_upper_callback=(void *)upper;uint8_t pcm[128]={0};
 accepted=32;
 assert(app_player_core_stream_write(&player,pcm,128,25)==32);
 assert(writes==1 && waits==0 && resets==0 && errors==0 && !locks);
 assert(player.is_preparing && player.is_stream_mode);
 accepted=0;
 assert(app_player_core_stream_write(&player,pcm,128,25)==0);
 assert(waits==0 && resets==0 && player.is_preparing && !locks);
 accepted=128;
 assert(app_player_core_stream_write(&player,pcm,128,25)==128);
 assert(waits==0 && player.is_preparing);
 /* The existing asynchronous PREPARED handler still starts the hardware. */
 assert(__lisa_player_core_callback_handler(PLAYER_EVT_PREPARED,0,0,0)==0);
 assert(plays==1 && signals==1 && notifications==1 && !player.is_preparing);
 player.prepare_error=true;unsigned before=writes;
 assert(app_player_core_stream_write(&player,pcm,128,25)<0 && writes==before);
 player.prepare_error=false;fail_during_write=true;
 assert(app_player_core_stream_write(&player,pcm,128,25)<0 && !locks);
 fail_during_write=false;player.prepare_error=false;accepted=-1;
 assert(app_player_core_stream_write(&player,pcm,128,25)<0 && !locks);
 player.is_stream_mode=false;before=writes;
 assert(app_player_core_stream_write(&player,pcm,128,25)<0 && writes==before);
 assert(waits==0 && resets==0 && errors==0);
 return 0;
}
''')

    def test_reset_releases_owned_pa_once_even_with_delayed_stop(self):
        source = (support.ROOT / 'arcs-sdk/components/app_player/app_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_APP_PLAYER_AUDIO_FOCUS 1
#define CONFIG_APP_PLAYER_PA_OFF_DELAY_MS 200
#define LISA_OS_WAIT_FOREVER (-1)
#define APP_PLAYER_ERR_INVALID_PARAM (-1)
#define APP_PLAYER_ERR_INVALID_STATE (-2)
#define APP_PLAYER_OK 0
#define APP_PLAYER_STATE_IDLE 0
#define PLAYER_OK 0
#define PLAYER_OP_FAIL (-2)
#define PLAYER_ST_NONE 1
#define PLAYER_ST_READY_TO_PLAY 2
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static unsigned locks;
#define PLAYER_MUTEX_LOCK(...) (++locks)
#define PLAYER_MUTEX_UNLOCK(...) (--locks)
typedef int PlayerErr;
typedef enum {PLAYER_EVT_INIT,PLAYER_EVT_PLAYING,PLAYER_EVT_PAUSED,PLAYER_EVT_STOPED,
 PLAYER_EVT_PLAYBACK_COMPLETE,PLAYER_EVT_ERROR} PlayerEvt;
typedef PlayerEvt app_player_event_t;
#define APP_PLAYER_EVENT_STOPPED PLAYER_EVT_STOPED
typedef struct app_player_s app_player_t;
typedef void (*app_player_event_cb_t)(app_player_t *,app_player_event_t,void *);
struct app_player_s {
 int operation_lock,hld,state;
 unsigned id;
 const char *name;
 bool pa_is_on,is_stream_mode,is_preparing,prepare_error,pause_preparing;
 bool stop_preparing_requested,wait_prepare_intercepted,user_initiated;
 PlayerEvt last_evt;
 struct {int lock;app_player_event_cb_t callback;void *user_data;} cb_map;
};
static app_player_t *active;
static unsigned pa_on,pa_off,focus_releases,notified;
static unsigned critical_depth;
#define taskENTER_CRITICAL() (++critical_depth)
#define taskEXIT_CRITICAL() (--critical_depth)
static int reset_result;
static int driver_state=4;
static bool become_none_during_reset;
static unsigned reset_calls;
static bool raw_stop_in_reset,duplicate_stop_during_pa_release;
static void __app_player_upper_callback_handler(app_player_t *,PlayerEvt,bool);
static void app_player_focus_set_user_initiated(app_player_t *p,bool v) {p->user_initiated=v;}
static bool app_player_focus_is_user_initiated(app_player_t *p) {return p->user_initiated;}
static int app_player_focus_release(app_player_t *p,bool user) {(void)p;assert(user);++focus_releases;return 0;}
static int pa_manager_control(int on,unsigned delay) {
 (void)delay;assert(!critical_depth);
 if(on) ++pa_on;
 else {
  ++pa_off;
  if(duplicate_stop_during_pa_release) {
   duplicate_stop_during_pa_release=false;
   __app_player_upper_callback_handler(active,PLAYER_EVT_STOPED,false);
  }
 }
 return 0;
}
static PlayerErr lisa_player_reset(int h) {
 (void)h;++reset_calls;
 if(become_none_during_reset) driver_state=PLAYER_ST_NONE;
 if(!reset_result && raw_stop_in_reset)
  __app_player_upper_callback_handler(active,PLAYER_EVT_STOPED,true);
 return reset_result;
}
static int lisa_player_get_state(int h) {(void)h;return driver_state;}
static app_player_event_t __convert_player_evt_to_app_event(PlayerEvt e) {return e;}
static void __enqueue_callback_event(app_player_t *p,app_player_event_t e) {(void)p;assert(e==PLAYER_EVT_STOPED);++notified;}
static void __enqueue_callback_event_owned(app_player_t *p,app_player_event_t e,
 app_player_event_cb_t cb,void *owner) {(void)cb;(void)owner;__enqueue_callback_event(p,e);}
''' + support.c_function(source, '__app_player_upper_callback_handler') +
support.c_function(source, 'app_player_reset') + r'''
int main(void) {
 app_player_t player={.pa_is_on=true,.is_stream_mode=true,.is_preparing=true,
  .prepare_error=true,.name="cloud"};active=&player;
 duplicate_stop_during_pa_release=true;
 assert(app_player_reset(&player)==APP_PLAYER_OK);
 assert(!player.pa_is_on && !player.is_stream_mode && !player.is_preparing);
 assert(!player.prepare_error && player.state==APP_PLAYER_STATE_IDLE && !locks);
 assert(pa_off==1 && focus_releases==1);
 __app_player_upper_callback_handler(&player,PLAYER_EVT_STOPED,true);
 assert(pa_off==1 && focus_releases==1);
 assert(app_player_reset(&player)==APP_PLAYER_OK && pa_off==1);
 /* An actual STOPPED emitted by reset and its synthetic cleanup share PA ownership. */
 player.pa_is_on=true;player.is_stream_mode=true;raw_stop_in_reset=true;
 assert(app_player_reset(&player)==APP_PLAYER_OK && pa_off==2);
 __app_player_upper_callback_handler(&player,PLAYER_EVT_STOPED,true);
 assert(pa_off==2 && !player.pa_is_on && !player.is_stream_mode && !locks);
 /* Failed reset must not claim that a still-active stream released its amplifier. */
 player.pa_is_on=true;player.is_stream_mode=true;reset_result=-1;
 assert(app_player_reset(&player)==APP_PLAYER_ERR_INVALID_STATE);
 assert(player.pa_is_on && player.is_stream_mode && pa_off==2 && !locks);
 /* The binary driver returns OP_FAIL for an already-reset NONE state.
  * This is an idempotent reset, and must still clean the application owner. */
 driver_state=PLAYER_ST_NONE;reset_result=PLAYER_OP_FAIL;
 unsigned calls_before=reset_calls;
 assert(app_player_reset(&player)==APP_PLAYER_OK);
 assert(reset_calls==calls_before && !player.pa_is_on && !player.is_stream_mode && pa_off==3);
 /* A concurrent driver-side completion/reset can win after the first read. */
 driver_state=4;become_none_during_reset=true;player.pa_is_on=true;player.is_stream_mode=true;
 assert(app_player_reset(&player)==APP_PLAYER_OK);
 assert(reset_calls==calls_before+1 && !player.pa_is_on && !player.is_stream_mode && pa_off==4);
 /* READY uses the same -2 code, but is not quiescent: do not fake completion. */
 driver_state=PLAYER_ST_READY_TO_PLAY;become_none_during_reset=false;
 player.pa_is_on=true;player.is_stream_mode=true;
 assert(app_player_reset(&player)==APP_PLAYER_ERR_INVALID_STATE);
 assert(player.pa_is_on && player.is_stream_mode && pa_off==4);
 /* An unrelated failure must not be ignored merely because NONE was observed. */
 driver_state=4;become_none_during_reset=true;reset_result=-1;
 assert(app_player_reset(&player)==APP_PLAYER_ERR_INVALID_STATE);
 assert(player.pa_is_on && player.is_stream_mode && pa_off==4 && !locks);
 return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
