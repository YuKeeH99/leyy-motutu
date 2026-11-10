#include <limits.h>
#include <stdlib.h> 
#include "lingxin_cbuffer.h"
#include "lingxin_mutex.h"
#include "lingxin_semaphore.h"
#include "lingxin_thread.h"
#include "lingxin_recorder.h"
#include "chat_state_machine.h"
#include "lingxin_log.h"
#include <stdio.h>
#include "lingxin_system_time.h"
#include "lingxin_memory.h"
#include "upload_record_interface.h"
#include "lingxin_test_runner.h"
#include "lingxin_recorder_manager.h"
#include "lingxin_protocol_manager.h"
#include "lingxin_trace.h"
#include <string.h>
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
#include "lingxin_clean_recorder.h"
#endif
#define LINGXIN_RECORD_MAX_BYTES 1280

#define LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH 5

static lingxin_mutex_t send_thread_mutex = NULL;
static lingxin_mutex_t record_close_mutex = NULL;
static lingxin_mutex_t record_close_callback_list_mutex = NULL;

static lingxin_recorder_t *lingxin_recorder = NULL;
static int recorder_ready = 0;
static int server_ready = 0;
static int stop_pending_until_server_ready = 0;
static int send_flag = 0;
static LingxinCircularBuffer* send_cbuf = NULL;
static lingxin_semaphore_t send_r_sem = NULL;
static volatile int send_thread_running = 0;
static int send_thread_stop_ticket;
static int record_close_stop_ticket;
static unsigned int send_thread_stop_epoch;
static unsigned int record_close_stop_epoch;
static volatile int send_thread_stop_flag = 0; // 0: 不退出 1: 立刻退出 2: 等待发完再退出

static int send_uni_size = 0;
// Debug/embedded-friendly default: avoid allocating ~128KB buffer during init.
static int send_cbuf_scale = 16;

typedef void (*SendThreadStopCallback)();

static void inner_record_init();
static void record_open_callback(int result);
static void record_close_callback_for_start(int result);
static void record_close_callback_for_stop(int result);
static void record_close_callback_for_stop_wait_send_left(int result);

static void* send_record(void *arg);
static int stop_send_thread(int wait_send, unsigned int epoch, int ticket);
static int start_send_thread();

static int get_frame_size();
static void set_send_flag();
static void cleanup_send_thread_sync_objects(void);

/****************** 录音模块初始化 ******************/
int module_record_manager_init(int custom_send_uni_size, int custom_send_cbuf_scale) {
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "recorder_manager_init", "send_uni_size is %d, send_cbuf_scale is %d", custom_send_uni_size, custom_send_cbuf_scale);
    if (!send_thread_mutex) {
        send_thread_mutex = lingxin_mutex_create();
    }
    if (!record_close_mutex) {
        record_close_mutex = lingxin_mutex_create();
    }
    if (!record_close_callback_list_mutex) {
        record_close_callback_list_mutex = lingxin_mutex_create();
    }
    if (custom_send_uni_size) {
        send_uni_size = custom_send_uni_size;
    }
    if (custom_send_cbuf_scale) {
        send_cbuf_scale = custom_send_cbuf_scale;
    }
    send_uni_size = get_frame_size();
    if (send_uni_size <= 0 || send_uni_size > LINGXIN_RECORD_MAX_BYTES) return -1;
    lingxin_checkpoint_report_with_int("lingxin_test_send_uni_size_check", send_uni_size);
    lingxin_checkpoint_report_with_int("lingxin_test_send_cbuf_scale_check", send_cbuf_scale);
    if (!send_cbuf) {
        send_cbuf = lingxin_cbuffer_init(send_cbuf_scale, send_uni_size + sizeof(uint32_t));
        if (send_cbuf == NULL) {
            lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_init_fail");
            return -1;
        }
    }
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_init_success");
    return 0;
}


