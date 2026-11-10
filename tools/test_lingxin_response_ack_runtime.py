"""A current TTS response supersedes the upload ACK wait, never another task."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_task_continue_guard import ROOT, CC, RUN
from test_offline_action import c_function
from test_recorder_manager_lifecycle_contract import function_body


class ResponseAckRuntimeTest(unittest.TestCase):
    def test_current_response_ack_and_late_end_audio_are_epoch_bound(self):
        source = (ROOT/'third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c').read_text(encoding='utf-8')
        functions = ''
        for name in ('complete_upload_ack_from_response', 'note_audio_response_progress'):
            if 'static void '+name+'(' in source:
                functions += c_function(source, name)+'\n'
            elif name == 'complete_upload_ack_from_response':
                functions += 'static void complete_upload_ack_from_response(VoiceChatHandler *h,uint32_t epoch) {}\n'
            else:
                functions += 'static void note_audio_response_progress(VoiceChatHandler *h) {}\n'
        binary = function_body(source.replace('if (isBinary)', 'void binary(void)'), 'binary')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX 1
#define lingxin_log_ut_with_args(...) ((void)0)
#define lingxin_server_diag_timing_mark(...) ((void)0)
typedef struct {bool waitTerminate,closing;} VoiceChatHandler;
static VoiceChatHandler h,old;
static VoiceChatHandler *globalHandler=&h;
static bool isTaskStarted=true,s_audio_response_open=true,s_waiting_first_tts;
static bool s_end_audio_sent=true,s_audio_ended_received,s_end_audio_ack_grace_expired;
static bool s_task_stage_end_pending=true,s_cloud_vad_upload_allowed;
static uint32_t s_recovery_epoch=9,s_response_audio_epoch,s_end_audio_ack_watchdog_epoch=9;
static uint32_t s_end_audio_ack_deadline_ms=5000,s_first_tts_deadline_ms;
static uint32_t s_diag_download_audio_chunk_count,s_diag_download_audio_bytes_total;
static unsigned end_tasks,received;
static unsigned voice_chat_current_epoch(void) {return s_recovery_epoch;}
static void voice_chat_handler_guard_lock(void) {}
static void voice_chat_handler_guard_unlock(void) {}
static void notify_audio_upload_acked(VoiceChatHandler *p) {s_audio_ended_received=true;}
static void sendEndTask(VoiceChatHandler *p) {++end_tasks;}
static void state_machine_receive_mp3_data(void *p,int n) {++received;}
''' + functions + r'''
static void binary(VoiceChatHandler *handler,int len) {const char *data="tts";
''' + binary + r'''
}
int main(void) {
 binary(&old,3); assert(!s_audio_ended_received && !end_tasks);
 s_audio_response_open=false;binary(&h,3);assert(!s_audio_ended_received);
 s_audio_response_open=true;h.waitTerminate=true;binary(&h,3);assert(!s_audio_ended_received);
 h.waitTerminate=false;binary(&h,0);assert(!s_audio_ended_received);
 binary(&h,3);assert(s_audio_ended_received && s_end_audio_ack_deadline_ms==0 && end_tasks==1);
 binary(&h,3);assert(end_tasks==1);
 /* Next round cannot inherit audio proof from the previous response. */
 ++s_recovery_epoch;s_end_audio_ack_watchdog_epoch=10;s_audio_ended_received=false;
 s_end_audio_ack_deadline_ms=5000;s_task_stage_end_pending=true;
 complete_upload_ack_from_response(&h,9);
 complete_upload_ack_from_response(&h,10);
 assert(!s_audio_ended_received && s_end_audio_ack_deadline_ms==5000 && end_tasks==1);
 /* Uploading / a pre-end response must not cancel an armed future ACK. */
 s_end_audio_sent=false;binary(&h,3);assert(!s_audio_ended_received && s_response_audio_epoch==10);
 s_end_audio_sent=true;s_cloud_vad_upload_allowed=true;
 complete_upload_ack_from_response(&h,10);assert(!s_audio_ended_received);
 /* Recorder-close may queue end_audio after TTS. Reuse proof for this task only. */
 s_cloud_vad_upload_allowed=false;s_audio_response_open=false;
 s_waiting_first_tts=true;s_first_tts_deadline_ms=30000;
 complete_upload_ack_from_response(&h,10);
 assert(s_audio_ended_received && !s_end_audio_ack_deadline_ms && end_tasks==2);
 assert(!s_waiting_first_tts && !s_first_tts_deadline_ms);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'ack.c',Path(temp)/'ack.elf'
            cfile.write_text(code,encoding='utf-8')
            subprocess.run([str(CC),'--specs=sim.specs','-std=c11',str(cfile),'-o',str(elf)],check=True)
            subprocess.run([str(RUN),str(elf)],check=True,timeout=30)


if __name__ == '__main__':
    unittest.main()
