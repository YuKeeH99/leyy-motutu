#include "lingxin_clean_recorder.h"
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
#include "FreeRTOS.h"
#include "task.h"
#include "sysutils.h"
#include <string.h>
#include "lisa_mem.h"
#include "lisa_log.h"
#include "lisa_thread.h"
#include "lingxin_adapter.h"
#include "lingxin_capture_buffer.h"
#include "lingxin_recorder_manager.h"
#include "lingxin_protocol_manager.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
#endif

#define CLEAN_FRAME_MAX 1280U
#define CLEAN_CLOUD_VAD_WAIT_MS 20000U
/* Persistent storage decouples prompt completion from recorder/cloud setup.
 * AP output never dereferences a recorder that another task may free. */
static lingxin_capture_buffer_t s_capture __psram_bss__;
static uint32_t s_owner;
static bool s_capture_claimed, s_capture_frozen, s_limit_reached;
static lingxin_capture_stats_t s_capture_stats[LINGXIN_CAPTURE_STATS_SLOTS];
typedef struct {
    uint32_t owner, generation, frame_size;
    volatile bool running, exited, draining, drained;
    bool opened, limit_stop_taken;
    uint32_t tail_bytes;
    TickType_t limit_started_at;
    bool limit_timeout_taken;
} clean_recorder_t;

static uint32_t clean_owner(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    return service_offline_action_generation();
#else
    return 1U;
#endif
}

void lingxin_clean_capture_reset(void)
{
    taskENTER_CRITICAL();
    s_owner = clean_owner();
    s_capture_claimed = s_capture_frozen = false;
    s_limit_reached = false;
    lingxin_capture_begin(&s_capture);
    lingxin_capture_end(&s_capture, s_capture.generation);
    taskEXIT_CRITICAL();
}

void lingxin_clean_capture_arm(void)
{
    taskENTER_CRITICAL();
    if (!s_capture.overflow && s_owner == clean_owner()) s_capture.active = true;
    taskEXIT_CRITICAL();
}

bool lingxin_clean_capture_current(uint32_t generation)
{
    taskENTER_CRITICAL();
    bool current = generation != 0U && generation == s_capture.generation &&
                   !s_capture.overflow && s_owner == clean_owner();
    bool frozen_tail = s_limit_reached && s_capture_frozen;
    taskEXIT_CRITICAL();
    return current && (frozen_tail || lingxin_audio_uplink_gate_is_open(NULL));
}

void lingxin_audio_capture_clean(const int16_t *samples, uint32_t count)
{
    if (!samples || !count || count > CLEAN_FRAME_MAX / sizeof(*samples)) return;
    taskENTER_CRITICAL();
    uint32_t generation = s_capture.generation;
    bool capture_ready = s_capture.active && !s_capture_frozen && s_owner == clean_owner();
    taskEXIT_CRITICAL();
    uint32_t peak = 0U;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        uint32_t magnitude = (uint32_t)(sample < 0 ? -sample : sample);
        if (magnitude > peak) peak = magnitude;
    }
    bool gate_open = lingxin_audio_uplink_gate_is_open(NULL);
    /* VAD processing must stay outside the RTOS critical section. The AP
     * output task exclusively owns its state; discard if reset raced us. */
    lingxin_speech_limit_result_t limit = {0};
    if (gate_open && capture_ready)
        limit = lingxin_speech_limit_process(generation, samples, count);
    taskENTER_CRITICAL();
    if (generation != s_capture.generation) {
        taskEXIT_CRITICAL();
        return;
    }
    lingxin_capture_stats_t *stats = &s_capture_stats[generation % LINGXIN_CAPTURE_STATS_SLOTS];
    if (stats->generation != generation) {
        memset(stats, 0, sizeof(*stats));
        stats->generation = generation;
    }
    stats->frames++;
    bool was_overflow = s_capture.overflow;
    bool accepted = false;
    count = limit.accepted;
    if (count && gate_open && !s_capture_frozen && s_owner == clean_owner())
        accepted = lingxin_capture_put(&s_capture, samples, count * sizeof(*samples));
    if (!gate_open) {
        stats->gate_drops++;
        if (peak > stats->gated_peak) stats->gated_peak = peak;
    } else if (accepted) {
        stats->accepted_samples += count;
        stats->speech_samples = limit.speech_samples;
        stats->vad_checked = limit.checked_frames;
        stats->vad_positive = limit.positive_frames;
        stats->vad_window_peak = limit.window_peak;
        if (limit.reached) s_capture_frozen = s_limit_reached = true;
        if (peak > stats->accepted_peak) stats->accepted_peak = peak;
        /* An amplitude counter, not a local VAD or upload decision. */
        if (peak >= 512U) stats->accepted_signal_frames++;
        if (s_capture.length > stats->max_queued) stats->max_queued = s_capture.length;
    } else {
        stats->capture_drops++;
    }
    bool overflow = !was_overflow && s_capture.overflow;
    taskEXIT_CRITICAL();
    if (overflow) LOGE("[Lingxin] clean capture backlog exceeded; discard this utterance");
}