/****************** 录音器开启 ******************/
LingxinRecorderStartCallback temp_start_callback = NULL;
static void run_start_callback(bool is_success) {
    if (temp_start_callback) {
        temp_start_callback(is_success);
        temp_start_callback = NULL;
    } else {
        lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_run_start_callback_fail");
    }
}
int module_record_start(LingxinRecorderStartCallback start_callback) {
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_recorder_start_called");
    lingxin_log_debug("recorder_manager_recorder_start_before_close");
    lingxin_trace_set("rec:start_enter");
    temp_start_callback = start_callback;
    recorder_ready = 0;
    server_ready = 0;
    stop_pending_until_server_ready = 0;
    set_send_flag();
    // [开启录音-1]关闭可能存在的录音
    lingxin_trace_set("rec:before_close_lock");
    lingxin_mutex_lock(record_close_mutex);
    lingxin_trace_set("rec:after_close_lock");
    if (lingxin_recorder == NULL) {
        lingxin_trace_set("rec:no_prev_recorder");
        record_close_callback_for_start(0);
    } else {
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_close_before_recorder_start");
        lingxin_trace_set("rec:close_prev_recorder");
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
        /* Restart cancels a previous failed drain; server_ready was cleared
         * above, so waiting for that drain here would never make progress. */
        lingxin_clean_recorder_prepare_stop(lingxin_recorder, false);
#endif
        lingxin_recorder_close(lingxin_recorder, record_close_callback_for_start);
    }
    lingxin_trace_set("rec:start_return");
    return 0;
}
static void record_close_callback_for_start(int result) {
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_close_callback_before_recorder_start");
    lingxin_trace_set(result == 0 ? "rec:close_cb_start_ok" : "rec:close_cb_start_fail");
    if (result == 0) {
        if (lingxin_recorder != NULL) {
            lingxin_log_ut(LINGXIN_DEBUG, "record_adapter_recorder_destory_before_recorder_start");
            lingxin_recorder_destroy(lingxin_recorder);
            lingxin_recorder = NULL;
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_destory_recorder_success_before_recorder_start");
        }
        lingxin_mutex_unlock(record_close_mutex);
        // [开启录音-2]退出可能存在的录音发送线程
        int res = stop_send_thread(0, voice_chat_current_epoch(), 0);
        if (res == 0) {
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_stop_send_thread_success_before_recorder_start");
            // [开启录音-3]初始化新的录音
            inner_record_init();
        } else {
            lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_stop_send_thread_fail_before_recorder_start");
            run_start_callback(false);
        }
    } else {
        lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_close_recorder_fail_before_recorder_start");
        lingxin_mutex_unlock(record_close_mutex);
        run_start_callback(false);
    }
}
static void inner_record_init() {
    lingxin_log_debug("recorder_manager_inner_record_init_begin");
    lingxin_trace_set("chat:recorder_inner_init");
    // [开启录音-3.1]开启新的录音发送线程
    int res = start_send_thread();
    if (res == 0) {
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_start_send_thread_success");
        // [开启录音-3.2]开启录音
        lingxin_mutex_lock(record_close_mutex);
        recorder_ready = 1;
        set_send_flag();
        lingxin_recorder = lingxin_recorder_create();
        lingxin_recorder_open_param_t props = {
            .frame_size = send_uni_size,
        };
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_open");
        lingxin_trace_set("chat:recorder_before_open");
        lingxin_log_debug("recorder_manager_before_recorder_open frame_size=%d", send_uni_size);
        lingxin_recorder_open(lingxin_recorder, &props, record_open_callback);
    } else {
        lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_start_send_thread_fail");
        run_start_callback(false);
    }
}
static void record_open_callback(int result) {
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_open_callback"); 
    lingxin_trace_set(result == 0 ? "rec:open_cb_ok" : "rec:open_cb_fail");
    if (result == 0) {
        lingxin_trace_set("chat:recorder_open_ok");
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_open_recorder_success");
        // [开启录音-4]通知状态机录音模块启动已完成
        lingxin_mutex_unlock(record_close_mutex);
        run_start_callback(true);
    } else {
        lingxin_trace_set("chat:recorder_open_fail");
        lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_open_recorder_fail");
        lingxin_mutex_unlock(record_close_mutex);
        run_start_callback(false);
    }
}


