"""Execute deferred capture-stop handoff with the real manager functions."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_recorder_manager_lifecycle_contract import ROOT, function_body
from test_v2_rgb_effect_contract import CC, RUN


class RecordLimitResumeTest(unittest.TestCase):
    def test_ack_never_drains_on_receive_worker(self):
        source = (ROOT / 'third_party/AI2T_LingXinEngine/src/voice_chat/recorder_manager.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX 1
#define LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH 5
#define LINGXIN_DEBUG 0
#define LINGXIN_WARN 1
#define lingxin_log_ut(...) ((void)0)
#define lingxin_trace_set(...) ((void)0)
typedef void (*LingxinRecorderStopCallback)(bool);
typedef void (*LingxinRecorderDataCallback)(void*,int,int);
static int record_close_mutex=1,record_close_callback_list_mutex=2;
static int stop_pending_until_server_ready,server_ready,send_flag;
static int record_close_stop_ticket;
static unsigned int record_close_stop_epoch;
static void *lingxin_recorder=(void*)1;
static LingxinRecorderDataCallback temp_data_callback;
static LingxinRecorderStopCallback temp_stop_callback_list[5];
static int temp_stop_callback_list_length,locks[3],closed,completed,cues;
#include <limits.h>
static int next_stop_ticket,temp_stop_callback_tickets[5];
static bool claimed_stop_persistent[5];
static unsigned temp_stop_callback_epochs[5],claimed_stop_epoch;
static LingxinRecorderStopCallback claimed_stop_callbacks[5];
static int claimed_stop_count;
static bool receive_worker,limit=true,frozen,tail_ready;
static bool lingxin_clean_recorder_limit_timed_out(void *r) {bool done=tail_ready;tail_ready=false;return done;}
static bool lingxin_clean_recorder_take_limit_finish(void *r) {bool done=tail_ready;tail_ready=false;return done;}
static void voice_chat_notify_record_limit(unsigned int epoch) {cues++;}
static unsigned int voice_chat_current_epoch(void) {return 7;}
static int failures;
static void voice_chat_fail_waiting_cloud_vad(unsigned int epoch) {assert(!locks[1] && epoch==7);failures++;}
static void lingxin_mutex_lock(int m) {assert(!locks[m]);locks[m]=1;}
static void lingxin_mutex_unlock(int m) {assert(locks[m]);locks[m]=0;}
static void set_send_flag(void) {send_flag=server_ready;}
static bool lingxin_clean_recorder_take_limit_stop(void *r) {
 bool reached=r && limit;limit=false;return reached;
}
static void lingxin_clean_recorder_prepare_stop(void *r,bool drain) {frozen=true;}
static void record_close_callback_for_stop_wait_send_left(int result) {
 assert(locks[1]);lingxin_mutex_unlock(1);closed++;
 while(temp_stop_callback_list_length) temp_stop_callback_list[--temp_stop_callback_list_length](true);
}
static void lingxin_recorder_close(void *r,void (*cb)(int)) {
 assert(!receive_worker);assert(send_flag && frozen);cb(0);
}
static void module_record_stop(int wait,void *cb) {assert(!receive_worker);}
static void stopped(bool ok) {assert(ok);completed++;}
static void data(void *p,int n,int i) {}
'''
        code += "static int register_stop_callback_once(unsigned int epoch,LingxinRecorderStopCallback callback,bool persistent) {" + function_body(source, "register_stop_callback_once") + "}\n"
        for name, args in (
            ('module_record_start_send', 'LingxinRecorderDataCallback data_callback'),
            ('module_record_poll_limit', 'LingxinRecorderStopCallback stop_callback'),
        ):
            code += f'void {name}({args}) {{' + function_body(source, name) + '}\n'
        code += r'''
int main(void) {
 module_record_poll_limit(stopped);
 assert(cues==0 && !stop_pending_until_server_ready && !closed);
 assert(temp_stop_callback_list_length==0);
 module_record_poll_limit(stopped);assert(temp_stop_callback_list_length==0);
 receive_worker=true;module_record_start_send(data);receive_worker=false;
 assert(send_flag && !closed && temp_stop_callback_list_length==0);
 module_record_poll_limit(stopped);assert(!closed && !completed && cues==0);
 /* Explicit manual stop before ACK still resumes off the receive worker. */
 stop_pending_until_server_ready=1;frozen=true;
 temp_stop_callback_list[0]=stopped;temp_stop_callback_list_length=1;
 module_record_poll_limit(stopped);
 assert(closed==1 && completed==1 && !stop_pending_until_server_ready);
 module_record_poll_limit(stopped);assert(closed==1 && completed==1);
 tail_ready=true;module_record_poll_limit(stopped);
 assert(closed==1 && completed==1 && !tail_ready && failures==1);
 module_record_poll_limit(stopped);assert(closed==1 && completed==1 && failures==1);
 assert(!locks[1] && !locks[2]);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            test, elf = Path(tmp) / 'resume.c', Path(tmp) / 'resume.elf'
            test.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(test), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