bool lingxin_clean_recorder_take_limit_stop(void *recorder)
{
    clean_recorder_t *ctx = recorder;
    if (!ctx) return false;
    taskENTER_CRITICAL();
    bool reached = ctx->opened && !ctx->limit_stop_taken &&
        ctx->owner == clean_owner() && ctx->owner == s_owner &&
        ctx->generation == s_capture.generation && !s_capture.overflow &&
        s_limit_reached;
    if (reached) {
        ctx->limit_stop_taken = true;
        ctx->limit_started_at = xTaskGetTickCount();
    }
    taskEXIT_CRITICAL();
    return reached;
}

bool lingxin_clean_recorder_limit_timed_out(void *recorder)
{
    clean_recorder_t *ctx = recorder;
    if (!ctx) return false;
    taskENTER_CRITICAL();
    bool ready = ctx->opened && ctx->limit_stop_taken && !ctx->limit_timeout_taken &&
        ctx->owner == clean_owner() && ctx->generation == s_capture.generation &&
        (TickType_t)(xTaskGetTickCount() - ctx->limit_started_at) >= pdMS_TO_TICKS(CLEAN_CLOUD_VAD_WAIT_MS);
    if (ready) ctx->limit_timeout_taken = true;
    taskEXIT_CRITICAL();
    return ready;
}

void lingxin_clean_capture_get_stats(lingxin_capture_stats_t *stats)
{
    if (!stats) return;
    taskENTER_CRITICAL();
    memcpy(stats, s_capture_stats, sizeof(s_capture_stats));
    taskEXIT_CRITICAL();
}

static void clean_sender(void *arg)
{
    clean_recorder_t *ctx = arg;
    uint8_t data[CLEAN_FRAME_MAX];
    uint32_t generation = 0, sent = 0;
    bool pending = false, silence = false;
    while (__atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE)) {
        bool draining = false;
        taskENTER_CRITICAL();
        bool cancelled = s_owner != ctx->owner || s_capture.generation != ctx->generation;
        bool overflow = s_owner == ctx->owner && s_capture.overflow;
        taskEXIT_CRITICAL();
        if (cancelled) {
            /* Never keep an invalidated sender alive until the next cloud
             * task. Overflow means this utterance is incomplete, not silent. */
            if (overflow) voice_chat_fail_current_round();
            break;
        }
        if (pending && !lingxin_clean_capture_current(generation)) pending = false;
        if (!module_record_is_server_ready() || !lingxin_clean_capture_current(ctx->generation)) {
            lisa_thread_mdelay(5);
            continue;
        }
        if (!pending) {
            taskENTER_CRITICAL();
            draining = ctx->draining;
            if (s_owner == ctx->owner && s_capture.generation == ctx->generation) {
                uint32_t length = ctx->frame_size;
                if ((draining || ctx->limit_stop_taken) && s_capture.length < length)
                    length = s_capture.length;
                silence = false;
                pending = lingxin_capture_take(&s_capture, data, length);
                if (pending && length < ctx->frame_size)
                    memset(data + length, 0, ctx->frame_size - length);
                generation = s_capture.generation;
                /* The twelve-second limit freezes only real input. Cloud VAD
                 * still needs a continuous stream to finish its endpoint;
                 * end_audio is sent only after its vad_end/vad_exit event. */
                if (!pending && !draining && ctx->limit_stop_taken) {
                    memset(data, 0, ctx->frame_size);
                    pending = silence = true;
                }
            }
            taskEXIT_CRITICAL();
        }
        if (draining && !pending) {
            __atomic_store_n(&ctx->drained, true, __ATOMIC_RELEASE);
            break;
        }
        /* Backpressure preserves the first frame instead of overwriting the
         * engine's small queue while task_started or the network is delayed. */
        if (pending && lingxin_try_process_record_data(data, ctx->frame_size, generation)) {
            pending = false;
            if (silence) {
                taskENTER_CRITICAL();
                ctx->tail_bytes += ctx->frame_size;
                taskEXIT_CRITICAL();
                lisa_thread_mdelay(ctx->frame_size / 32U);
            }
            if (++sent == 1U) LOGI("[Lingxin] first clean PCM sent, generation=%u", generation);
        } else {
            lisa_thread_mdelay(5);
        }
    }
    LOGI("[Lingxin] clean sender stopped, frames=%u", sent);
    __atomic_store_n(&ctx->exited, true, __ATOMIC_RELEASE);
}