/****************** 录音开始发送 ******************/

LingxinRecorderDataCallback temp_data_callback = NULL;
void module_record_start_send(LingxinRecorderDataCallback data_callback) {
    lingxin_log_ut(LINGXIN_DEBUG, "record_manager_start_send");
    lingxin_mutex_lock(record_close_mutex);
    temp_data_callback = data_callback;
    server_ready = 1;
#ifndef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    bool resume_stop = stop_pending_until_server_ready != 0;
    stop_pending_until_server_ready = 0;
#endif
    lingxin_mutex_unlock(record_close_mutex);
    set_send_flag();
#ifndef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    if (resume_stop) {
        lingxin_trace_set("chat:record_stop_resume_after_server_ready");
        module_record_stop(1, NULL);
    }
#endif
    /* In half duplex this callback runs on the socket receive worker, which
     * also drains its TX queue. Let the watchdog resume a frozen capture so
     * a cold capture backlog cannot block the worker that must send it. */
} 

bool module_record_is_server_ready(void) {
    return server_ready != 0;
}

/****************** 录音器结束 ******************/
static LingxinRecorderStopCallback temp_stop_callback_list[LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH];
static unsigned int temp_stop_callback_epochs[LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH];
static LingxinRecorderStopCallback claimed_stop_callbacks[LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH];
static bool claimed_stop_persistent[LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH];
static unsigned int claimed_stop_epoch;
static int claimed_stop_count;
static int next_stop_ticket;
static int temp_stop_callback_tickets[LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH];
static int temp_stop_callback_list_length = 0;

/* Retain claims after dispatch: a late VAD must not complete the same stop
 * twice. Termination has a different callback and may still preempt a drain. */
static int register_stop_callback_once(unsigned int epoch, LingxinRecorderStopCallback callback, bool persistent)
{
    if (!callback) return 1;
    lingxin_mutex_lock(record_close_callback_list_mutex);
    if (epoch != voice_chat_current_epoch()) {
        lingxin_mutex_unlock(record_close_callback_list_mutex);
        return 0;
    }
    if (claimed_stop_epoch != epoch) {
        claimed_stop_epoch = epoch;
        claimed_stop_count = 0;
        next_stop_ticket = 0;
    }
    for (int i = 0; i < claimed_stop_count; ++i) {
        if (claimed_stop_callbacks[i] == callback) {
            lingxin_mutex_unlock(record_close_callback_list_mutex);
            return 0;
        }
    }
    /* Retire queued callbacks from superseded rounds before checking space. */
    int kept = 0;
    for (int i = 0; i < temp_stop_callback_list_length; ++i) {
        if (temp_stop_callback_epochs[i] == epoch) {
            temp_stop_callback_tickets[kept] = temp_stop_callback_tickets[i];
            temp_stop_callback_epochs[kept] = epoch;
            temp_stop_callback_list[kept++] = temp_stop_callback_list[i];
        }
    }
    temp_stop_callback_list_length = kept;
    if (kept >= LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH ||
        claimed_stop_count >= LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH || next_stop_ticket == INT_MAX) {
        lingxin_mutex_unlock(record_close_callback_list_mutex);
        return -1;
    }
    claimed_stop_persistent[claimed_stop_count] = persistent;
    claimed_stop_callbacks[claimed_stop_count++] = callback;
    int ticket = ++next_stop_ticket;
    temp_stop_callback_tickets[kept] = ticket;
    temp_stop_callback_epochs[kept] = epoch;
    temp_stop_callback_list[kept] = callback;
    ++temp_stop_callback_list_length;
    lingxin_mutex_unlock(record_close_callback_list_mutex);
    return ticket;
}

