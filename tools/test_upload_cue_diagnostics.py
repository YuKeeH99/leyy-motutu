"""Run cue ownership and diagnostic branches from the production functions."""
import unittest

from test_lingxin_round_races import COMMON, ROOT, body, run_c


PLAYER = 'apps/arcs-mini/voice_player.c'
CUE_SUPPORT = r'''
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define __atomic_compare_exchange_n(p,e,n,w,s,f) cue_cas(p,e,n)
static bool cue_cas(uint32_t *p,uint32_t *e,uint32_t n){
 if(*p!=*e){*e=*p;return false;}*p=n;return true;
}
''' + '#include "' + (ROOT / 'apps/arcs-mini/voice_player.h').as_posix() + '"\n' + r'''
typedef enum {APP_PLAYER_EVENT_PLAYING,APP_PLAYER_EVENT_COMPLETED,
 APP_PLAYER_EVENT_STOPPED,APP_PLAYER_EVENT_ERROR} app_player_event_t;
static uint32_t s_upload_cue_notified_epoch,s_upload_cue_playing_epoch;
static bool s_upload_cue_playback_started;
static voice_upload_cue_stats_t s_upload_cue_stats;
'''


def functions(*names):
    signatures = {
        'voice_player_upload_cue_note': 'static void voice_player_upload_cue_note(uint32_t *counter,uint32_t epoch)',
        'voice_player_get_upload_cue_stats': 'void voice_player_get_upload_cue_stats(voice_upload_cue_stats_t *stats)',
        'voice_player_upload_cue_event': 'static void voice_player_upload_cue_event(app_player_event_t event)',
        'voice_player_release_upload_cue': 'static void voice_player_release_upload_cue(uint32_t epoch)',
        'voice_player_cancel_upload_cue': 'void voice_player_cancel_upload_cue(uint32_t epoch)',
        'voice_player_wait_upload_cue': 'void voice_player_wait_upload_cue(void)',
    }
    return ''.join(signatures[name] + body(PLAYER, name) for name in names)


class UploadCueDiagnosticsTest(unittest.TestCase):
    def test_one_notification_per_round_even_after_playback_releases_waiter(self):
        run_c(COMMON + CUE_SUPPORT + r'''
typedef struct {uint32_t round_epoch, owner_generation;} upload_cue_request_t;
static uint32_t s_upload_cue_epoch,s_upload_cue_cancelled_epoch;
static uint32_t current_epoch=7;
static int queued;
static bool fail_queue;
static bool voice_player_upload_epoch_current(uint32_t e){return e==current_epoch;}
static void voice_player_start_upload_cue(void *p,uint32_t n){}
static int voice_invoke(void(*f)(void*,uint32_t),void*p,unsigned n){queued++;return fail_queue ? -1 : 0;}
static void voice_player_release_upload_cue(uint32_t e){if(s_upload_cue_epoch==e)s_upload_cue_epoch=0;}
''' + functions('voice_player_upload_cue_note') + r'''
void voice_player_notify_upload_complete(uint32_t epoch)
''' + body(PLAYER, 'voice_player_notify_upload_complete') + r'''
int main(void){
 voice_player_notify_upload_complete(7);assert(queued==1);
 voice_player_notify_upload_complete(7);assert(queued==1);
 s_upload_cue_epoch=0;
 voice_player_notify_upload_complete(7);assert(queued==1);
 current_epoch=8;voice_player_notify_upload_complete(8);assert(queued==2);
 assert(s_upload_cue_stats.valid_requests==4 && s_upload_cue_stats.duplicate_requests==2);
 voice_player_notify_upload_complete(7);assert(s_upload_cue_stats.skipped_stale==1);
 current_epoch=9;fail_queue=true;voice_player_notify_upload_complete(9);
 assert(queued==3 && !s_upload_cue_epoch && s_upload_cue_stats.enqueue_failures==1);
 voice_player_notify_upload_complete(9);assert(queued==3);
}
''')

    def test_old_player_completion_cannot_release_new_pending_request(self):
        run_c(COMMON + CUE_SUPPORT + r'''
static uint32_t s_upload_cue_epoch=8,s_upload_cue_cancelled_epoch;
''' + functions('voice_player_release_upload_cue', 'voice_player_upload_cue_event',
                'voice_player_get_upload_cue_stats', 'voice_player_cancel_upload_cue') + r'''
int main(void){
 s_upload_cue_playing_epoch=7;
 voice_player_upload_cue_event(APP_PLAYER_EVENT_PLAYING);
 voice_player_upload_cue_event(APP_PLAYER_EVENT_PLAYING);
 voice_player_upload_cue_event(APP_PLAYER_EVENT_COMPLETED);
 voice_player_upload_cue_event(APP_PLAYER_EVENT_COMPLETED);
 assert(s_upload_cue_epoch==8 && !s_upload_cue_playing_epoch);
 voice_upload_cue_stats_t stats={0};
 voice_player_get_upload_cue_stats(&stats);voice_player_get_upload_cue_stats(NULL);
 assert(stats.started==1 && stats.completed==1 && stats.pending_epoch==8 && !stats.playing_epoch);
 voice_player_cancel_upload_cue(7);assert(s_upload_cue_epoch==8 && !s_upload_cue_stats.cancelled);
 voice_player_cancel_upload_cue(8);voice_player_cancel_upload_cue(8);
 assert(!s_upload_cue_epoch && s_upload_cue_stats.cancelled==1);
 s_upload_cue_playing_epoch=9;s_upload_cue_epoch=9;
 voice_player_upload_cue_event(APP_PLAYER_EVENT_ERROR);
 voice_player_upload_cue_event(APP_PLAYER_EVENT_STOPPED);
 assert(!s_upload_cue_epoch && s_upload_cue_stats.playback_failures==1 && !s_upload_cue_stats.stopped);
}
''')

    def test_wait_counts_real_timeout_but_not_round_change(self):
        run_c(COMMON + CUE_SUPPORT + r'''
static uint32_t s_upload_cue_epoch=7;
static bool current=true,change;
static unsigned delays;
#define pdMS_TO_TICKS(ms) (ms)
static bool voice_player_upload_epoch_current(uint32_t epoch){return current;}
static void vTaskDelay(unsigned ticks){assert(ticks==5);delays++;if(change)current=false;}
''' + functions('voice_player_release_upload_cue', 'voice_player_upload_cue_note',
                'voice_player_wait_upload_cue') + r'''
int main(void){
 voice_player_wait_upload_cue();assert(delays==200 && !s_upload_cue_epoch && s_upload_cue_stats.wait_timeouts==1);
 s_upload_cue_epoch=8;change=true;delays=0;
 voice_player_wait_upload_cue();assert(delays==1 && !s_upload_cue_epoch && s_upload_cue_stats.wait_timeouts==1);
}
''')


if __name__ == '__main__':
    unittest.main()
