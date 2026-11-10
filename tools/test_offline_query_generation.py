"""Run the production query worker with a cancellation inside cloud start."""
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


def function(source, name):
    found = re.search(r'^(?:static )?(?:bool|int|void) ' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert found, name
    end = source.index('{', found.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[found.start():end]


class OfflineQueryGenerationTest(unittest.TestCase):
    def test_cancel_during_start_cleans_old_query_before_releasing_new_owner(self):
        source = (ROOT/'apps/arcs-mini/services/service_voice_action.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define VOICE_QUERY_STOP_POLLS 3
#define VOICE_QUERY_STOP_POLL_MS 10
#define VOICE_QUERY_SETTLE_MS 1
#define VOICE_QUERY_RETRY_COUNT 3
#define VOICE_QUERY_RETRY_MS 1
#define LINGXIN_DIALOG_PHASE_IDLE 0
#define portMAX_DELAY 0
#define pdPASS 1
typedef int service_voice_query_t;
typedef struct {int query;uint32_t generation;uint32_t owner_generation;} voice_query_request_t;
static uint32_t s_query_generation=1;
static bool s_query_inflight;
static void *s_query_queue;
static uint32_t owner=11;
static bool online=true,active,inject_cancel=true,seen_barrier,fail_stop_once;
static unsigned starts,stops,blocked_new_starts;
static bool change_owner_while_waiting, delivered;
static uint32_t request_owner=11;
static jmp_buf finished;
bool service_voice_action_query_idle(void);
static bool lingxin_wait_query_idle(uint32_t owner_generation);
static bool service_offline_action_online_current(uint32_t generation) {return online && generation==owner;}
static uint32_t service_offline_action_generation(void) {return owner;}
static const char *voice_query_name(int query) {return "version";}
static bool voice_query_build_text(int query,char *text,size_t size) {snprintf(text,size,"2.6.3");return true;}
static void lisa_thread_mdelay(unsigned ms) {
 if(change_owner_while_waiting) {change_owner_while_waiting=false;++owner;s_query_inflight=false;}
}
static void lingxin_server_suppress_current_tts(const char *reason) {}
static bool lingxin_server_chat_is_active(void) {return active;}
static int lingxin_server_dialog_phase(void) {return active?1:0;}
static int lingxin_server_chat_stop(void) {
 ++stops;
 if(fail_stop_once && active) {fail_stop_once=false;return -EBUSY;}
 active=false;return 0;
}
static int lingxin_server_chat_start_query_text(const char *text) {
 ++starts;
 if(inject_cancel) {
  ++s_query_generation;owner=12;online=false;
  seen_barrier=!service_voice_action_query_idle();
  /* Both offline cleanup and the next online start must honor this barrier. */
  online=true; /* New online owner selected, but its worker must not start yet. */
  if(!lingxin_wait_query_idle(owner)) ++blocked_new_starts;
 }
 active=true;return 0; /* Old SDK call completes after cancellation. */
}
static int xQueueReceive(void *queue,voice_query_request_t *request,unsigned wait) {
 if(delivered) longjmp(finished,1);
 delivered=true;*request=(voice_query_request_t){1,s_query_generation,request_owner};return 1;
}
'''
        # Before the fix, the absence of a barrier is modeled as idle so the
        # regression fails on the leaked real-worker start, not a missing symbol.
        if 'bool service_voice_action_query_idle(void)' not in source:
            code += '\nbool service_voice_action_query_idle(void) {return true;}\n'
        names = ['voice_query_is_current', 'voice_query_stop_old_chat']
        for optional in ['service_voice_action_query_idle', 'voice_query_begin', 'voice_query_finish_stale']:
            if re.search(r'^(?:static )?(?:bool|void) '+optional+r'\(', source, re.M):
                names.append(optional)
        names.append('voice_query_task')
        code += '\n'.join(function(source, name) for name in names)
        wakeup=(ROOT/'src/category/comm/msgs/voice_wakeup_msg.c').read_text(encoding='utf-8')
        code += '\n'+function(wakeup, 'lingxin_wait_query_idle')
        code += r'''
int main(void) {
 fail_stop_once=true;
 if(!setjmp(finished)) voice_query_task(0);
 assert(starts==1 && !active); /* Stale success must not survive offline cleanup. */
 assert(seen_barrier && blocked_new_starts==1 && service_voice_action_query_idle());
 assert(stops==3); /* old round stop, rejected compensation, accepted retry */
 online=true;inject_cancel=false;
 assert(service_voice_action_query_idle());
 lingxin_server_chat_start_query_text("new owner");
 assert(active && starts==2 && stops==3); /* No delayed compensation can stop it. */
 s_query_inflight=true;change_owner_while_waiting=true;
 assert(!lingxin_wait_query_idle(12)); /* Owner changes at the same time barrier opens. */
 assert(owner==13 && lingxin_wait_query_idle(13));
 request_owner=12;delivered=false;
 if(!setjmp(finished)) voice_query_task(0);
 assert(starts==2 && stops==3); /* Even a matching query generation cannot cross owners. */
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'query.c', Path(temp)/'query.elf'
            cfile.write_text(code, encoding='utf-8')
            build=subprocess.run([str(CC),'--specs=sim.specs','-std=c11',str(cfile),'-o',str(elf)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(RUN),str(elf)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