static void run_stop_callback(bool is_success, unsigned int completing_epoch, int completing_ticket) {
    LingxinRecorderStopCallback completed[LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH];
    int count = 0, kept = 0;
    lingxin_mutex_lock(record_close_callback_list_mutex);
    for (int i = 0; i < temp_stop_callback_list_length; ++i) {
        if (temp_stop_callback_epochs[i] == completing_epoch &&
            temp_stop_callback_tickets[i] <= completing_ticket) {
            completed[count++] = temp_stop_callback_list[i];
        } else {
            temp_stop_callback_list[kept] = temp_stop_callback_list[i];
            temp_stop_callback_tickets[kept] = temp_stop_callback_tickets[i];
            temp_stop_callback_epochs[kept++] = temp_stop_callback_epochs[i];
        }
    }
    temp_stop_callback_list_length = kept;
    if (claimed_stop_epoch == completing_epoch) {
        int retained = 0;
        for (int i = 0; i < claimed_stop_count; ++i) {
            bool completed_now = false;
            for (int j = 0; j < count; ++j)
                if (completed[j] == claimed_stop_callbacks[i]) completed_now = true;
            if (claimed_stop_persistent[i] || !completed_now) {
                claimed_stop_callbacks[retained] = claimed_stop_callbacks[i];
                claimed_stop_persistent[retained++] = claimed_stop_persistent[i];
            }
        }
        claimed_stop_count = retained;
    }
    lingxin_mutex_unlock(record_close_callback_list_mutex);
    /* Snapshot this completion only. A callback can start a new round and
     * register new stops; the old completion must never consume those. */
    for (int i = 0; i < count; ++i) {
        if (completing_epoch != voice_chat_current_epoch()) return;
        completed[i](is_success);
    }
}
void module_record_poll_limit(LingxinRecorderStopCallback stop_callback) {
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    if (!record_close_mutex || !record_close_callback_list_mutex || !stop_callback) return;
    /* The existing close lock pins the recorder and generation until capture
     * is frozen and close begins. A stale deadline cannot stop a new round. */
    lingxin_mutex_lock(record_close_mutex);
    if (stop_pending_until_server_ready) {
        if (!server_ready) {
            lingxin_mutex_unlock(record_close_mutex);
            return;
        }
        stop_pending_until_server_ready = 0;
        lingxin_trace_set("chat:record_stop_resume_after_server_ready");
        if (lingxin_recorder) {
            lingxin_recorder_close(lingxin_recorder, record_close_callback_for_stop_wait_send_left);
        } else {
            record_close_callback_for_stop_wait_send_left(0);
        }
        return;
    }
    unsigned int epoch = voice_chat_current_epoch();
    bool reached = lingxin_clean_recorder_take_limit_stop(lingxin_recorder);
    if (reached) {
        lingxin_log_ut(LINGXIN_DEBUG, "record_limit_12000ms_freeze_then_silent_tail");
    }
    bool timed_out = lingxin_clean_recorder_limit_timed_out(lingxin_recorder);
    lingxin_mutex_unlock(record_close_mutex);
    if (timed_out) {
        lingxin_log_ut(LINGXIN_WARN, "record_limit_cloud_vad_wait_timeout");
        voice_chat_fail_waiting_cloud_vad(epoch);
    }
#else
    (void)stop_callback;
#endif
}