lingxin_recorder_t lingxin_recorder_create(void)
{
    clean_recorder_t *ctx = lisa_mem_calloc(1, sizeof(*ctx));
    if (ctx) ctx->exited = true;
    return ctx;
}

void lingxin_recorder_open(lingxin_recorder_t recorder, lingxin_recorder_open_param_t *param,
                           lingxin_recorder_callback_t callback)
{
    clean_recorder_t *ctx = recorder;
    int result = -1;
    if (ctx && !ctx->opened && __atomic_load_n(&ctx->exited, __ATOMIC_ACQUIRE)) {
        ctx->frame_size = param && param->frame_size > 0 ? (uint32_t)param->frame_size : 640U;
        if (ctx->frame_size <= CLEAN_FRAME_MAX && !(ctx->frame_size & 1U)) {
            taskENTER_CRITICAL();
            ctx->owner = clean_owner();
            if (s_owner != ctx->owner || s_capture_claimed) {
                s_owner = ctx->owner;
                lingxin_capture_begin(&s_capture);
                s_capture_frozen = s_limit_reached = false;
            }
            ctx->generation = s_capture.generation;
            s_capture_claimed = true;
            bool overflow = s_capture.overflow;
            taskEXIT_CRITICAL();
            if (overflow) {
                if (callback) callback(-1);
                return;
            }
            ctx->draining = ctx->drained = false;
            __atomic_store_n(&ctx->running, true, __ATOMIC_RELEASE);
            __atomic_store_n(&ctx->exited, false, __ATOMIC_RELEASE);
            lisa_thread_attr_t attr = {.name = (uint8_t *)"lx.clean.send", .stack_size = 4096,
                                      .priority = LISA_OS_PRIORITY_LOW};
            if (lisa_thread_create(&attr, clean_sender, ctx)) {
                ctx->opened = true;
                result = 0;
                LOGI("[Lingxin] AEC mono recorder open, owner=%u frame=%u", ctx->owner, ctx->frame_size);
            } else {
                __atomic_store_n(&ctx->running, false, __ATOMIC_RELEASE);
                __atomic_store_n(&ctx->exited, true, __ATOMIC_RELEASE);
                taskENTER_CRITICAL();
                lingxin_capture_end(&s_capture, ctx->generation);
                taskEXIT_CRITICAL();
            }
        }
    }
    if (callback) callback(result);
}

void lingxin_clean_recorder_prepare_stop(void *recorder, bool drain)
{
    clean_recorder_t *ctx = recorder;
    if (!ctx) return;
    taskENTER_CRITICAL();
    if (ctx->opened && s_owner == ctx->owner && s_capture.generation == ctx->generation) {
        s_capture_frozen = true;
        ctx->draining = drain;
    }
    taskEXIT_CRITICAL();
}

static bool clean_stop(clean_recorder_t *ctx)
{
    if (!ctx) return false;
    if (ctx->opened && ctx->draining) {
        for (uint32_t ms = 0; ms < 2500U && !__atomic_load_n(&ctx->exited, __ATOMIC_ACQUIRE); ms += 5U)
            lisa_thread_mdelay(5);
        if (!__atomic_load_n(&ctx->exited, __ATOMIC_ACQUIRE)) {
            LOGE("[Lingxin] graceful clean drain timed out; retain recorder for retry");
            return false;
        }
    }
    taskENTER_CRITICAL();
    if (ctx->opened && s_owner == ctx->owner)
        lingxin_capture_end(&s_capture, ctx->generation);
    ctx->opened = false;
    taskEXIT_CRITICAL();
    __atomic_store_n(&ctx->running, false, __ATOMIC_RELEASE);
    for (uint32_t ms = 0; ms < 2500U && !__atomic_load_n(&ctx->exited, __ATOMIC_ACQUIRE); ms += 5U)
        lisa_thread_mdelay(5);
    return __atomic_load_n(&ctx->exited, __ATOMIC_ACQUIRE);
}

void lingxin_recorder_close(lingxin_recorder_t recorder, lingxin_recorder_callback_t callback)
{
    int result = clean_stop(recorder) ? 0 : -1;
    if (callback) callback(result);
}

void lingxin_recorder_destroy(lingxin_recorder_t recorder)
{
    if (clean_stop(recorder)) lisa_mem_free(recorder);
    else LOGE("[Lingxin] clean sender stop timed out; retain context");
}

int lingxin_recorder_get_size_per_ms(void) { return 32; }
#endif
