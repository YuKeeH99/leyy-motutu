"""Run production boundary functions with deliberately synchronous/late callbacks."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_task_continue_guard import ROOT, CC, RUN
from test_recorder_manager_lifecycle_contract import function_body


def body(path, name):
    return "{\n" + function_body((ROOT / path).read_text(encoding="utf-8"), name) + "\n}"


def run_c(source, *defines):
    with tempfile.TemporaryDirectory() as directory:
        cfile, elf = Path(directory) / "race.c", Path(directory) / "race.elf"
        cfile.write_text(source, encoding="utf-8")
        subprocess.run([str(CC), "--specs=sim.specs", "-std=c11", *defines,
                        str(cfile), "-o", str(elf)], check=True)
        subprocess.run([str(RUN), str(elf)], check=True, timeout=20)


PROTOCOL = "third_party/AI2T_LingXinEngine/src/voice_chat/protocol_manager.c"
UPLOAD = "third_party/AI2T_LingXinEngine/src/voice_chat/state_upload_module_manager/chat_upload_manager.c"
COMMON = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define lingxin_log_ut(...) ((void)0)
#define lingxin_log_ut_with_args(...) ((void)0)
#define lingxin_log_error(...) ((void)0)
#define lingxin_log_debug(...) ((void)0)
#define lingxin_trace_set(...) ((void)0)
#define lingxin_server_diag_timing_mark(...) ((void)0)
#define core_node_record(...) ((void)0)
#define event_send_log(...) ((void)0)
/* This simulator lacks AMO instructions; interleavings are injected at the
 * external call boundaries, so a scalar add is sufficient in this harness. */
#define __atomic_add_fetch(p, n, order) (*(p) += (n))
'''


class RoundRacesTest(unittest.TestCase):
    def test_failure_revalidates_epoch_when_reserving_teardown(self):
        run_c(COMMON + r'''
#define __atomic_compare_exchange_n(p,e,n,w,s,f) cas(p,e,n)
static bool cas(uint32_t *p,uint32_t *e,uint32_t n){if(*p!=*e){*e=*p;return false;}*p=n;return true;}
static uint32_t s_recovery_epoch=7,s_connection_failure_pending_epoch;
static uint32_t s_start_task_deadline_ms,s_terminate_deadline_ms,s_first_tts_deadline_ms,s_end_audio_ack_deadline_ms;
static bool isAIResponseding=true,s_audio_response_open=true,isAudioSending=true,isTaskStarted=true;
static bool s_waiting_task_terminated,s_waiting_first_tts,s_task_stage_end_pending,s_diag_waiting_task_started;
static void *globalHandler;
static bool switch_at_lock=true,locked;
static int threads;
typedef unsigned lingxin_tid_t;
typedef struct {const char *name;int priority,stack_size;} lingxin_thread_param_t;
static void voice_chat_handler_guard_lock(void){assert(!locked);if(switch_at_lock){switch_at_lock=false;s_recovery_epoch++;}locked=true;}
static void voice_chat_handler_guard_unlock(void){assert(locked);locked=false;}
static void diag_note_task_started_wait_end(void*h,const char*s){}
static void *voice_chat_connection_failure_thread(void*p){return 0;}
static int lingxin_thread_create(lingxin_tid_t *id,lingxin_thread_param_t *p,void*(*fn)(void*),void *arg){assert(!locked);threads++;return 0;}
static void on_voice_chat_init_fail_for_epoch(uint32_t epoch)
''' + body(PROTOCOL,'on_voice_chat_init_fail_for_epoch') + r'''
int main(void){
 on_voice_chat_init_fail_for_epoch(7);
 assert(s_recovery_epoch==8 && isTaskStarted && isAudioSending && !threads && !s_connection_failure_pending_epoch && !locked);
 on_voice_chat_init_fail_for_epoch(8);
 assert(!isTaskStarted && !isAudioSending && threads==1 && s_connection_failure_pending_epoch==8 && !locked);
 on_voice_chat_init_fail_for_epoch(8);assert(threads==1 && !locked);
}
''')

    def test_binary_audio_only_enters_the_active_response_window(self):
        source = (ROOT / PROTOCOL).read_text(encoding="utf-8")
        binary = function_body(source.replace("if (isBinary)", "void binary(void)"), "binary")
        run_c(COMMON + r'''
typedef struct {bool waitTerminate;} VoiceChatHandler;
static VoiceChatHandler h, old;
static VoiceChatHandler *globalHandler=&h;
static bool isTaskStarted, s_audio_response_open, s_waiting_first_tts;
static uint32_t s_first_tts_deadline_ms, s_diag_download_audio_chunk_count, s_diag_download_audio_bytes_total;
static int received;
static void note_audio_response_progress(VoiceChatHandler *h) {}
static void state_machine_receive_mp3_data(void *p, int n) {received++;}
static void binary(VoiceChatHandler *handler) {char *data="mp3"; int len=3;
''' + binary + r'''
}
int main(void) {
    binary(&h); assert(!received);
    isTaskStarted=true; binary(&h); assert(!received);
    s_audio_response_open=true; binary(&h); assert(received==1);
    binary(&old); assert(received==1);
    h.waitTerminate=true; binary(&h); assert(received==1);
    h.waitTerminate=false; s_audio_response_open=false; binary(&h); assert(received==1);
}
''')

    def test_rejected_audio_aborts_instead_of_silently_splicing_sentence(self):
        run_c(COMMON + r'''
typedef struct {void *websocket;} VoiceChatHandler;
static VoiceChatHandler h;
static bool isTaskStarted=true, s_cloud_vad_upload_allowed=true, isAudioSending, s_waiting_task_terminated;
#define WEBSOCKET_SEND_CANCELLED (-2)
static uint32_t s_recovery_epoch=7, s_diag_binary_audio_drop_before_task_started;
static uint32_t s_diag_binary_audio_send_count, s_diag_binary_audio_send_bytes;
static int failures, leases, send_result;
static VoiceChatHandler *voice_chat_handler_acquire(void) {leases++; return &h;}
static void voice_chat_handler_release(VoiceChatHandler *p) {leases--;}
static int hook_websocket_send_binary_guarded(void *w, void *d, int n, bool(*current)(void*),void *arg) {assert(current(arg));return send_result;}
static void on_voice_chat_init_fail_for_epoch(uint32_t epoch) {assert(epoch==7 && !leases); failures++;}
static bool voice_chat_audio_send_current(void *arg)
''' + body(PROTOCOL, "voice_chat_audio_send_current") + r'''
static void voiceChatSendAudio(void *audioData, int dataSize)
''' + body(PROTOCOL, "voiceChatSendAudio") + r'''
int main(void) {
    char pcm[1280]={0};
    voiceChatSendAudio(pcm, sizeof(pcm));
    assert(failures==1 && !leases && !s_diag_binary_audio_send_count);
    send_result=sizeof(pcm); voiceChatSendAudio(pcm, sizeof(pcm));
    assert(failures==1 && !leases && s_diag_binary_audio_send_count==1);
}
''', "-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX")

    def test_stop_before_task_ack_freezes_capture_immediately(self):
        run_c(COMMON + r'''
#define LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH 5
typedef void (*LingxinRecorderStopCallback)(bool);
typedef void (*lingxin_recorder_callback_t)(int);
static int record_close_stop_ticket;
static unsigned int record_close_stop_epoch;
static void *lingxin_recorder = (void *)1;
static int stop_pending_until_server_ready, server_ready, record_close_mutex, record_close_callback_list_mutex;
static int temp_stop_callback_list_length, closes;
static LingxinRecorderStopCallback temp_stop_callback_list[5];
static bool frozen;
static unsigned int voice_chat_current_epoch(void) {return 7;}
static int register_stop_callback_once(unsigned int epoch, LingxinRecorderStopCallback cb, bool persistent) {return 1;}
static void lingxin_mutex_lock(int x) {}
static void lingxin_mutex_unlock(int x) {}
static void lingxin_clean_recorder_prepare_stop(void *r, bool drain) {frozen = true;}
static void record_close_callback_for_stop_wait_send_left(int r) {}
static void record_close_callback_for_stop(int r) {}
static void lingxin_recorder_close(void *r, lingxin_recorder_callback_t cb) {closes++;}
static void module_record_stop(int wait_send_left, LingxinRecorderStopCallback stop_callback, bool persistent)
''' + body("third_party/AI2T_LingXinEngine/src/voice_chat/recorder_manager.c", "module_record_stop_impl") + r'''
int main(void) {
    module_record_stop(1, NULL, true);
    assert(frozen && stop_pending_until_server_ready && !closes);
    /* Delayed ACK resumes only draining, not microphone capture. */
    server_ready = 1; stop_pending_until_server_ready = 0;
    module_record_stop(1, NULL, true);
    assert(frozen && closes == 1);
}
''', "-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX")

    def test_late_playback_event_cannot_complete_a_new_response(self):
        run_c(COMMON + r'''
#define LOGI(...) try_interrupt()
#define LOGW(...) ((void)0)
#define lisa_mem_free(p) free(p)
#define VOICE_MSG_PLAYER_TTS_STOPED 1
enum {Lingxin_Download_Audio_PlayEnd, Lingxin_Download_Audio_TerminateEnd};
typedef struct {void (*callback)(int, void *); int event; void *user_data;
                const char *reason; uint32_t generation;} lingxin_playback_event_ctx_t;
static uint32_t s_playback_generation = 2;
static int callbacks, completions, notifications;
static bool restart_in_callback;
static bool dispatch_locked, interrupt_at_log, interrupt_pending;
static bool lingxin_playback_dispatch_lock(void) {dispatch_locked=true; return true;}
static void lingxin_playback_dispatch_unlock(void) {
    dispatch_locked=false;
    if (interrupt_pending) {s_playback_generation++; interrupt_pending=false;}
}
static void try_interrupt(void) {
    if (!interrupt_at_log) return;
    interrupt_at_log=false;
    if (dispatch_locked) interrupt_pending=true; else s_playback_generation++;
}
static void callback(int event, void *u) {
    assert(s_playback_generation==2);
    callbacks++; if (restart_in_callback) s_playback_generation++;
}
static void lingxin_server_tts_playback_complete(void) {completions++;}
static void voice_msg_pub_timeout(int e, void *p, int n, int t) {notifications++;}
static void lingxin_playback_event_thread(void *arg)
''' + body("src/server/lingxin_server/adapter/lingxin_audio_adapter.c", "lingxin_playback_event_thread") + r'''
static void dispatch(uint32_t generation, int event) {
    lingxin_playback_event_ctx_t *p = calloc(1, sizeof(*p));
    p->callback = callback; p->event = event; p->generation = generation;
    lingxin_playback_event_thread(p);
}
int main(void) {
    dispatch(1, Lingxin_Download_Audio_PlayEnd);
    dispatch(1, Lingxin_Download_Audio_TerminateEnd);
    assert(!callbacks && !completions && !notifications);
    dispatch(2, Lingxin_Download_Audio_PlayEnd);
    assert(callbacks == 1 && completions == 1 && notifications == 1);
    restart_in_callback = true;
    dispatch(2, Lingxin_Download_Audio_PlayEnd);
    assert(callbacks == 2 && completions == 1 && notifications == 1);
    s_playback_generation=2; restart_in_callback=false; interrupt_at_log=true;
    dispatch(2, Lingxin_Download_Audio_PlayEnd);
    assert(callbacks==3 && completions==2 && s_playback_generation==3);
}
''')

    def test_start_ack_before_send_returns_stays_ready(self):
        run_c(COMMON + r'''
typedef struct {char *taskId;} Config;
typedef struct {Config *chat_config; void *websocket; bool waitTerminate,closing;unsigned active_operations;} VoiceChatHandler;
typedef struct {bool is_schedule_timer_task;} ChatStartNewParams;
static Config config;
static VoiceChatHandler storage = {&config, 0, false}, *globalHandler;
static bool isAIResponseding, isAudioSending, isTaskStarted, isWebsocketConnected;
static bool s_handler_close_finalizing;
static uint32_t s_recovery_epoch, s_connection_failure_pending_epoch;
static int sends, failures;
static bool lifecycle_locked,failure_at_lock;
static void voice_chat_handler_guard_lock(void){
 assert(!lifecycle_locked);lifecycle_locked=true;
 if(failure_at_lock){failure_at_lock=false;s_connection_failure_pending_epoch=s_recovery_epoch;}
}
static void voice_chat_handler_guard_unlock(void){assert(lifecycle_locked);lifecycle_locked=false;}
static void voice_chat_handler_release(VoiceChatHandler*h){if(h){assert(h->active_operations);h->active_operations--;}}
static bool fill_config_params(Config *c, ChatStartNewParams *p) {return true;}
static bool module_voiceChat_exit(void) {globalHandler = NULL; return true;}
static void on_voice_chat_init_fail(void) {failures++;}
static bool sendStartTask(VoiceChatHandler *h) {
    sends++;
    /* Real websocket worker can run before send returns. */
    isTaskStarted = true; isAIResponseding = true; isAudioSending = true;
    return true;
}
static bool voiceChatCreate(ChatStartNewParams *p) {
    globalHandler = &storage; isWebsocketConnected = true;
    return sendStartTask(globalHandler);
}
static bool voice_chat_start_new(ChatStartNewParams *params)
''' + body(PROTOCOL, "voice_chat_start_new") + r'''
int main(void) {
    ChatStartNewParams p = {0};
    assert(voice_chat_start_new(&p));
    assert(isTaskStarted && isAudioSending && isAIResponseding);
    isAIResponseding = false;
    assert(voice_chat_start_new(&p));
    assert(isTaskStarted && isAudioSending && isAIResponseding);
    assert(sends == 2 && !failures);
    isAIResponseding=false; s_connection_failure_pending_epoch=s_recovery_epoch;
    assert(!voice_chat_start_new(&p) && sends==2);
    s_connection_failure_pending_epoch=0;failure_at_lock=true;
    unsigned previous=s_recovery_epoch;
    assert(!voice_chat_start_new(&p) && sends==2 && s_recovery_epoch==previous && !lifecycle_locked);
}
''')

    def test_terminate_pending_start_and_fast_ack(self):
        run_c(COMMON + r'''
typedef struct {char *taskId;} Config;
typedef struct {Config *chat_config; void *websocket; bool waitTerminate;} VoiceChatHandler;
static Config config = {"round-A"};
static VoiceChatHandler storage = {&config, 0, false}, *globalHandler = &storage;
static bool isAIResponseding = true, isAudioSending, isTaskStarted, isWebsocketConnected = true;
static bool waitTerminateOrEndSuccess, s_diag_waiting_task_started = true;
static bool s_waiting_task_terminated, s_cloud_vad_upload_allowed, s_audio_response_open;
static uint32_t s_recovery_epoch = 1, s_start_task_deadline_ms;
static int sent, failures;
static void on_voice_chat_init_fail_for_epoch(uint32_t e) {failures++;}
static void on_voice_chat_init_fail(void) {failures++;}
static void arm_terminate_task_watchdog(void) {s_waiting_task_terminated = true;}
static bool hook_websocket_send_text(void *w, const char *m) {
    assert(storage.waitTerminate && waitTerminateOrEndSuccess && s_waiting_task_terminated);
    sent++;
    /* ACK may synchronously restart B: old send stack must not overwrite B. */
    storage.waitTerminate = false; waitTerminateOrEndSuccess = false;
    s_waiting_task_terminated = false; isAIResponseding = true; isTaskStarted = true;
    return true;
}
static int voiceChatTerminateCheck(void)
''' + body(PROTOCOL, "voiceChatTerminateCheck") + r'''
int main(void) {
    assert(voiceChatTerminateCheck() == 1);
    assert(sent == 1 && !storage.waitTerminate && !waitTerminateOrEndSuccess);
    assert(!s_waiting_task_terminated && isTaskStarted && !failures);
}
''', "-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX")

    def test_end_audio_and_end_task_do_not_overwrite_fast_acks(self):
        run_c(COMMON + r'''
static void notify_recording_ended(uint32_t epoch, bool local_speech) {}
typedef struct {char *taskId;} Config;
typedef struct {Config *chat_config; void *websocket; bool waitTerminate;} VoiceChatHandler;
static Config config = {"round-A"};
static VoiceChatHandler storage = {&config, 0, false}, *globalHandler = &storage;
static bool isAudioSending = true, isTaskStarted = true, waitTerminateOrEndSuccess;
static bool s_end_audio_sent, s_audio_ended_received, s_task_stage_end_pending;
static bool s_end_audio_ack_grace_expired, s_end_task_sent, s_waiting_first_tts;
static bool s_cloud_vad_upload_allowed;
static uint32_t s_recovery_epoch = 1, s_end_audio_ack_watchdog_epoch, s_end_audio_ack_deadline_ms;
#define LINGXIN_END_AUDIO_ACK_TIMEOUT_MS 5000
/* This fixture has no TTS payload; inference is covered separately. */
static void complete_upload_ack_from_response(VoiceChatHandler *h, uint32_t epoch) {}
static void arm_first_tts_watchdog(void) {s_waiting_first_tts = true;}
static void on_voice_chat_init_fail_for_epoch(uint32_t e) {assert(0);}
static bool hook_websocket_send_text(void *w, const char *m) {
    if (strstr(m, "end_audio")) {
        assert(s_end_audio_sent && s_waiting_first_tts && s_task_stage_end_pending);
        s_audio_ended_received = true; s_end_audio_ack_deadline_ms = 0;
        s_waiting_first_tts = false;
    } else {
        assert(s_end_task_sent && waitTerminateOrEndSuccess);
        /* task_ended followed by the next round's initialization */
        s_end_task_sent = false; waitTerminateOrEndSuccess = false;
    }
    return true;
}
static void voiceChatStopSendAudio(void)
''' + body(PROTOCOL, "voiceChatStopSendAudio") + "\nstatic void sendEndTask(VoiceChatHandler *handler)\n" + body(PROTOCOL, "sendEndTask") + r'''
int main(void) {
    s_task_stage_end_pending=true; /* Early stage_end must survive end_audio. */
    voiceChatStopSendAudio();
    assert(s_audio_ended_received && !s_end_audio_ack_deadline_ms && !s_waiting_first_tts);
    sendEndTask(globalHandler);
    assert(!s_end_task_sent && !waitTerminateOrEndSuccess);
}
''', "-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX")

    def test_failed_recorder_callbacks_never_report_success(self):
        run_c(COMMON + r'''
enum {State_Event_Upload_InitEnd, State_Event_Upload_CloseEnd, State_Event_Upload_TerminateEnd};
static int events, ends, failures;
static unsigned s_cancelled_record_stop_epoch;
static unsigned voice_chat_current_epoch(void) {return 7;}
static void state_machine_run_event_with_payload(int e, void *p) {events++;}
static void voiceChatStopSendAudio(void) {ends++;}
static void voice_chat_fail_current_round(void) {failures++;}
static void on_record_init(bool success)
''' + body(UPLOAD, "on_record_init") + "\nstatic void on_record_stop(bool success)\n" + body(UPLOAD, "on_record_stop") + "\nstatic void on_record_terminate(bool success)\n" + body(UPLOAD, "on_record_terminate") + r'''
int main(void) {
    on_record_init(false); on_record_stop(false); on_record_terminate(false);
    assert(events == 0 && ends == 0 && failures == 3);
    on_record_init(true); on_record_stop(true); on_record_terminate(true);
    assert(events == 3 && ends == 1 && failures == 3);
    s_cancelled_record_stop_epoch=7;on_record_stop(true);on_record_stop(false);
    assert(events == 3 && ends == 1 && failures == 3);
}
''')

    def test_cloud_vad_does_not_arm_uninformed_local_idle_timer(self):
        source = COMMON + r'''
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
static void *s_voice_idle_exit_timer = (void *)1;
static unsigned s_voice_idle_exit_timeout_ms = 30000;
static int armed;
static bool voice_cloud_session_is_running(const char *r) {return true;}
static void voice_idle_exit_timeout_refresh_from_kv(void) {}
static int xTimerIsTimerActive(void *t) {return 0;}
static int xTimerStop(void *t, int w) {return 1;}
static int xTimerChangePeriod(void *t, unsigned p, int w) {armed++; return 1;}
static void voice_idle_exit_timer_start(void)
''' + body("src/category/comm/msgs/voice_cloud_msg.c", "voice_idle_exit_timer_start") + r'''
int main(void) {
    voice_idle_exit_timer_start();
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX)
    assert(armed == 0);
#else
    assert(armed == 1);
#endif
}
'''
        run_c(source, "-DCONFIG_VOICE_LINK_LINGXIN", "-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX")
        run_c(source)


if __name__ == "__main__":
    unittest.main()