static void module_record_stop_impl(int wait_send_left, LingxinRecorderStopCallback stop_callback, bool persistent) {
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "recorder_manager_stop_called", "wait_send_left = %d", wait_send_left);
    if (wait_send_left && stop_pending_until_server_ready) {
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_stop_already_pending_server_ready");
        return;
    }
    unsigned int epoch = voice_chat_current_epoch();
    int claimed = register_stop_callback_once(epoch, stop_callback, persistent);
    if (claimed <= 0) {
        if (claimed < 0 && stop_callback) stop_callback(false);
        return;
    }
    lingxin_mutex_lock(record_close_mutex);
    if (epoch != voice_chat_current_epoch()) {
        lingxin_mutex_unlock(record_close_mutex);
        return;
    }
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    if (!wait_send_left) stop_pending_until_server_ready = 0;
    /* Stop is a capture boundary even before task_started. Serialize the
     * pending flag with ACK so neither side can miss the other's update. */
    lingxin_clean_recorder_prepare_stop(lingxin_recorder, wait_send_left != 0);
#endif
    if (wait_send_left && !server_ready) {
        record_close_stop_ticket = claimed;
        record_close_stop_epoch = epoch;
        stop_pending_until_server_ready = 1;
        lingxin_mutex_unlock(record_close_mutex);
        lingxin_trace_set("chat:record_stop_deferred_server_not_ready");
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_stop_deferred_server_not_ready");
        return;
    }
    record_close_stop_ticket = claimed;
    record_close_stop_epoch = epoch;
    // [正常结束录音-1]关闭可能存在的录音
    lingxin_recorder_callback_t callback = wait_send_left 
        ? record_close_callback_for_stop_wait_send_left 
        : record_close_callback_for_stop;
    if (lingxin_recorder == NULL) {
        callback(0);
    } else {
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_close_when_stop");
        lingxin_recorder_close(lingxin_recorder, callback);
    }
}
void module_record_stop(int wait_send_left, LingxinRecorderStopCallback stop_callback) {
    module_record_stop_impl(wait_send_left, stop_callback, false);
}

void module_record_stop_once(int wait_send_left, LingxinRecorderStopCallback stop_callback) {
    module_record_stop_impl(wait_send_left, stop_callback, true);
}

static void record_close_callback_for_stop(int result) {
    unsigned int epoch = record_close_stop_epoch;
    int ticket = record_close_stop_ticket;
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_close_callback_when_stop");
    if (result == 0) {
        if (lingxin_recorder != NULL) {
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_destroy_when_stop");
            lingxin_recorder_destroy(lingxin_recorder);
            lingxin_recorder = NULL;
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_destory_recorder_success_when_stop");
        }
        lingxin_mutex_unlock(record_close_mutex);
        // [正常结束录音-2]退出可能存在的录音发送线程
        int res = stop_send_thread(0, epoch, ticket);
        if (res == 0) {
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_stop_send_thread_success_when_stop");
            // [正常结束录音-3]通知状态机录音模块正常结束已完成
            run_stop_callback(true, epoch, ticket);
        } else {
            lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_stop_send_thread_fail_when_stop");
            run_stop_callback(false, epoch, ticket);
        }
    } else {
        lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_close_recorder_fail_when_stop");
        lingxin_mutex_unlock(record_close_mutex);
        run_stop_callback(false, epoch, ticket);
    }
}
static void record_close_callback_for_stop_wait_send_left(int result) {
    unsigned int epoch = record_close_stop_epoch;
    int ticket = record_close_stop_ticket;
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_close_callback_when_stop_wait");
    if (result == 0) {
        if (lingxin_recorder != NULL) {
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_recorder_destroy_when_stop_wait");
            lingxin_recorder_destroy(lingxin_recorder);
            lingxin_recorder = NULL;
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_destory_recorder_success_when_stop_wait");
        }
        lingxin_mutex_unlock(record_close_mutex);
        // [正常结束录音-2]退出可能存在的录音发送线程，但等待发完当前内容
        int res = stop_send_thread(1, epoch, ticket);
        if (res == 0) {
            lingxin_log_ut(LINGXIN_DEBUG, "recorder_manager_stop_send_thread_success_when_stop_wait");
            // [正常结束录音-3]通知状态机录音模块正常结束已完成
            // state_machine_run_event(State_Event_Record_Stop);
        } else {
            lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_stop_send_thread_fail_when_stop_wait");
            run_stop_callback(false, epoch, ticket);
        }
    } else {
        lingxin_log_ut(LINGXIN_ERROR, "recorder_manager_close_recorder_fail_when_stop_wait");
        lingxin_mutex_unlock(record_close_mutex);
        run_stop_callback(false, epoch, ticket);
    }
}


