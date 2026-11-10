import re
import unittest
from tools import test_offline_action as support


class PlayerCallbackDrainTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_queued_events_keep_original_callback_and_owner_during_reregistration(self):
        source = (support.ROOT / 'arcs-sdk/components/app_player/app_player.c').read_text(encoding='utf-8')
        header = (support.ROOT / 'arcs-sdk/components/app_player/app_player_internal.h').read_text(encoding='utf-8')
        event_type = re.search(r'typedef struct callback_event \{.*?\} callback_event_t;', header, re.S).group()
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define LISA_OS_WAIT_FOREVER (-1)
#define APP_PLAYER_OK 0
#define APP_PLAYER_ERR_INVALID_PARAM (-1)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static unsigned locks;
#define PLAYER_MUTEX_LOCK(...) (++locks)
#define PLAYER_MUTEX_UNLOCK(...) (--locks)
typedef unsigned app_player_event_t;
typedef struct app_player_s app_player_t;
typedef void (*app_player_event_cb_t)(app_player_t *, app_player_event_t, void *);
''' + event_type + r'''
typedef struct {
 callback_event_t *head, *tail;
 int lock, sem;
 bool running;
 unsigned pending;
} callback_queue_t;
struct app_player_s {
 callback_queue_t *cb_queue;
 struct {int lock;app_player_event_cb_t callback;void *user_data;} cb_map;
 const char *name;
 unsigned id;
};
static app_player_t *active_player;
static unsigned calls, frees;
static bool register_during_allocation;
static unsigned owners[3], events[3], handlers[3];
static int app_player_register_callback(app_player_t *, app_player_event_cb_t, void *);
static void callback_a(app_player_t *, app_player_event_t, void *);
static void callback_b(app_player_t *, app_player_event_t, void *);
static void callback_c(app_player_t *, app_player_event_t, void *);
static void *lisa_mem_calloc(size_t n,size_t size) {
 assert(!locks);
 if(register_during_allocation) {
  register_during_allocation=false;
  assert(app_player_register_callback(active_player,callback_b,(void *)8)==0);
 }
 return calloc(n,size);
}
static void lisa_mem_free(void *p) {++frees;free(p);}
static void lisa_semaphore_give(int sem) {(void)sem;assert(!locks);}
static int lisa_semaphore_take(int sem,int timeout) {
 (void)sem;(void)timeout;
 assert(!locks);
 if(active_player->cb_queue->head) return 0;
 active_player->cb_queue->running=false;
 return -1;
}
''' + support.c_function(source, 'app_player_register_callback').replace('int app_player_register_callback', 'static int app_player_register_callback') +
support.c_function(source, '__enqueue_callback_event_owned') +
support.c_function(source, '__enqueue_callback_event') +
support.c_function(source, '__callback_thread_func') +
support.c_function(source, 'app_player_callbacks_idle') + r'''
static void note(app_player_t *p,unsigned e,void *owner,unsigned handler) {
 assert(!locks && !app_player_callbacks_idle(p) && calls<3);
 owners[calls]=(unsigned)(uintptr_t)owner;events[calls]=e;handlers[calls]=handler;++calls;
}
static void callback_a(app_player_t *p,unsigned e,void *owner) {
 note(p,e,owner,1);
 /* Registering/enqueuing from a callback must not retarget already queued B. */
 assert(app_player_register_callback(p,callback_c,(void *)9)==0);
 __enqueue_callback_event(p,3);
}
static void callback_b(app_player_t *p,unsigned e,void *owner) {note(p,e,owner,2);}
static void callback_c(app_player_t *p,unsigned e,void *owner) {note(p,e,owner,3);}
int main(void) {
 callback_queue_t queue={.running=true};
 app_player_t player={.cb_queue=&queue,.name="cloud"};active_player=&player;
 assert(app_player_register_callback(&player,callback_a,(void *)7)==0);
 register_during_allocation=true;
 __enqueue_callback_event(&player,1); /* Owner changes while event allocation yields. */
 __enqueue_callback_event(&player,2);
 assert(app_player_register_callback(&player,callback_c,(void *)99)==0);
 __callback_thread_func(&player);
 assert(calls==3 && frees==3 && app_player_callbacks_idle(&player));
 assert(handlers[0]==1 && owners[0]==7 && events[0]==1);
 assert(handlers[1]==2 && owners[1]==8 && events[1]==2);
 assert(handlers[2]==3 && owners[2]==9 && events[2]==3);
 /* A previously unobserved event must not be delivered to a later owner. */
 player.cb_map.callback=NULL;player.cb_map.user_data=NULL;queue.running=true;
 __enqueue_callback_event(&player,4);
 assert(app_player_register_callback(&player,callback_c,(void *)10)==0);
 __callback_thread_func(&player);
 assert(calls==3 && frees==4 && app_player_callbacks_idle(&player) && !locks);
 return 0;
}
''')

    def test_inflight_bus_message_and_old_callback_cannot_cross_shutdown(self):
        voice = (support.ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static bool s_shutdown_prompt_in_progress;
static uint32_t s_audio_message_inflight;
static unsigned stops;
static void voice_player_begin_shutdown_prompt(void);
static void voice_player_handle_msg(void *u,uint32_t id,void *d,uint32_t n,void *x) {
 assert(s_audio_message_inflight==1);
 voice_player_begin_shutdown_prompt(); /* power worker preempts accepted CANCEL */
 assert(s_audio_message_inflight==1 && s_shutdown_prompt_in_progress);
 stops++;
}
''' + support.c_function(voice, 'voice_player_begin_shutdown_prompt').replace('void voice_player_begin', 'static void voice_player_begin') +
support.c_function(voice, 'voice_player_play_msg') + r'''
int main(void) {
 voice_player_play_msg(0,1,0,0,0);
 assert(stops==1 && s_audio_message_inflight==0);
 voice_player_play_msg(0,1,0,0,0);assert(stops==1);
 return 0;
}
''')

    def test_dequeued_callback_is_still_pending_until_return(self):
        source = (support.ROOT / 'arcs-sdk/components/app_player/app_player.c').read_text(encoding='utf-8')
        enqueue = support.c_function(source, '__enqueue_callback_event_owned')
        callback = support.c_function(source, '__callback_thread_func')
        self.assertIn('queue->pending++', enqueue)
        self.assertGreater(callback.index('queue->pending--'), callback.index('cb(player, evt->event, user_data)'))
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define PLAYER_MUTEX_LOCK(...) ((void)0)
#define PLAYER_MUTEX_UNLOCK(...) ((void)0)
typedef struct {void *head;unsigned pending;int lock;} callback_queue_t;
typedef struct {callback_queue_t *cb_queue;} app_player_t;
''' + support.c_function(source, 'app_player_callbacks_idle') + r'''
int main(void) {
 callback_queue_t queue={.head=0,.pending=1};app_player_t player={&queue};
 assert(!app_player_callbacks_idle(&player));
 queue.pending=0;assert(app_player_callbacks_idle(&player));
 return 0;
}
''')
