"""Execute the production event branches with transport/lifecycle sinks.

The embedded engine cannot link on a host; compile its unmodified branch bodies
with the RISC-V simulator. In particular, catch late task_stage_end bypassing
the audio_ended barrier after the acknowledgement timeout.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_task_continue_guard import ROOT, CC, RUN
from test_recorder_manager_lifecycle_contract import function_body


def event_body(source, name):
    start = source.index('{', source.index(f'strcmp(event, "{name}")'))
    depth = 0
    for end in range(start, len(source)):
        depth += (source[end] == '{') - (source[end] == '}')
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(name)


class StageEndRuntimeTest(unittest.TestCase):
    def test_stage_end_never_closes_unacknowledged_audio(self):
        source = (ROOT / 'third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c').read_text(encoding='utf-8')
        harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
static bool s_end_audio_sent, s_audio_ended_received;
static bool isTaskStarted=true,s_task_audio_input;
static bool s_end_audio_ack_grace_expired, s_task_stage_end_pending;
static uint32_t s_end_audio_ack_deadline_ms, s_end_audio_ack_watchdog_epoch;
static void *handler;
static int ended, failed;
#define LINGXIN_WARN 0
#define lingxin_server_diag_timing_mark(...) ((void)0)
#define lingxin_log_ut_with_args(...) ((void)0)
static void sendEndTask(void *p) { (void)p; ended++; }
static void on_voice_chat_init_fail_for_epoch(uint32_t epoch) { (void)epoch; failed++; }
''' + '\nstatic void notify_audio_upload_acked(void *handler) {\n' + function_body(source, 'notify_audio_upload_acked') + '\n}\nstatic void stage(void)\n' + event_body(source, 'task_stage_end') + '\nstatic void ack(void)\n' + event_body(source, 'audio_ended') + r'''
int main(void) {
    /* Text task has no end_audio pair. */
    stage(); assert(ended == 1); ended = 0;
    s_task_audio_input=true;stage();assert(!ended && s_task_stage_end_pending);
    ack();assert(!s_audio_ended_received && !ended);
    s_end_audio_sent = true;
    stage(); assert(ended == 0 && s_task_stage_end_pending);
    ack(); assert(ended == 1 && !s_task_stage_end_pending); ended = 0;
    s_audio_ended_received = false;
    s_end_audio_ack_grace_expired = true;
    stage(); assert(ended == 0 && failed == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cfile, elf = Path(directory) / 'stage.c', Path(directory) / 'stage.elf'
            cfile.write_text(harness, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', str(cfile), '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)


if __name__ == '__main__':
    unittest.main()