/****************** 发送线程（读缓存） ******************/
static int start_send_thread() {
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_thread_start_called");
    lingxin_mutex_lock(send_thread_mutex);

    // 0. 检查发送线程是否已经运行
    if (send_thread_running) {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_thread_start_fail", "send thread is already running.");
        goto start_send_thread_err;
    }
    // 1. 清空录音发送缓冲区
    if (send_cbuf == NULL) {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_thread_start_fail", "fail to get send buffer.");
        goto start_send_thread_err;
    }
    lingxin_cbuffer_reset(send_cbuf);
    // 2. 初始化信号量
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_semaphore_create");
    send_r_sem = lingxin_semaphore_create(0);
    if (send_r_sem == NULL) {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_thread_start_fail", "fail to initialize send semaphore.");
        goto start_send_thread_err;
    }
    // 3. 重置flag
    send_thread_stop_flag = 0;
    // 4. 创建录音发送线程
    long now_time = lingxin_get_timestamp_s();
    char name[16];
    snprintf(name, sizeof(name), "s_%ld", now_time);
    lingxin_thread_param_t thread_param = {
        .priority = 16,
        .stack_size = 4096*2,
        .name = name,
    };
    char *thread_buffer = lingxin_malloc(send_uni_size + sizeof(uint32_t));
    if (thread_buffer == NULL) {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_thread_start_fail", "sender buffer allocation failed.");
        goto start_send_thread_err;
    }
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_thread_create");
    /* Mark running before create: the new task may be scheduled immediately
     * and must never race with the creator overwriting its exit state. */
    send_thread_running = 1;
    lingxin_tid_t thread_id = 0;
    int ret = lingxin_thread_create(&thread_id, &thread_param, send_record, thread_buffer);
    if (ret == 0) {  // 检查线程创建是否成功
        lingxin_log_ut_with_args(LINGXIN_DEBUG, "recorder_thread_start_success", "pid is %d", thread_id);
    } else {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_thread_start_fail", "fail to create send thread, ret is %d.", ret);
        lingxin_free(thread_buffer);
        send_thread_running = 0;
        goto start_send_thread_err;
    }
    lingxin_mutex_unlock(send_thread_mutex);
    return 0;

start_send_thread_err:
    if (!send_thread_running) {
        cleanup_send_thread_sync_objects();
    }
    lingxin_mutex_unlock(send_thread_mutex);
    return -1;
}
static int stop_send_thread(int wait_send, unsigned int epoch, int ticket) {
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_thread_stop_called");
    bool complete_waiting_stop = false;
    bool wait_for_immediate_exit = false;

    lingxin_mutex_lock(send_thread_mutex);
    if (epoch != voice_chat_current_epoch()) {
        lingxin_mutex_unlock(send_thread_mutex);
        return 0;
    }
    if (send_thread_running) {
        send_thread_stop_epoch = epoch;
        send_thread_stop_ticket = ticket;
        lingxin_log_ut_with_args(LINGXIN_DEBUG, "recorder_thread_stop_start", "wait_send is %d", wait_send);
        if (wait_send) {
            if (send_thread_stop_flag == 1) {
                lingxin_mutex_unlock(send_thread_mutex);
                return 0; /* Cancellation cannot be turned back into a drain. */
            }
            send_thread_stop_flag = 2;
        } else {
            send_thread_stop_flag = 1;
        }
        lingxin_trace_set(send_r_sem ? "chat:record_stop_wake_sender" : "chat:record_stop_no_sender_sem");
        if (send_r_sem) {
            lingxin_semaphore_set_value(send_r_sem, 0);
            lingxin_semaphore_post(send_r_sem);
        }

        /* wait_send is completed by the sender after its queue drains. For an
         * immediate stop, wait only after releasing the lifecycle mutex. */
        if (!wait_send) {
            wait_for_immediate_exit = true;
        }
    } else if (wait_send) {
        // The sender may already have exited (for example after server VAD ended
        // the upload). Complete the newly queued stop request synchronously.
        lingxin_trace_set("chat:record_stop_sender_already_stopped");
        complete_waiting_stop = true;
    } else {
        cleanup_send_thread_sync_objects();
    }
    lingxin_mutex_unlock(send_thread_mutex);

    if (wait_for_immediate_exit) {
        int retry = 0;
        while (send_thread_running && retry < 250) {
            lingxin_thread_sleep(10);
            retry++;
        }

        if (send_thread_running) {
            lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_thread_stop_timeout",
                                     "sender did not exit after %d ms; preserve its resources",
                                     retry * 10);
            return -1;
        }
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_thread_stop_success");
    }

    if (complete_waiting_stop) {
        run_stop_callback(true, epoch, ticket);
    }
    return 0;
}
static void* send_record(void *arg) {
    unsigned int epoch = 0;
    int ticket = 0;
    lingxin_log_ut(LINGXIN_DEBUG, "recorder_thread_entry_function_called");
    int ret;
    bool complete_waiting_stop = false;
    char *buf = (char *)arg;
    if (buf == NULL) {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_thread_entry_function_fail", "buf malloc failed");
        goto send_record_thread_exit;
    }

    int count = 0;
	while(1) {
		lingxin_semaphore_pend(send_r_sem, 0);
        if (send_thread_stop_flag == 1) {
            lingxin_log_ut_with_args(LINGXIN_DEBUG, "recorder_thread_receive_stop_signal", "flag is %d", send_thread_stop_flag);
            break;
        }
        // 读取buffer的大小和写入一致，不存在写入数据不到current_frame_size的情况
        while (1) {
            if (send_thread_stop_flag == 1) {
                lingxin_log_ut_with_args(LINGXIN_DEBUG, "recorder_thread_receive_stop_signal", "flag is %d", send_thread_stop_flag);
                goto send_record_thread_exit;
            }
            lingxin_mutex_lock(send_thread_mutex);
            ret = send_flag ? lingxin_cbuffer_get(send_cbuf, buf) : -1;
            lingxin_mutex_unlock(send_thread_mutex);
            if (ret < 0) break;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
            uint32_t generation;
            memcpy(&generation, buf + send_uni_size, sizeof(generation));
            if (!lingxin_clean_capture_current(generation)) continue;
#endif
            // lingxin_log_debug("recorder_thread_send_data: length is %d", send_uni_size);
            if (temp_data_callback) {
                temp_data_callback(buf, send_uni_size, ++count);
            }
        }
        if (send_thread_stop_flag == 1 || send_thread_stop_flag == 2) {
            lingxin_log_ut_with_args(LINGXIN_DEBUG, "recorder_thread_receive_stop_signal", "flag is %d", send_thread_stop_flag);
            break;
        }
	}
send_record_thread_exit:
    if (buf) {
        lingxin_free(buf);
        buf = NULL;
    }
    /* Keep running=true until every object owned by this sender has been
     * released. start_send_thread() and the producer use the same mutex, so
     * an old sender can never delete or post a new round's semaphore. */
    lingxin_mutex_lock(send_thread_mutex);
    complete_waiting_stop = (send_thread_stop_flag == 2);
    epoch = send_thread_stop_epoch;
    ticket = send_thread_stop_ticket;
    cleanup_send_thread_sync_objects();
    send_thread_stop_flag = 0;
    send_thread_running = 0;
    lingxin_mutex_unlock(send_thread_mutex);

    // 如果此时是等待发完当前内容，则需要通知状态机录音模块正常结束已完成
    if (complete_waiting_stop) {
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_thread_wait_send_complete");
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_thread_wait_send_complete_callback");
        run_stop_callback(true, epoch, ticket);
    }
    return NULL;
}

/****************** 接收录音数据写入缓存（写缓存） ******************/
bool lingxin_try_process_record_data(void *data, int len, uint32_t generation)
{
    if (!data || !send_thread_mutex || !send_cbuf || len != send_uni_size ||
        len <= 0 || len > LINGXIN_RECORD_MAX_BYTES) return false;
    uint8_t frame[LINGXIN_RECORD_MAX_BYTES + sizeof(uint32_t)];
    bool accepted = false;
    lingxin_mutex_lock(send_thread_mutex);
    if (send_thread_running && send_thread_stop_flag == 0 && send_r_sem && send_flag &&
        !lingxin_cbuffer_full(send_cbuf)
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
        && lingxin_clean_capture_current(generation)
#endif
    ) {
        memcpy(frame, data, len);
        memcpy(frame + len, &generation, sizeof(generation));
        lingxin_cbuffer_put(send_cbuf, frame);
        lingxin_semaphore_set_value(send_r_sem, 0);
        lingxin_semaphore_post(send_r_sem);
        accepted = true;
    }
    lingxin_mutex_unlock(send_thread_mutex);
    return accepted;
}

void lingxin_process_record_data(void *data, int len) {
    if (data == NULL || len != send_uni_size || len <= 0 || len > LINGXIN_RECORD_MAX_BYTES) {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_data_process_fail", "data is NULL.");
        return;
    }
    if (send_cbuf == NULL) {
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_data_process_fail", "send_cbuf is NULL.");
        return;
    }

    lingxin_mutex_lock(send_thread_mutex);
    if (!send_thread_running) {
        lingxin_mutex_unlock(send_thread_mutex);
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_data_process_fail", "send_thread is not running.");
        return;
    }
    if (send_r_sem == NULL) {
        lingxin_mutex_unlock(send_thread_mutex);
        lingxin_log_ut_with_args(LINGXIN_ERROR, "recorder_data_process_fail", "send_r_sem is NULL.");
        return;
    }
    uint8_t frame[LINGXIN_RECORD_MAX_BYTES + sizeof(uint32_t)] = {0};
    memcpy(frame, data, len);
    lingxin_cbuffer_put(send_cbuf, frame);
    // lingxin_log_debug("recorder_data_process_success: len=%d, send_flag=%d", len, send_flag);
    if (send_flag) {
        lingxin_semaphore_set_value(send_r_sem, 0);
        lingxin_semaphore_post(send_r_sem);
    }
    lingxin_mutex_unlock(send_thread_mutex);
}

/****************** 相关参数获取 ******************/
// 获取帧大小
static int get_frame_size() {
    if (send_uni_size <= 0) {
        return lingxin_recorder_get_size_per_ms() * 20; // 默认20ms写一次
    } else {
        return send_uni_size;
    }
}

static void set_send_flag() {
    lingxin_mutex_lock(send_thread_mutex);
    if (recorder_ready && server_ready) {
        send_flag = 1;
        if (send_thread_stop_flag == 2 && send_r_sem != NULL) {
            // 录音已经停止，需要通知发送线程发送剩余内容
            lingxin_semaphore_set_value(send_r_sem, 0);
            lingxin_semaphore_post(send_r_sem);
        }
    } else {
        send_flag = 0;
    }
    lingxin_mutex_unlock(send_thread_mutex);
}

static void cleanup_send_thread_sync_objects(void)
{
    if (send_r_sem) {
        lingxin_log_ut(LINGXIN_DEBUG, "recorder_adapter_semaphore_destroy");
        lingxin_semaphore_destroy(send_r_sem);
        send_r_sem = NULL;
    }
}
