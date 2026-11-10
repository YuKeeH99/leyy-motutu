#include "audio_buffer_play.h"
#include "app_player.h"
#include "chat_api.h"
#include "lingxin_local_player.h"
#include "lingxin_recorder.h"
#include "lingxin_recorder_manager.h"
#include "lingxin_chat_upload_manager.h"
#include "lingxin_server.h"
#include "lingxin_system.h"
#include "lingxin_tts_suppress_policy.h"
#include "voice_player.h"
#include "lingxin_adapter.h"
#include "lingxin_clean_recorder.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lingxin_protocol_manager.h"
#include "voice_player_comm.h"
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
#include "service_codex_voice_input.h"
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "lingxin.audio"

#include "lisa_audio.h"
#include "lisa_device.h"
#include "lisa_log.h"
#include "lisa_mem.h"
#include "lisa_queue.h"
#include "lisa_thread.h"
#include "lisa_typedef.h"
#include "mp3dec.h"
#include "speex_resampler.h"
#include "voice_msg.h"

#define LINGXIN_RECORDER_RATE_HZ          16000
/* Cloud TTS is decoded to this rate before it enters the PCM stream.
 * The player clock stays at 16 kHz. Speex is the same resampler local prompt
 * files use, so pitch and duration stay with the source. */
#define LINGXIN_PLAYBACK_RATE_HZ          LINGXIN_RECORDER_RATE_HZ
#define LINGXIN_RESAMPLE_QUALITY          4
#define LINGXIN_RECORDER_BITS             16
#define LINGXIN_RECORDER_CHANNEL_BYTES    2
#define LINGXIN_RECORDER_FRAME_MS         80
#define LINGXIN_RECORDER_FRAME_BYTES      1280
#define LINGXIN_RECORDER_QUEUE_DEPTH      16
#define LINGXIN_RECORDER_BYPASS_AUDIO0    0
#define LINGXIN_STREAM_URL                "stream://type=pcm&rate=16000&channel=1&bits=16"
#define LINGXIN_STREAM_WRITE_TIMEOUT_MS   100
#define LINGXIN_STREAM_STALL_TIMEOUT_MS   3000U
#define LINGXIN_STREAM_QUEUE_MAX_BYTES    (1536U * 1024U)
#define LINGXIN_STREAM_THROTTLE_HIGH_BYTES (1024U * 1024U)
#define LINGXIN_STREAM_THROTTLE_MAX_MS     500U
#define LINGXIN_STREAM_THROTTLE_POLL_MS    10U
#define LINGXIN_STREAM_CHUNK_MAX_BYTES    (8U * 1024U)
#define LINGXIN_STREAM_QUEUE_DEPTH        2048
#define LINGXIN_MP3_DECODE_INITIAL_BYTES  (8 * 1024)
#define LINGXIN_MP3_DECODE_MAX_BYTES      (64 * 1024)
#define LINGXIN_MP3_DECODE_MAX_SAMPLES    (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)
#define LINGXIN_PCM_PREFILL_BYTES         (8 * 1024)
#define LINGXIN_PCM_PREFILL_CAP_BYTES     (16 * 1024)
#define LINGXIN_CODEX_PCM_PREFILL_BYTES   (24 * 1024)
#define LINGXIN_CODEX_PCM_PREFILL_CAP     (32 * 1024)
/* A no-progress limit, never a maximum answer duration. */
#define LINGXIN_PLAYBACK_STALL_MS         10000U
#define LINGXIN_AUTO_END_SPEECH_AVG_ABS   250U
#define LINGXIN_AUTO_END_SILENCE_AVG_ABS  180U
#define LINGXIN_AUTO_END_SILENCE_MS       800U
#define LINGXIN_AUTO_END_MIN_SPEECH_MS    160U
#define LINGXIN_AUTO_END_MIN_RECORD_MS    800U
#define LINGXIN_AUTO_END_MAX_RECORD_MS    30000U
#define LINGXIN_VAD_PREROLL_MS             320U
#define LINGXIN_VAD_PREROLL_BYTES          (LINGXIN_RECORDER_RATE_HZ * LINGXIN_RECORDER_CHANNEL_BYTES * \
                                            LINGXIN_VAD_PREROLL_MS / 1000U)

typedef struct {
    lisa_device_t *audio_dev;
    bool opened;
    volatile bool sender_running;
    volatile bool sender_exited;
    bool auto_end_speech_seen;
    bool auto_end_requested;
    bool vad_upload_enabled;
    lisa_queue_t *queue;
    lisa_thread_t *sender_thread;
    int frame_size;
    uint16_t pending_len;
    uint32_t auto_end_total_ms;
    uint32_t auto_end_speech_ms;
    uint32_t auto_end_silence_ms;
    uint32_t vad_preroll_write;
    uint32_t vad_preroll_len;
    uint8_t pending[LINGXIN_RECORDER_FRAME_BYTES];
    uint8_t vad_preroll[LINGXIN_VAD_PREROLL_BYTES];
} lingxin_recorder_ctx_t;

typedef struct {
    uint16_t len;
    uint8_t data[LINGXIN_RECORDER_FRAME_BYTES];
} lingxin_record_frame_t;

typedef struct {
    int volume;
    lingxin_local_player_callback_t callback;
} lingxin_local_player_ctx_t;

typedef struct {
    uint32_t generation;
    uint32_t epoch;
} lingxin_playback_watchdog_ctx_t;

typedef struct {
    PlaybackEventHandler callback;
    LingxinDownloadAudioEvent event;
    void *user_data;
    const char *reason;
    uint32_t generation;
} lingxin_playback_event_ctx_t;

typedef struct {
    uint8_t *data;
    uint32_t len;
    bool end;
} lingxin_stream_write_item_t;

typedef uint8_t player_t;

#define PLAYER_T_CLOUD              1U
#define PLAYER_T_TONE               2U
#define APP_PLAYER_PREPARING        0xEFU
#define PLAYER_EVT_ERROR            0U
#define PLAYER_EVT_PREPARED         1U
#define PLAYER_EVT_PLAYING          2U
#define PLAYER_EVT_PAUSED           3U
#define PLAYER_EVT_STOPED           4U
#define PLAYER_EVT_NEARLY_COMPLETE  5U
#define PLAYER_EVT_PLAYBACK_COMPLETE 6U
#define PLAYER_EVT_SEEKING          7U
#define PLAYER_EVT_SEEK_COMPLETE    8U
#define PLAYER_EVT_INIT             100U
#define LINGXIN_PLAYBACK_PLAYER_TYPE PLAYER_T_CLOUD

typedef int (*player_status_cb)(uint16_t st);

typedef struct {
    app_player_t *player;
    player_status_cb status_cb;
    bool callback_registered;
    bool stream_mode_expected;
    bool stream_mode_started;
} lingxin_player_compat_t;

static PlaybackEventHandler s_playback_callback;
static void *s_playback_user_data;
static bool s_stream_playing;
static uint32_t s_playback_generation;
static SemaphoreHandle_t s_playback_dispatch_mutex;
static SemaphoreHandle_t s_playback_lifecycle_mutex;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static uint32_t s_stream_owner_generation;
#endif
static lingxin_local_player_ctx_t *s_current_local_player;
static uint32_t s_record_frame_count;
static uint32_t s_record_bytes_total;
static uint32_t s_record_drop_count;
static uint32_t s_record_send_count;
static volatile bool s_record_uplink_gate_open = true;
static volatile uint32_t s_record_uplink_gate_generation;
static uint32_t s_record_uplink_gate_drop_count;
static lingxin_recorder_ctx_t *s_active_recorder;
static uint32_t s_stream_chunk_count;
static uint32_t s_stream_bytes_total;
static uint32_t s_stream_decode_frame_count;
static uint32_t s_stream_pcm_bytes_total;
static uint8_t *s_stream_pcm_prefill;
static uint32_t s_stream_pcm_prefill_len;
static uint32_t s_stream_writer_generation;
static uint32_t s_stream_epoch;
static uint32_t s_stream_queue_bytes;
static uint32_t s_stream_queue_peak_bytes;
static uint32_t s_stream_enqueue_active;
static uint32_t s_stream_pcm_written_bytes;
static int s_rs_src_rate;
static SpeexResamplerState *s_rs;
static int16_t s_rs_mono[LINGXIN_MP3_DECODE_MAX_SAMPLES];
static int16_t s_rs_out[LINGXIN_MP3_DECODE_MAX_SAMPLES * 2 + 128];
static uint32_t s_stream_last_progress_ms;
static bool s_stream_finish_submitted;
static bool s_stream_response_ready;
/* Local action suppression changes playback generation, but not the cloud
 * response identity. Its EOF must still close that same response. */
static uint32_t s_stream_response_id;
static uint32_t s_stream_failure_count;
static volatile bool s_player_cleanup_running;
static bool s_player_cleanup_started;
static bool s_player_cleanup_ok;
/* A completed reset attempt is not necessarily a successful output stop. */
static bool s_playback_input_blocked;
static uint32_t s_pending_termination_generation;
static lisa_thread_t *s_termination_worker;
static bool s_auto_end_thread_running;
static lisa_queue_t *s_stream_write_queue;
static lisa_thread_t *s_stream_writer_thread;
static volatile bool s_stream_writer_running;
static volatile bool s_stream_writer_exited = true;
static volatile bool s_stream_write_failed;
static bool s_stream_mode_started;
static bool s_stream_player_started;
static bool s_next_codex_prefill;
static bool s_stream_codex_prefill;

void lingxin_audio_set_codex_prefill(bool enabled)
{
    __atomic_store_n(&s_next_codex_prefill, enabled, __ATOMIC_RELEASE);
}
static bool s_stream_suppress_stop_event;
static bool s_stream_audio_ended;
static lingxin_tts_suppress_policy_t s_tts_suppress_policy;
static lingxin_player_compat_t s_cloud_player_compat;
static lingxin_player_compat_t s_tone_player_compat;

bool lingxin_audio_playback_input_ready(void)
{
    taskENTER_CRITICAL();
    bool ready = !s_playback_input_blocked && !s_stream_playing &&
                 s_stream_writer_exited && s_stream_enqueue_active == 0U &&
                 s_pending_termination_generation == 0U &&
                 !s_player_cleanup_started && !s_player_cleanup_running;
    taskEXIT_CRITICAL();
    return ready;
}

static void lingxin_playback_release_input(uint32_t generation)
{
    taskENTER_CRITICAL();
    if (generation == s_playback_generation) s_playback_input_blocked = false;
    taskEXIT_CRITICAL();
}

void lingxin_audio_get_playback_stats(lingxin_playback_stats_t *stats)
{
    if (!stats) return;
    taskENTER_CRITICAL();
    *stats = (lingxin_playback_stats_t){
        .generation = s_playback_generation, .chunks = s_stream_chunk_count,
        .bytes = s_stream_bytes_total, .pcm_bytes = s_stream_pcm_bytes_total,
        .pcm_written = s_stream_pcm_written_bytes, .queued_bytes = s_stream_queue_bytes,
        .queue_peak_bytes = s_stream_queue_peak_bytes, .failures = s_stream_failure_count,
        .writer_exited = s_stream_writer_exited,
        .cleanup_pending = s_player_cleanup_started || s_player_cleanup_running ||
                           s_pending_termination_generation != 0U,
        .input_ready = lingxin_audio_playback_input_ready(),
        .finish_submitted = s_stream_finish_submitted,
        .playing = s_stream_playing, .started = s_stream_player_started,
        .suppressed = s_tts_suppress_policy.active, .failed = s_stream_write_failed,
        .ended = s_stream_audio_ended,
    };
    taskEXIT_CRITICAL();
}

static int lingxin_audio_player_status_cb(uint16_t st);
static int lingxin_audio_player_status_for_generation(uint16_t st, uint32_t generation);
static void lingxin_stream_fail(uint32_t generation, const char *reason);
static bool lingxin_ensure_termination_worker(void);

static uint32_t lingxin_playback_now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static bool lingxin_stream_writer_is_current(void)
{
    return s_stream_writer_running && !s_stream_write_failed &&
           s_stream_writer_generation == __atomic_load_n(&s_playback_generation, __ATOMIC_ACQUIRE);
}


/* Only PlayEnd dispatch is serialized with generation changes. Never hold
 * this lock around player reset, writer joins or TerminateEnd: those paths
 * can wait for the websocket thread which initializes the next player. */
static bool lingxin_playback_dispatch_lock(void)
{
    if (s_playback_dispatch_mutex == NULL) {
        SemaphoreHandle_t created = xSemaphoreCreateRecursiveMutex();
        if (created == NULL) return false;
        taskENTER_CRITICAL();
        if (s_playback_dispatch_mutex == NULL) {
            s_playback_dispatch_mutex = created;
            created = NULL;
        }
        taskEXIT_CRITICAL();
        if (created != NULL) vSemaphoreDelete(created);
    }
    return xSemaphoreTakeRecursive(s_playback_dispatch_mutex, portMAX_DELAY) == pdTRUE;
}

static void lingxin_playback_dispatch_unlock(void)
{
    (void)xSemaphoreGiveRecursive(s_playback_dispatch_mutex);
}

static uint32_t lingxin_playback_advance_generation(void)
{
    if (!lingxin_playback_dispatch_lock()) {
        voice_chat_fail_current_round();
        return 0U;
    }
    taskENTER_CRITICAL();
    s_playback_input_blocked = true;
    uint32_t generation = __atomic_add_fetch(&s_playback_generation, 1U, __ATOMIC_ACQ_REL);
    if (generation == 0U)
        generation = __atomic_add_fetch(&s_playback_generation, 1U, __ATOMIC_ACQ_REL);
    taskEXIT_CRITICAL();
    lingxin_playback_dispatch_unlock();
    return generation;
}

static void lingxin_record_discard_buffered(lingxin_recorder_ctx_t *recorder)
{
    if (recorder == NULL) {
        return;
    }

    recorder->pending_len = 0U;
    recorder->vad_preroll_write = 0U;
    recorder->vad_preroll_len = 0U;
    recorder->auto_end_total_ms = 0U;
    recorder->auto_end_speech_ms = 0U;
    recorder->auto_end_silence_ms = 0U;
    recorder->auto_end_speech_seen = false;
    recorder->auto_end_requested = false;
    recorder->vad_upload_enabled = false;
    if (recorder->queue != NULL) {
        lisa_queue_clear(recorder->queue);
    }
}

uint32_t lingxin_audio_uplink_gate_close(const char *reason)
{
    taskENTER_CRITICAL();
    uint32_t generation = s_record_uplink_gate_generation + 1U;

    if (generation == 0U) {
        generation = 1U;
    }
    s_record_uplink_gate_generation = generation;
    s_record_uplink_gate_open = false;
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    lingxin_clean_capture_reset();
#endif
    s_record_uplink_gate_drop_count = 0U;
    taskEXIT_CRITICAL();
    lingxin_record_discard_buffered(s_active_recorder);
    lingxin_trace_set("audio:wakeup_gate_closed");
    lingxin_server_diag_timing_mark("wakeup_prompt_gate_closed", (int)generation);
    LOGI("[Lingxin] wakeup audio gate closed, generation=%u reason=%s",
         (unsigned)generation, reason ? reason : "");
    return generation;
}

uint32_t lingxin_audio_uplink_gate_hold(const char *reason)
{
    /* The accepted keyword already began this epoch, possibly before the
     * recorder opened. Starting its prompt must not invalidate that recorder. */
    taskENTER_CRITICAL();
    uint32_t generation = !s_record_uplink_gate_open ? s_record_uplink_gate_generation : 0U;
    taskEXIT_CRITICAL();
    return generation ? generation : lingxin_audio_uplink_gate_close(reason);
}

void lingxin_audio_uplink_gate_open(uint32_t generation, const char *reason)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(service_offline_action_generation())) return;
#endif
    taskENTER_CRITICAL();
    if (generation == 0U || generation != s_record_uplink_gate_generation) {
        taskEXIT_CRITICAL();
        LOGI("[Lingxin] ignore stale wakeup audio gate release, expected=%u current=%u reason=%s",
             (unsigned)generation,
             (unsigned)s_record_uplink_gate_generation,
             reason ? reason : "");
        return;
    }
    if (s_record_uplink_gate_open) {
        taskEXIT_CRITICAL();
        return; /* A late failsafe must not clear a live utterance. */
    }

    /* Legacy raw capture must be cleared before opening. Clean capture was
     * reset on close and is armed here without erasing a live utterance. */
#ifndef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    taskEXIT_CRITICAL();
    lingxin_record_discard_buffered(s_active_recorder);
    taskENTER_CRITICAL();
    if (generation != s_record_uplink_gate_generation) {
        taskEXIT_CRITICAL();
        return;
    }
#endif
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    lingxin_clean_capture_arm();
#endif
    s_record_uplink_gate_open = true;
    taskEXIT_CRITICAL();
    lingxin_trace_set("audio:wakeup_gate_open");
    lingxin_server_diag_timing_mark("wakeup_prompt_gate_open", (int)s_record_uplink_gate_drop_count);
    LOGI("[Lingxin] wakeup audio gate opened, generation=%u dropped_frames=%u reason=%s",
         (unsigned)generation,
         (unsigned)s_record_uplink_gate_drop_count,
         reason ? reason : "");
}

bool lingxin_audio_uplink_gate_is_open(uint32_t *generation)
{
    if (generation != NULL) {
        *generation = s_record_uplink_gate_generation;
    }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (voice_player_local_tone_blocks_uplink()) return false;
    if (!service_offline_action_online_current(service_offline_action_generation())) return false;
#endif
    return s_record_uplink_gate_open;
}

static lingxin_player_compat_t *lingxin_compat_get_ctx(player_t type)
{
    return (type == PLAYER_T_TONE) ? &s_tone_player_compat : &s_cloud_player_compat;
}

static app_player_t *lingxin_compat_resolve_player(player_t type)
{
    if (type == PLAYER_T_TONE) {
        return tone_player;
    }

    return alert_player;
}

static uint16_t lingxin_compat_map_event(app_player_event_t event)
{
    switch (event) {
    case APP_PLAYER_EVENT_PREPARED:
        return PLAYER_EVT_PREPARED;
    case APP_PLAYER_EVENT_PLAYING:
        return PLAYER_EVT_PLAYING;
    case APP_PLAYER_EVENT_PAUSED:
        return PLAYER_EVT_PAUSED;
    case APP_PLAYER_EVENT_STOPPED:
        return PLAYER_EVT_STOPED;
    case APP_PLAYER_EVENT_COMPLETED:
        return PLAYER_EVT_PLAYBACK_COMPLETE;
    case APP_PLAYER_EVENT_SEEK_COMPLETE:
        return PLAYER_EVT_SEEK_COMPLETE;
    case APP_PLAYER_EVENT_ERROR:
    default:
        return PLAYER_EVT_ERROR;
    }
}

static void lingxin_compat_player_event_cb(app_player_t *player, app_player_event_t event, void *user_data)
{
    lingxin_player_compat_t *ctx = (lingxin_player_compat_t *)user_data;

    (void)player;
    if (ctx == NULL) {
        return;
    }

    if (event == APP_PLAYER_EVENT_PLAYING) {
        ctx->stream_mode_started = ctx->stream_mode_expected;
    } else if (event == APP_PLAYER_EVENT_STOPPED ||
               event == APP_PLAYER_EVENT_COMPLETED ||
               event == APP_PLAYER_EVENT_ERROR) {
        ctx->stream_mode_expected = false;
        ctx->stream_mode_started = false;
    }

    if (ctx->status_cb != NULL) {
        ctx->status_cb(lingxin_compat_map_event(event));
    }
}


static void lingxin_cloud_player_event_cb(app_player_t *player, app_player_event_t event, void *user_data)
{
    uint32_t generation = (uint32_t)(uintptr_t)user_data;
    if (generation != __atomic_load_n(&s_playback_generation, __ATOMIC_ACQUIRE)) return;
    lingxin_audio_player_status_for_generation(lingxin_compat_map_event(event), generation);
}

static bool lingxin_compat_prepare_player(player_t type, player_status_cb cb, lingxin_player_compat_t **out_ctx,
                                          uint32_t generation)
{
    lingxin_player_compat_t *ctx = lingxin_compat_get_ctx(type);
    app_player_t *player = lingxin_compat_resolve_player(type);

    if (ctx == NULL || player == NULL) {
        LOGE("[Lingxin] compat player unavailable, type=%u player=%p", type, player);
        return false;
    }

    if (type == PLAYER_T_CLOUD && cb != NULL) {
        if (app_player_register_callback(player, lingxin_cloud_player_event_cb,
              (void *)(uintptr_t)generation) != APP_PLAYER_OK) return false;
        ctx->callback_registered = true;
        ctx->player = player;
    }
    if (!ctx->callback_registered || ctx->player != player) {
        if (app_player_register_callback(player, lingxin_compat_player_event_cb, ctx) != APP_PLAYER_OK) {
            LOGE("[Lingxin] compat register callback failed, type=%u player=%p", type, player);
            return false;
        }
        ctx->callback_registered = true;
        ctx->player = player;
    }

    if (cb != NULL) {
        ctx->status_cb = cb;
    }
    if (out_ctx != NULL) {
        *out_ctx = ctx;
    }

    return true;
}

static int lingxin_compat_player_play(player_t type, const char *url, player_status_cb cb, uint32_t generation)
{
    lingxin_player_compat_t *ctx = NULL;

    if (url == NULL || url[0] == '\0') {
        return -1;
    }

    if (!lingxin_compat_prepare_player(type, cb, &ctx, generation)) {
        return -1;
    }

    if (cb != NULL) {
        cb(APP_PLAYER_PREPARING);
    }

    if (type == PLAYER_T_CLOUD && strcmp(url, LINGXIN_STREAM_URL) == 0) {
        ctx->stream_mode_expected = true;
        ctx->stream_mode_started = false;
        return 0;
    }

    ctx->stream_mode_expected = false;
    ctx->stream_mode_started = false;
    if (type == PLAYER_T_CLOUD) voice_player_wait_upload_cue();
    return app_player_play(ctx->player, url) == APP_PLAYER_OK ? 0 : -1;
}

static int lingxin_compat_player_put_stream_data(player_t type, uint8_t *data, uint32_t size, uint32_t wait_ms)
{
    lingxin_player_compat_t *ctx = NULL;

    if (!lingxin_compat_prepare_player(type, NULL, &ctx, 0U)) {
        return -1;
    }

    if (!ctx->stream_mode_expected) {
        ctx->stream_mode_expected = true;
    }

    if (!ctx->stream_mode_started) {
        if (type == PLAYER_T_CLOUD) voice_player_wait_upload_cue();
        if (app_player_play_stream(ctx->player, LINGXIN_RECORDER_RATE_HZ, 1, LINGXIN_RECORDER_BITS) != APP_PLAYER_OK) {
            LOGE("[Lingxin] compat stream start failed, type=%u", type);
            return -1;
        }
#ifdef CONFIG_APP_PLAYER_AUDIO_FOCUS
        /* app_player reports OK when focus rejects a stream. Detect that
         * pseudo-success so the caller can fall back instead of dropping PCM. */
        if (app_player_focus_get_state(ctx->player) != APP_PLAYER_FOCUS_FOREGROUND) {
            LOGW("[Lingxin] compat stream start rejected by audio focus, type=%u", type);
            return -1;
        }
#endif
        ctx->stream_mode_started = true;
    }

    if (data == NULL && size == 0U) {
        return app_player_finish_stream(ctx->player) == APP_PLAYER_OK ? 0 : -1;
    }

    return app_player_write_stream(ctx->player, data, size, wait_ms);
}

static void lingxin_cloud_player_cleanup_thread(void *arg)
{
    app_player_t *player = arg;
    /* The vendor reset may wait indefinitely. Keep it out of main/WS and
     * forbid starting another response until this worker really returns. */
    s_player_cleanup_ok = app_player_reset(player) == APP_PLAYER_OK;
    if (s_player_cleanup_ok) {
        s_cloud_player_compat.stream_mode_expected = false;
        s_cloud_player_compat.stream_mode_started = false;
    }
    __atomic_store_n(&s_player_cleanup_running, false, __ATOMIC_RELEASE);
}

static bool lingxin_playback_lifecycle_lock(void)
{
    if (s_playback_lifecycle_mutex == NULL) {
        SemaphoreHandle_t created = xSemaphoreCreateRecursiveMutex();
        if (created == NULL) return false;
        taskENTER_CRITICAL();
        if (s_playback_lifecycle_mutex == NULL) {
            s_playback_lifecycle_mutex = created;
            created = NULL;
        }
        taskEXIT_CRITICAL();
        if (created != NULL) vSemaphoreDelete(created);
    }
    return xSemaphoreTakeRecursive(s_playback_lifecycle_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void lingxin_playback_lifecycle_unlock(void)
{
    (void)xSemaphoreGiveRecursive(s_playback_lifecycle_mutex);
}

static bool lingxin_compat_player_stop_sync(player_t type)
{
    app_player_t *player = lingxin_compat_resolve_player(type);
    if (player == NULL) return true;
    if (!s_player_cleanup_started) {
        s_player_cleanup_started = true;
        s_player_cleanup_running = true;
        s_player_cleanup_ok = false;
        lisa_thread_attr_t attr = {
            .name = (uint8_t *)"lx.play.stop", .stack_size = 4096,
            .priority = LISA_OS_PRIORITY_LOW,
        };
        if (lisa_thread_create(&attr, lingxin_cloud_player_cleanup_thread, player) == NULL) {
            s_player_cleanup_running = false;
            s_player_cleanup_started = false;
            return false;
        }
    }
    for (uint32_t waited = 0U; s_player_cleanup_running && waited < 1500U; waited += 10U)
        lisa_thread_mdelay(10);
    if (s_player_cleanup_running) return false;
    bool ok = s_player_cleanup_ok;
    s_player_cleanup_started = false;
    return ok;
}

static void lingxin_compat_player_volume(player_t type, uint8_t volume)
{
    lingxin_player_compat_t *ctx = NULL;
    uint8_t fixed = volume == 0U ? 1U : volume;

    if (!lingxin_compat_prepare_player(type, NULL, &ctx, 0U)) {
        return;
    }

    (void)app_player_set_volume(ctx->player, fixed);
}

static void lingxin_playback_event_thread(void *arg)
{
    lingxin_playback_event_ctx_t *ctx = (lingxin_playback_event_ctx_t *)arg;

    if (ctx == NULL) {
        return;
    }

    bool locked = ctx->event == Lingxin_Download_Audio_PlayEnd;
    if (locked && !lingxin_playback_dispatch_lock()) {
        lisa_mem_free(ctx);
        return;
    }
    if (ctx->generation != __atomic_load_n(&s_playback_generation, __ATOMIC_ACQUIRE)) {
        if (locked) lingxin_playback_dispatch_unlock();
        lisa_mem_free(ctx);
        return;
    }
    LOGI("[Lingxin] playback async event begin, event=%d, reason=%s",
         ctx->event,
         ctx->reason ? ctx->reason : "");
    if (ctx->callback) {
        ctx->callback(ctx->event, ctx->user_data);
    }
    if (ctx->event == Lingxin_Download_Audio_PlayEnd &&
        ctx->generation == __atomic_load_n(&s_playback_generation, __ATOMIC_ACQUIRE)) {
        /* The app_player completion is authoritative.  The engine currently
         * advances to the next input round without emitting lifecycle
         * PLAY_END, so session-control actions must observe this boundary
         * directly.  The server-side gate is generation-bound and idempotent. */
        lingxin_server_tts_playback_complete();
        voice_msg_pub_timeout(VOICE_MSG_PLAYER_TTS_STOPED, NULL, 0, 0);
    }
    LOGI("[Lingxin] playback async event end, event=%d, reason=%s",
         ctx->event,
         ctx->reason ? ctx->reason : "");
    if (locked) lingxin_playback_dispatch_unlock();

    lisa_mem_free(ctx);
}

static bool lingxin_post_playback_event_async(LingxinDownloadAudioEvent event, const char *reason,
                                              uint32_t generation)
{
    if (s_playback_callback == NULL) {
        LOGW("[Lingxin] playback event drop, no callback, event=%d", event);
        return true;
    }

    lingxin_playback_event_ctx_t *ctx =
        (lingxin_playback_event_ctx_t *)lisa_mem_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        LOGE("[Lingxin] playback event alloc failed, event=%d", event);
        if (event != Lingxin_Download_Audio_TerminateEnd) voice_chat_fail_epoch(s_stream_epoch);
        return false;
    }

    ctx->callback = s_playback_callback;
    ctx->event = event;
    ctx->user_data = s_playback_user_data;
    ctx->reason = reason;
    ctx->generation = generation;

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lx.play.evt",
        .stack_size = 6 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_playback_event_thread, ctx);
    if (thread == NULL) {
        LOGE("[Lingxin] playback event thread create failed, event=%d", event);
        lisa_mem_free(ctx);
        if (event != Lingxin_Download_Audio_TerminateEnd) voice_chat_fail_epoch(s_stream_epoch);
        return false;
    }
    return true;
}



static void lingxin_stream_write_item_free(lingxin_stream_write_item_t *item)
{
    if (item != NULL && item->data != NULL) {
        lisa_mem_free(item->data);
        taskENTER_CRITICAL();
        s_stream_queue_bytes -= item->len;
        taskEXIT_CRITICAL();
        item->data = NULL;
    }
}

static void lingxin_stream_write_queue_drain(void)
{
    lingxin_stream_write_item_t item;

    if (s_stream_write_queue == NULL) {
        return;
    }

    while (lisa_queue_pop(s_stream_write_queue, &item, sizeof(item), 0) == LISA_OK) {
        lingxin_stream_write_item_free(&item);
    }
}

static bool lingxin_mp3_decode_buffer_append(uint8_t **buffer,
                                             uint32_t *len,
                                             uint32_t *cap,
                                             const uint8_t *data,
                                             uint32_t data_len)
{
    if (buffer == NULL || len == NULL || cap == NULL || data == NULL || data_len == 0U) {
        return true;
    }

    if (*len > LINGXIN_MP3_DECODE_MAX_BYTES ||
        data_len > LINGXIN_MP3_DECODE_MAX_BYTES ||
        *len + data_len > LINGXIN_MP3_DECODE_MAX_BYTES) {
        LOGE("[Lingxin] mp3 decode buffer overflow: len=%u, append=%u", *len, data_len);
        return false;
    }

    uint32_t need = *len + data_len;
    if (need > *cap) {
        uint32_t new_cap = *cap ? *cap : LINGXIN_MP3_DECODE_INITIAL_BYTES;
        while (new_cap < need) {
            new_cap *= 2U;
        }
        if (new_cap > LINGXIN_MP3_DECODE_MAX_BYTES) {
            new_cap = LINGXIN_MP3_DECODE_MAX_BYTES;
        }

        uint8_t *new_buffer = (uint8_t *)lisa_mem_realloc(*buffer, new_cap);
        if (new_buffer == NULL) {
            LOGE("[Lingxin] mp3 decode buffer realloc failed: cap=%u, need=%u", new_cap, need);
            return false;
        }
        *buffer = new_buffer;
        *cap = new_cap;
    }

    memcpy(*buffer + *len, data, data_len);
    *len += data_len;
    return true;
}

static void lingxin_mp3_decode_buffer_consume(uint8_t *buffer, uint32_t *len, uint32_t consumed)
{
    if (buffer == NULL || len == NULL || consumed == 0U) {
        return;
    }

    if (consumed >= *len) {
        *len = 0;
        return;
    }

    memmove(buffer, buffer + consumed, *len - consumed);
    *len -= consumed;
}

static void lingxin_stream_pcm_prefill_reset(void)
{
    if (s_stream_pcm_prefill != NULL) {
        lisa_mem_free(s_stream_pcm_prefill);
        s_stream_pcm_prefill = NULL;
    }
    s_stream_pcm_prefill_len = 0U;
}

static bool lingxin_stream_submit_pcm(const uint8_t *data, uint32_t pcm_bytes)
{
    uint32_t offset = 0U;
    uint32_t progress_at = lingxin_playback_now_ms();
    while (offset < pcm_bytes) {
        if (!lingxin_stream_writer_is_current()) return false;
        int ret = lingxin_compat_player_put_stream_data(LINGXIN_PLAYBACK_PLAYER_TYPE,
                         (uint8_t *)data + offset, pcm_bytes - offset,
                         LINGXIN_STREAM_WRITE_TIMEOUT_MS);
        if (!lingxin_stream_writer_is_current()) return false;
        if (ret < 0 || (uint32_t)ret > pcm_bytes - offset) {
            LOGE("[Lingxin] PCM write failed ret=%d remaining=%u", ret, pcm_bytes - offset);
            return false;
        }
        if (ret == 0) {
            if ((uint32_t)(lingxin_playback_now_ms() - progress_at) >= LINGXIN_STREAM_STALL_TIMEOUT_MS)
                return false;
            lisa_thread_mdelay(10);
            continue;
        }
        offset += (uint32_t)ret;
        s_stream_pcm_written_bytes += (uint32_t)ret;
        progress_at = lingxin_playback_now_ms();
        s_stream_last_progress_ms = progress_at;
        if (!s_stream_player_started) {
            s_stream_player_started = true;
            lingxin_server_diag_timing_mark("stream_first_pcm_write", ret);
            voice_msg_pub_timeout(VOICE_MSG_PLAYER_TTS_PLAYING, NULL, 0, 0);
        }
    }
    return true;
}

static bool lingxin_stream_flush_pcm_prefill(void)
{
    if (s_stream_pcm_prefill_len == 0U) {
        return true;
    }

    uint32_t bytes = s_stream_pcm_prefill_len;
    if (!lingxin_stream_submit_pcm(s_stream_pcm_prefill, bytes)) {
        return false;
    }
    lingxin_stream_pcm_prefill_reset();
    return true;
}

static void lingxin_resample_reset(void)
{
    if (s_rs != NULL) {
        speex_resampler_destroy(s_rs);
        s_rs = NULL;
    }
    s_rs_src_rate = 0;
}

/* Speex resample onto the 16 kHz playback clock. Same converter the prompt
 * player uses, so a 22050 Hz cloud frame keeps its pitch and duration. */
static bool lingxin_resample_16k(const int16_t *in,
                                 uint32_t in_samples,
                                 int src_rate,
                                 int16_t *out,
                                 uint32_t out_cap,
                                 uint32_t *out_samples)
{
    uint32_t in_off = 0U;
    uint32_t produced = 0U;
    int err = RESAMPLER_ERR_SUCCESS;

    if (in == NULL || out == NULL || out_samples == NULL || src_rate < 8000 || src_rate > 48000) {
        return false;
    }
    if (s_rs == NULL) {
        s_rs = speex_resampler_init(1, (spx_uint32_t)src_rate, LINGXIN_PLAYBACK_RATE_HZ,
                                    LINGXIN_RESAMPLE_QUALITY, &err);
        if (s_rs == NULL || err != RESAMPLER_ERR_SUCCESS) {
            s_rs = NULL;
            return false;
        }
        /* This SDK's skip_zeros writes the filter delay with memset, which
         * corrupts last_sample and the first frame then produces no audio. */
        s_rs_src_rate = src_rate;
    } else if (s_rs_src_rate != src_rate) {
        return false;
    }
    if (in_samples == 0U) {
        *out_samples = 0U;
        return true;
    }

    while (in_off < in_samples) {
        spx_uint32_t in_len = (spx_uint32_t)(in_samples - in_off);
        spx_uint32_t out_len;

        if (produced >= out_cap) {
            return false;
        }
        out_len = (spx_uint32_t)(out_cap - produced);
        err = speex_resampler_process_int(s_rs, 0, in + in_off, &in_len, out + produced, &out_len);
        if (err != RESAMPLER_ERR_SUCCESS || (in_len == 0U && out_len == 0U)) {
            return false;
        }
        in_off += in_len;
        produced += out_len;
    }
    *out_samples = produced;
    return true;
}

static bool lingxin_stream_write_pcm(const short *pcm, const MP3FrameInfo *info)
{
    const int16_t *mono;
    uint32_t mono_samples;
    uint32_t out_samples;
    uint32_t i;

    if (pcm == NULL || info == NULL || info->outputSamps <= 0) {
        return true;
    }

    if ((info->nChans != 1 && info->nChans != 2) ||
        info->bitsPerSample != LINGXIN_RECORDER_BITS ||
        info->samprate < 8000 || info->samprate > 48000) {
        LOGE("[Lingxin] mp3 stream unsupported frame: rate=%d ch=%d bits=%d samples=%d",
             info->samprate,
             info->nChans,
             info->bitsPerSample,
             info->outputSamps);
        return false;
    }

    if (info->nChans == 1) {
        mono = (const int16_t *)pcm;
        mono_samples = (uint32_t)info->outputSamps;
    } else {
        mono_samples = (uint32_t)info->outputSamps / 2U;
        if (mono_samples == 0U || mono_samples > LINGXIN_MP3_DECODE_MAX_SAMPLES) {
            LOGE("[Lingxin] mp3 stereo frame size invalid: samples=%d", info->outputSamps);
            return false;
        }
        for (i = 0U; i < mono_samples; i++) {
            s_rs_mono[i] = (int16_t)(((int32_t)pcm[i * 2U] + (int32_t)pcm[i * 2U + 1U]) / 2);
        }
        mono = s_rs_mono;
    }

    if (s_stream_decode_frame_count == 0U) {
        LOGI("[Lingxin] mp3 source rate=%d ch=%d, playback rate=%d",
             info->samprate,
             info->nChans,
             LINGXIN_PLAYBACK_RATE_HZ);
    }
    if (!lingxin_resample_16k(mono,
                              mono_samples,
                              info->samprate,
                              s_rs_out,
                              (uint32_t)(sizeof(s_rs_out) / sizeof(s_rs_out[0])),
                              &out_samples)) {
        LOGE("[Lingxin] mp3 resample failed: rate=%d samples=%u", info->samprate, mono_samples);
        return false;
    }
    s_stream_decode_frame_count++;
    if (out_samples == 0U) {
        return true;
    }

    uint32_t pcm_bytes = out_samples * sizeof(short);
    s_stream_pcm_bytes_total += pcm_bytes;
    if (s_stream_decode_frame_count == 1U || (s_stream_decode_frame_count % 20U) == 0U) {
        LOGI("[Lingxin] mp3 decoded to pcm frames=%u, pcm_bytes=%u, last=%u",
             s_stream_decode_frame_count,
             s_stream_pcm_bytes_total,
             pcm_bytes);
    }

    if (!s_stream_player_started) {
        uint32_t cap = s_stream_codex_prefill ? LINGXIN_CODEX_PCM_PREFILL_CAP : LINGXIN_PCM_PREFILL_CAP_BYTES;
        uint32_t target = s_stream_codex_prefill ? LINGXIN_CODEX_PCM_PREFILL_BYTES : LINGXIN_PCM_PREFILL_BYTES;
        if (s_stream_pcm_prefill == NULL) {
            s_stream_pcm_prefill = (uint8_t *)lisa_mem_alloc(cap);
            if (s_stream_pcm_prefill == NULL) {
                LOGE("[Lingxin] pcm prefill allocate failed");
                return false;
            }
        }
        if (s_stream_pcm_prefill_len + pcm_bytes > cap) {
            LOGE("[Lingxin] pcm prefill overflow: buffered=%u, append=%u",
                 s_stream_pcm_prefill_len,
                 pcm_bytes);
            return false;
        }
        memcpy(s_stream_pcm_prefill + s_stream_pcm_prefill_len, s_rs_out, pcm_bytes);
        s_stream_pcm_prefill_len += pcm_bytes;
        if (s_stream_pcm_prefill_len < target) {
            return true;
        }
        return lingxin_stream_flush_pcm_prefill();
    }

    return lingxin_stream_submit_pcm((const uint8_t *)s_rs_out, pcm_bytes);
}

static bool lingxin_stream_decode_pending(HMP3Decoder decoder,
                                          uint8_t *pending,
                                          uint32_t *pending_len,
                                          short *pcm)
{
    if (decoder == NULL || pending == NULL || pending_len == NULL || pcm == NULL) {
        return false;
    }

    while (*pending_len > 0U) {
        if (!lingxin_stream_writer_is_current()) return false;
        int sync = MP3FindSyncWord(pending, (int)*pending_len);
        if (sync < 0) {
            if (*pending_len > 3U) {
                memmove(pending, pending + *pending_len - 3U, 3U);
                *pending_len = 3U;
            }
            return true;
        }

        if (sync > 0) {
            LOGI("[Lingxin] mp3 stream skip non-frame bytes=%d", sync);
            lingxin_mp3_decode_buffer_consume(pending, pending_len, (uint32_t)sync);
        }

        unsigned char *in = pending;
        int bytes_left = (int)*pending_len;
        int ret = MP3Decode(decoder, &in, &bytes_left, pcm, 0);
        uint32_t consumed = (uint32_t)(in - pending);

        if (ret == ERR_MP3_NONE) {
            MP3FrameInfo info;
            MP3GetLastFrameInfo(decoder, &info);
            if (consumed == 0U) {
                LOGE("[Lingxin] mp3 decode consumed zero bytes");
                return false;
            }
            lingxin_mp3_decode_buffer_consume(pending, pending_len, consumed);
            if (!lingxin_stream_write_pcm(pcm, &info)) {
                return false;
            }
            continue;
        }

        if (ret == ERR_MP3_INDATA_UNDERFLOW || ret == ERR_MP3_MAINDATA_UNDERFLOW) {
            return true;
        }

        LOGW("[Lingxin] mp3 decode frame failed: ret=%d, pending=%u, consumed=%u",
             ret,
             *pending_len,
             consumed);
        if (consumed > 0U) {
            lingxin_mp3_decode_buffer_consume(pending, pending_len, consumed);
        } else {
            lingxin_mp3_decode_buffer_consume(pending, pending_len, 1U);
        }
    }

    return true;
}

static bool lingxin_stream_writer_stop(void)
{
    s_stream_writer_running = false;

    /* Do not release the queue while the writer may still be blocked in its
     * first app_player_write_stream(). The previous fixed delay caused an
     * intermittent queue use-after-free during wake-word interruption. */
    for (uint32_t waited_ms = 0U; (!s_stream_writer_exited || s_stream_enqueue_active != 0U) && waited_ms < 2500U; waited_ms += 10U) {
        lisa_thread_mdelay(10);
    }
    if (!s_stream_writer_exited || s_stream_enqueue_active != 0U) {
        LOGE("[Lingxin] stream writer stop timeout; preserve queue to avoid use-after-free");
        return false;
    }
    s_stream_writer_thread = NULL;

    if (s_stream_write_queue != NULL) {
        lingxin_stream_write_queue_drain();
        lisa_queue_delete(s_stream_write_queue);
        s_stream_write_queue = NULL;
    }
    return true;
}

static void lingxin_stream_fail(uint32_t generation, const char *reason)
{
    taskENTER_CRITICAL();
    if (generation != s_playback_generation || s_stream_write_failed) {
        taskEXIT_CRITICAL();
        return;
    }
    s_stream_write_failed = true;
    s_playback_input_blocked = true;
    s_stream_writer_running = false;
    s_stream_failure_count++;
    uint32_t epoch = s_stream_epoch;
    taskEXIT_CRITICAL();
    LOGW("[Lingxin] playback failed gen=%u reason=%s queued=%u written=%u",
         generation, reason, s_stream_queue_bytes, s_stream_pcm_written_bytes);
    /* This schedules teardown outside the websocket/player/decoder callbacks.
     * Never manufacture PlayEnd: teardown must stop output before recording. */
    voice_chat_fail_epoch(epoch);
}



static void lingxin_playback_watchdog_thread(void *arg)
{
    lingxin_playback_watchdog_ctx_t *ctx = arg;
    if (ctx == NULL) return;
    uint32_t written = s_stream_pcm_written_bytes;
    uint32_t progress_at = lingxin_playback_now_ms();
    while (ctx->generation == __atomic_load_n(&s_playback_generation, __ATOMIC_ACQUIRE) &&
           s_stream_playing && !s_stream_write_failed) {
        lisa_thread_mdelay(200);
        if (ctx->generation != __atomic_load_n(&s_playback_generation, __ATOMIC_ACQUIRE) ||
            !s_stream_playing || s_stream_write_failed) break;
        /* Querying app_player position takes its operation mutex and could
         * itself hang behind a stuck reset. PCM writes are paced by the
         * output FIFO; after finish only the small PCM FIFO remains to drain. */
        if (written != s_stream_pcm_written_bytes) {
            written = s_stream_pcm_written_bytes;
            progress_at = lingxin_playback_now_ms();
        }
        if ((uint32_t)(lingxin_playback_now_ms() - progress_at) >= LINGXIN_PLAYBACK_STALL_MS) {
            lingxin_stream_fail(ctx->generation, "no_playback_progress");
            break;
        }
    }
    lisa_mem_free(ctx);
}

static void lingxin_start_playback_watchdog(void)
{
    lingxin_playback_watchdog_ctx_t *ctx =
        (lingxin_playback_watchdog_ctx_t *)lisa_mem_calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        LOGE("[Lingxin] playback watchdog alloc failed");
        lingxin_stream_fail(s_playback_generation, "watchdog_alloc");
        return;
    }

    ctx->generation = s_playback_generation;
    ctx->epoch = s_stream_epoch;

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lx.play.wd",
        .stack_size = 4 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_playback_watchdog_thread, ctx);
    if (thread == NULL) {
        LOGE("[Lingxin] playback watchdog create failed");
        lisa_mem_free(ctx);
        lingxin_stream_fail(s_playback_generation, "watchdog_thread");
    }
}

static void lingxin_stream_writer_thread(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    lingxin_stream_write_item_t item;
    HMP3Decoder decoder = MP3InitDecoder();
    uint8_t *pending = NULL;
    uint32_t pending_len = 0, pending_cap = 0;
    short *pcm = lisa_mem_alloc(LINGXIN_MP3_DECODE_MAX_SAMPLES * sizeof(short));
    if (decoder == NULL || pcm == NULL) lingxin_stream_fail(generation, "decoder_alloc");

    while (generation == s_playback_generation && lingxin_stream_writer_is_current()) {
        if (lisa_queue_pop(s_stream_write_queue, &item, sizeof(item), 100) != LISA_OK) continue;
        bool ok = true;
        if (item.end) {
            if (pending_len > 0U) ok = lingxin_stream_decode_pending(decoder, pending, &pending_len, pcm);
            if (ok) ok = lingxin_stream_flush_pcm_prefill();
            if (ok && lingxin_stream_writer_is_current()) {
                /* Publish before finish: COMPLETE is allowed to arrive synchronously. */
                s_stream_finish_submitted = true;
                ok = lingxin_compat_player_put_stream_data(LINGXIN_PLAYBACK_PLAYER_TYPE, NULL, 0, 0) >= 0;
            }
        } else if (item.data != NULL && item.len > 0U) {
            ok = lingxin_mp3_decode_buffer_append(&pending, &pending_len, &pending_cap, item.data, item.len) &&
                 lingxin_stream_decode_pending(decoder, pending, &pending_len, pcm);
        }
        bool end = item.end;
        lingxin_stream_write_item_free(&item);
        if (!ok && generation == s_playback_generation && s_stream_writer_running)
            lingxin_stream_fail(generation, "decode_or_pcm_write");
        if (end || !ok) break;
    }
    lisa_mem_free(pending);
    lisa_mem_free(pcm);
    if (decoder != NULL) MP3FreeDecoder(decoder);
    /* No player calls, queue access or shared-buffer cleanup after publishing
     * exited. Init/terminate may now safely reclaim the old response. */
    s_stream_writer_running = false;
    __atomic_store_n(&s_stream_writer_exited, true, __ATOMIC_RELEASE);
}

static bool lingxin_stream_writer_start(uint32_t generation)
{
    if (generation != s_playback_generation) return false;
    if (s_stream_writer_running && s_stream_write_queue != NULL) {
        return true;
    }
    if (!s_stream_writer_exited) {
        LOGW("[Lingxin] previous stream writer has not exited");
        return false;
    }

    if (s_stream_write_queue == NULL) {
        s_stream_write_queue = lisa_queue_create(LINGXIN_STREAM_QUEUE_DEPTH,
                                                 (uint8_t *)"lx_mp3_q",
                                                 sizeof(lingxin_stream_write_item_t));
        if (s_stream_write_queue == NULL) {
            LOGE("[Lingxin] mp3->pcm stream queue create failed");
            return false;
        }
    }

    s_stream_writer_exited = false;
    s_stream_writer_running = true;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lx.pcm.wr",
        .stack_size = 8 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    s_stream_writer_generation = generation;
    s_stream_writer_thread = lisa_thread_create(&attr, lingxin_stream_writer_thread,
                                                (void *)(uintptr_t)s_stream_writer_generation);
    if (s_stream_writer_thread == NULL) {
        LOGE("[Lingxin] mp3->pcm stream writer create failed");
        s_stream_writer_running = false;
        s_stream_writer_exited = true;
        lisa_queue_delete(s_stream_write_queue);
        s_stream_write_queue = NULL;
        return false;
    }

    return true;
}

static bool lingxin_stream_enqueue_copy(const void *data, uint32_t len, bool end, uint32_t generation)
{
    taskENTER_CRITICAL();
    lisa_queue_t *queue = s_stream_write_queue;
    if (generation != s_playback_generation || queue == NULL || !s_stream_writer_running || s_stream_write_failed ||
        len > LINGXIN_STREAM_QUEUE_MAX_BYTES - s_stream_queue_bytes) {
        taskEXIT_CRITICAL();
        return false;
    }
    s_stream_enqueue_active++;
    taskEXIT_CRITICAL();
    bool ok = true;
    uint32_t offset = 0;
    do {
        lingxin_stream_write_item_t item = {.end = end};
        if (!end) {
            item.len = len - offset;
            if (item.len > LINGXIN_STREAM_CHUNK_MAX_BYTES) item.len = LINGXIN_STREAM_CHUNK_MAX_BYTES;
            item.data = lisa_mem_alloc(item.len);
            if (item.data == NULL) {ok = false; break;}
            memcpy(item.data, (const uint8_t *)data + offset, item.len);
            taskENTER_CRITICAL();
            s_stream_queue_bytes += item.len;
            if (s_stream_queue_bytes > s_stream_queue_peak_bytes) s_stream_queue_peak_bytes = s_stream_queue_bytes;
            taskEXIT_CRITICAL();
        }
        /* The websocket receive task must never wait for audible playback. */
        if (generation != s_playback_generation || !s_stream_writer_running ||
            lisa_queue_push(queue, &item, sizeof(item), 0) != LISA_OK) {
            lingxin_stream_write_item_free(&item);
            ok = false;
            break;
        }
        offset += item.len;
    } while (!end && offset < len);
    taskENTER_CRITICAL();
    s_stream_enqueue_active--;
    taskEXIT_CRITICAL();
    return ok;
}

static bool lingxin_stream_start_playback(uint32_t generation)
{
    if (generation != s_playback_generation) return false;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(s_stream_owner_generation)) return false;
#endif
    if (s_stream_mode_started) return true;
    s_playback_input_blocked = true;
    s_stream_playing = true;
    s_stream_mode_started = true;
    s_stream_last_progress_ms = lingxin_playback_now_ms();
    if (lingxin_compat_player_play(LINGXIN_PLAYBACK_PLAYER_TYPE, LINGXIN_STREAM_URL,
                                   lingxin_audio_player_status_cb, generation) < 0 || !lingxin_stream_writer_start(generation)) return false;
    lingxin_start_playback_watchdog();
    return !s_stream_write_failed && generation == s_playback_generation;
}





static const char *lingxin_player_event_name(uint16_t st)
{
    switch (st) {
    case APP_PLAYER_PREPARING:
        return "APP_PLAYER_PREPARING";
    case PLAYER_EVT_ERROR:
        return "PLAYER_EVT_ERROR";
    case PLAYER_EVT_PREPARED:
        return "PLAYER_EVT_PREPARED";
    case PLAYER_EVT_PLAYING:
        return "PLAYER_EVT_PLAYING";
    case PLAYER_EVT_PAUSED:
        return "PLAYER_EVT_PAUSED";
    case PLAYER_EVT_STOPED:
        return "PLAYER_EVT_STOPED";
    case PLAYER_EVT_NEARLY_COMPLETE:
        return "PLAYER_EVT_NEARLY_COMPLETE";
    case PLAYER_EVT_PLAYBACK_COMPLETE:
        return "PLAYER_EVT_PLAYBACK_COMPLETE";
    case PLAYER_EVT_SEEKING:
        return "PLAYER_EVT_SEEKING";
    case PLAYER_EVT_SEEK_COMPLETE:
        return "PLAYER_EVT_SEEK_COMPLETE";
    case PLAYER_EVT_INIT:
        return "PLAYER_EVT_INIT";
    default:
        return "PLAYER_EVT_UNKNOWN";
    }
}

static uint32_t lingxin_audio_abs16(int16_t value)
{
    return value < 0 ? (uint32_t)(-(int32_t)value) : (uint32_t)value;
}

static void lingxin_auto_end_thread(void *arg)
{
    lingxin_recorder_ctx_t *recorder = (lingxin_recorder_ctx_t *)arg;

    LOGI("[Lingxin] auto end thread begin");
    if (recorder != NULL && !recorder->auto_end_speech_seen) {
        LOGI("[Lingxin] local VAD no speech, exit chat without server audio submission");
        lingxin_chat_upload_manager_vad_exit();
    } else {
        int ret = stop_chat_record(NULL);
        LOGI("[Lingxin] auto end stop_chat_record ret=%d", ret);
    }
    s_auto_end_thread_running = false;
}

static void lingxin_auto_end_request(lingxin_recorder_ctx_t *recorder, const char *reason)
{
    if (recorder == NULL || recorder->auto_end_requested || s_auto_end_thread_running) {
        return;
    }

    recorder->auto_end_requested = true;
    s_auto_end_thread_running = true;

    LOGI("[Lingxin] auto end requested: reason=%s, total_ms=%u, speech_ms=%u, silence_ms=%u",
         reason ? reason : "",
         recorder->auto_end_total_ms,
         recorder->auto_end_speech_ms,
         recorder->auto_end_silence_ms);
    lingxin_server_diag_timing_mark(reason && strcmp(reason, "no_speech_timeout") == 0
                                        ? "client_no_speech_timeout"
                                        : "client_record_watchdog",
                                    (int)recorder->auto_end_total_ms);

    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lx.auto.end",
        .stack_size = 8 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_auto_end_thread, recorder);
    if (thread == NULL) {
        LOGE("[Lingxin] auto end thread create failed");
        s_auto_end_thread_running = false;
        recorder->auto_end_requested = false;
    }
}

static void lingxin_auto_end_update(lingxin_recorder_ctx_t *recorder, uint32_t avg_abs, uint32_t duration_ms)
{
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    /* The official v2 half-duplex API makes the cloud authoritative for both
     * sentence end (vad_end) and no-input timeout (vad_exit). Local amplitude
     * heuristics must not stop or suppress the microphone stream. */
    (void)recorder;
    (void)avg_abs;
    (void)duration_ms;
    return;
#else
    if (recorder == NULL || recorder->auto_end_requested || duration_ms == 0U) {
        return;
    }

    /* Recorder capture starts before task_started. Do not let the wake tone,
     * WebSocket setup time or stale buffered samples seed this turn's VAD. */
    if (!module_record_is_server_ready()) {
        recorder->auto_end_total_ms = 0;
        recorder->auto_end_speech_ms = 0;
        recorder->auto_end_silence_ms = 0;
        recorder->auto_end_speech_seen = false;
        return;
    }

    recorder->auto_end_total_ms += duration_ms;

    if (avg_abs >= LINGXIN_AUTO_END_SPEECH_AVG_ABS) {
        recorder->auto_end_speech_ms += duration_ms;
        recorder->auto_end_silence_ms = 0;
        if (recorder->auto_end_speech_ms >= LINGXIN_AUTO_END_MIN_SPEECH_MS) {
            recorder->auto_end_speech_seen = true;
        }
    } else if (!recorder->auto_end_speech_seen) {
        /* Speech qualification must be continuous. Short clicks and the tail
         * of the local wake prompt must not accumulate into a speech hit. */
        recorder->auto_end_speech_ms = 0;
        recorder->auto_end_silence_ms = 0;
    } else if (avg_abs <= LINGXIN_AUTO_END_SILENCE_AVG_ABS) {
        recorder->auto_end_silence_ms += duration_ms;
    }

    if (recorder->auto_end_total_ms >= LINGXIN_AUTO_END_MAX_RECORD_MS) {
        lingxin_auto_end_request(recorder, "max_record_duration");
        return;
    }

    if (recorder->auto_end_total_ms < LINGXIN_AUTO_END_MIN_RECORD_MS) {
        return;
    }

    if (recorder->auto_end_speech_seen && recorder->auto_end_silence_ms >= LINGXIN_AUTO_END_SILENCE_MS) {
        lingxin_auto_end_request(recorder, "silence_after_speech");
    }
#endif
}

static int lingxin_audio_player_status_cb(uint16_t st)
{
    return lingxin_audio_player_status_for_generation(st, s_playback_generation);
}

static int lingxin_audio_player_status_for_generation(uint16_t st, uint32_t generation)
{
    LOGI("[Lingxin] cloud player event=%s(%u), chunks=%u, bytes=%u",
         lingxin_player_event_name(st),
         st,
         s_stream_chunk_count,
         s_stream_bytes_total);

    bool playing = false, complete = false, stopped = false;
    taskENTER_CRITICAL();
    if (generation != s_playback_generation) {
        taskEXIT_CRITICAL();
        return 0;
    }
    if (st == PLAYER_EVT_PLAYING) {
        s_stream_player_started = true;
        playing = true;
    } else if (st == PLAYER_EVT_PLAYBACK_COMPLETE) {
        if (s_stream_playing && s_stream_finish_submitted && !s_stream_write_failed) {
            s_stream_playing = false;
            s_stream_mode_started = false;
            s_stream_writer_running = false;
            s_stream_audio_ended = true;
            s_playback_input_blocked = false;
            complete = true;
        }
    } else if (st == PLAYER_EVT_ERROR || st == PLAYER_EVT_STOPED) {
        if (s_stream_suppress_stop_event) {
            s_stream_suppress_stop_event = false;
            taskEXIT_CRITICAL();
            return 0;
        }
        s_stream_mode_started = false;
        s_stream_writer_running = false;
        s_stream_audio_ended = true;
        s_stream_playing = false;
        stopped = true;
        /* Buffer cleanup belongs to init/terminate, not a potentially late
         * player notification that could free the next response's buffer. */
    }
    taskEXIT_CRITICAL();
    if (playing) {
        lingxin_server_diag_timing_mark("player_evt_playing", (int)s_stream_chunk_count);
        voice_msg_pub_timeout(VOICE_MSG_PLAYER_TTS_PLAYING, NULL, 0, 0);
    }
    if (complete) {
        lingxin_server_diag_timing_mark("tts_playback_complete", (int)s_stream_chunk_count);
        lingxin_post_playback_event_async(Lingxin_Download_Audio_PlayEnd, "player_complete", generation);
    }
    if (stopped) {
        lingxin_stream_fail(generation, "player_stop_or_error");
        voice_msg_pub_timeout(VOICE_MSG_PLAYER_TTS_STOPED, NULL, 0, 0);
    }

    return 0;
}

static int lingxin_local_player_status_cb(uint16_t st)
{
    LOGI("[Lingxin] local player event=%d", st);

    lingxin_local_player_ctx_t *player = s_current_local_player;
    if (!player || !player->callback) {
        return 0;
    }

    if (st == PLAYER_EVT_PLAYBACK_COMPLETE) {
        player->callback(0);
    } else if (st == PLAYER_EVT_ERROR || st == PLAYER_EVT_STOPED) {
        player->callback(-1);
    }

    return 0;
}

static void lingxin_record_sender_thread(void *arg)
{
    lingxin_recorder_ctx_t *recorder = (lingxin_recorder_ctx_t *)arg;
    lingxin_record_frame_t frame;

    LOGI("[Lingxin] record sender thread begin");
    while (recorder != NULL && __atomic_load_n(&recorder->sender_running, __ATOMIC_ACQUIRE)) {
        if (lisa_queue_pop(recorder->queue, &frame, sizeof(frame), 100) != LISA_OK) {
            continue;
        }

        /* The gate may close after this frame was popped. Recheck at the
         * final cloud boundary so stale wake-prompt audio cannot escape. */
        if (!lingxin_audio_uplink_gate_is_open(NULL)) {
            s_record_uplink_gate_drop_count++;
            continue;
        }

        if (frame.len > 0 && frame.len <= sizeof(frame.data)) {
            lingxin_process_record_data(frame.data, frame.len);
            s_record_send_count++;
            if (s_record_send_count == 1U || (s_record_send_count % 100U) == 0U) {
                LOGI("[Lingxin] record sender sent=%u, drop=%u, last_len=%u",
                     s_record_send_count,
                     s_record_drop_count,
                     frame.len);
            }
        }
    }
    __atomic_store_n(&recorder->sender_exited, true, __ATOMIC_RELEASE);
    LOGI("[Lingxin] record sender thread end, sent=%u, drop=%u", s_record_send_count, s_record_drop_count);
}

static bool lingxin_record_sender_stop(lingxin_recorder_ctx_t *ctx)
{
    if (ctx == NULL) {
        return true;
    }

    __atomic_store_n(&ctx->sender_running, false, __ATOMIC_RELEASE);
    for (uint32_t waited_ms = 0U;
         !__atomic_load_n(&ctx->sender_exited, __ATOMIC_ACQUIRE) && waited_ms < 2500U;
         waited_ms += 10U) {
        lisa_thread_mdelay(10);
    }
    if (!__atomic_load_n(&ctx->sender_exited, __ATOMIC_ACQUIRE)) {
        LOGE("[Lingxin] record sender stop timeout; preserve queue/context to avoid use-after-free");
        return false;
    }
    ctx->sender_thread = NULL;

    if (ctx->queue != NULL) {
        lisa_queue_clear(ctx->queue);
        lisa_queue_delete(ctx->queue);
        ctx->queue = NULL;
    }
    return true;
}

static bool lingxin_record_sender_start(lingxin_recorder_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }

    if (!__atomic_load_n(&ctx->sender_exited, __ATOMIC_ACQUIRE)) {
        LOGE("[Lingxin] previous record sender has not exited");
        return false;
    }

    if (ctx->queue == NULL) {
        ctx->queue = lisa_queue_create(LINGXIN_RECORDER_QUEUE_DEPTH,
                                       (uint8_t *)"lx_rec_q",
                                       sizeof(lingxin_record_frame_t));
        if (ctx->queue == NULL) {
            LOGE("[Lingxin] record sender queue create failed");
            return false;
        }
    }

    __atomic_store_n(&ctx->sender_exited, false, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->sender_running, true, __ATOMIC_RELEASE);
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lx.rec.send",
        .stack_size = 8 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    ctx->sender_thread = lisa_thread_create(&attr, lingxin_record_sender_thread, ctx);
    if (ctx->sender_thread == NULL) {
        LOGE("[Lingxin] record sender thread create failed");
        __atomic_store_n(&ctx->sender_running, false, __ATOMIC_RELEASE);
        __atomic_store_n(&ctx->sender_exited, true, __ATOMIC_RELEASE);
        lisa_queue_delete(ctx->queue);
        ctx->queue = NULL;
        return false;
    }

    return true;
}

static void lingxin_record_enqueue(lingxin_recorder_ctx_t *recorder, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (recorder == NULL || recorder->queue == NULL || data == NULL || len == 0) {
        return;
    }

    uint32_t target = (recorder->frame_size > 0 && recorder->frame_size <= LINGXIN_RECORDER_FRAME_BYTES)
                          ? (uint32_t)recorder->frame_size
                          : LINGXIN_RECORDER_FRAME_BYTES;

    while (len > 0U) {
        uint32_t room = target - recorder->pending_len;
        uint32_t chunk = len < room ? len : room;

        memcpy(recorder->pending + recorder->pending_len, p, chunk);
        recorder->pending_len += (uint16_t)chunk;

        p += chunk;
        len -= chunk;

        if (recorder->pending_len == target) {
            lingxin_record_frame_t frame;

            frame.len = (uint16_t)target;
            memcpy(frame.data, recorder->pending, target);

            if (lisa_queue_push(recorder->queue, &frame, sizeof(frame), 0) != LISA_OK) {
                s_record_drop_count++;
                if (s_record_drop_count == 1U || (s_record_drop_count % 50U) == 0U) {
                    LOGW("[Lingxin] record queue full, drop=%u", s_record_drop_count);
                }
            }

            recorder->pending_len = 0;
        }
    }
}

static void lingxin_vad_preroll_put(lingxin_recorder_ctx_t *recorder, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (recorder == NULL || data == NULL || len == 0U) {
        return;
    }

    if (len >= LINGXIN_VAD_PREROLL_BYTES) {
        p += len - LINGXIN_VAD_PREROLL_BYTES;
        len = LINGXIN_VAD_PREROLL_BYTES;
        memcpy(recorder->vad_preroll, p, len);
        recorder->vad_preroll_write = 0U;
        recorder->vad_preroll_len = len;
        return;
    }

    uint32_t first = LINGXIN_VAD_PREROLL_BYTES - recorder->vad_preroll_write;
    if (first > len) {
        first = len;
    }
    memcpy(recorder->vad_preroll + recorder->vad_preroll_write, p, first);
    if (len > first) {
        memcpy(recorder->vad_preroll, p + first, len - first);
    }
    recorder->vad_preroll_write = (recorder->vad_preroll_write + len) % LINGXIN_VAD_PREROLL_BYTES;
    recorder->vad_preroll_len += len;
    if (recorder->vad_preroll_len > LINGXIN_VAD_PREROLL_BYTES) {
        recorder->vad_preroll_len = LINGXIN_VAD_PREROLL_BYTES;
    }
}

static void lingxin_vad_preroll_flush(lingxin_recorder_ctx_t *recorder)
{
    if (recorder == NULL || recorder->vad_preroll_len == 0U) {
        return;
    }

    uint32_t start = (recorder->vad_preroll_write + LINGXIN_VAD_PREROLL_BYTES -
                      recorder->vad_preroll_len) % LINGXIN_VAD_PREROLL_BYTES;
    uint32_t first = LINGXIN_VAD_PREROLL_BYTES - start;
    if (first > recorder->vad_preroll_len) {
        first = recorder->vad_preroll_len;
    }

    lingxin_record_enqueue(recorder, recorder->vad_preroll + start, first);
    if (recorder->vad_preroll_len > first) {
        lingxin_record_enqueue(recorder, recorder->vad_preroll, recorder->vad_preroll_len - first);
    }
    recorder->vad_preroll_len = 0U;
    recorder->vad_preroll_write = 0U;
}

static void lingxin_record_enqueue_after_vad(lingxin_recorder_ctx_t *recorder, const void *data, uint32_t len)
{
    if (recorder == NULL || data == NULL || len == 0U) {
        return;
    }

#ifndef CONFIG_LINGXIN_LOCAL_VAD_UPLOAD_GATE
    /* Let Lingxin cloud VAD see quiet/far-field speech. The local detector is
     * still kept for the bounded auto-end safety path, but no longer drops
     * audio before its amplitude threshold is reached. */
    lingxin_record_enqueue(recorder, data, len);
    return;
#else
    if (!recorder->auto_end_speech_seen) {
        lingxin_vad_preroll_put(recorder, data, len);
        return;
    }

    if (!recorder->vad_upload_enabled) {
        recorder->vad_upload_enabled = true;
        LOGI("[Lingxin] local VAD accepted speech, release preroll=%u ms/%u bytes",
             LINGXIN_VAD_PREROLL_MS,
             recorder->vad_preroll_len);
        lingxin_vad_preroll_flush(recorder);
    }
    lingxin_record_enqueue(recorder, data, len);
#endif
}

static void lingxin_record_flush_pending(lingxin_recorder_ctx_t *recorder)
{
    if (recorder == NULL || recorder->queue == NULL || recorder->pending_len == 0U) {
        return;
    }

    uint32_t target = (recorder->frame_size > 0 && recorder->frame_size <= LINGXIN_RECORDER_FRAME_BYTES)
                          ? (uint32_t)recorder->frame_size
                          : LINGXIN_RECORDER_FRAME_BYTES;
    lingxin_record_frame_t frame;

    frame.len = (uint16_t)target;
    memcpy(frame.data, recorder->pending, recorder->pending_len);
    memset(frame.data + recorder->pending_len, 0, target - recorder->pending_len);

    if (lisa_queue_push(recorder->queue, &frame, sizeof(frame), 20) != LISA_OK) {
        s_record_drop_count++;
        LOGW("[Lingxin] record queue full while flushing, drop=%u", s_record_drop_count);
    }

    recorder->pending_len = 0;
}

static void lingxin_record_audio_callback(const lisa_audio_event_t *event, void *user_data)
{
    lingxin_recorder_ctx_t *recorder = (lingxin_recorder_ctx_t *)user_data;
    if (!recorder || !recorder->opened || !event || !event->record_buffer || event->record_samples == 0) {
        return;
    }

    /* Local prompts are played through the loudspeaker next to the
     * microphone.  Drop it at the capture boundary: do not queue it, retain it
     * as VAD preroll, or let it advance any local speech counters. */
    if (!lingxin_audio_uplink_gate_is_open(NULL)) {
        s_record_uplink_gate_drop_count++;
        lingxin_record_discard_buffered(recorder);
        if (s_record_uplink_gate_drop_count == 1U) {
            lingxin_trace_set("audio:wakeup_prompt_drop_begin");
            LOGI("[Lingxin] dropping microphone frames while local audio/uplink gate is active");
        }
        return;
    }

    const int16_t *samples = (const int16_t *)event->record_buffer;
    uint32_t sample_count = event->record_samples;
    s_record_frame_count++;
    s_record_bytes_total += sample_count * LINGXIN_RECORDER_CHANNEL_BYTES;

    if ((sample_count & 1U) == 0U) {
        uint32_t pairs = sample_count / 2U;
        uint64_t sum_abs_l = 0;
        uint64_t sum_abs_r = 0;
        int64_t sum_l = 0;
        int16_t min_l = samples[0];
        int16_t max_l = samples[0];
        int16_t min_r = samples[1];
        int16_t max_r = samples[1];

        for (uint32_t i = 0; i < pairs; i++) {
            int16_t left = samples[i * 2U];
            int16_t right = samples[i * 2U + 1U];

            if (left < min_l) {
                min_l = left;
            }
            if (left > max_l) {
                max_l = left;
            }
            if (right < min_r) {
                min_r = right;
            }
            if (right > max_r) {
                max_r = right;
            }

            sum_abs_l += lingxin_audio_abs16(left);
            sum_abs_r += lingxin_audio_abs16(right);
            sum_l += left;
        }

        uint32_t avg_abs_l = (uint32_t)(sum_abs_l / pairs);
        int32_t mean_l = (int32_t)(sum_l / (int64_t)pairs);
        uint64_t vad_abs_sum_l = 0;
        for (uint32_t i = 0; i < pairs; i++) {
            int32_t centered = (int32_t)samples[i * 2U] - mean_l;
            vad_abs_sum_l += centered < 0 ? (uint32_t)(-centered) : (uint32_t)centered;
        }
        uint32_t vad_abs_l = (uint32_t)(vad_abs_sum_l / pairs);
        uint32_t frame_ms = (pairs * 1000U) / LINGXIN_RECORDER_RATE_HZ;
        lingxin_auto_end_update(recorder, vad_abs_l, frame_ms);

        if (s_record_frame_count == 1U || (s_record_frame_count % 100U) == 0U) {
            LOGI("[Lingxin] record frames=%u, samples=%u, bytes=%u, L[min=%d max=%d avg_abs=%u vad_abs=%u], R[min=%d max=%d avg_abs=%u]",
                 s_record_frame_count,
                 sample_count,
                 s_record_bytes_total,
                 min_l,
                 max_l,
                 avg_abs_l,
                 vad_abs_l,
                 min_r,
                 max_r,
                 (uint32_t)(sum_abs_r / pairs));
        }
    } else {
        uint64_t sum_abs = 0;
        for (uint32_t i = 0; i < sample_count; i++) {
            sum_abs += lingxin_audio_abs16(samples[i]);
        }
        uint32_t avg_abs = (uint32_t)(sum_abs / sample_count);
        uint32_t frame_ms = (sample_count * 1000U) / LINGXIN_RECORDER_RATE_HZ;
        lingxin_auto_end_update(recorder, avg_abs, frame_ms);

        if (s_record_frame_count == 1U || (s_record_frame_count % 100U) == 0U) {
            LOGI("[Lingxin] record frames=%u, input_samples=%u, input_bytes_total=%u",
                 s_record_frame_count,
                 sample_count,
                 s_record_bytes_total);
        }
    }

    if ((sample_count & 1U) == 0U) {
        uint32_t frames = sample_count / 2U;
        int16_t mono[256];

        while (frames > 0U) {
            uint32_t chunk = frames > (sizeof(mono) / sizeof(mono[0])) ? (sizeof(mono) / sizeof(mono[0])) : frames;

            for (uint32_t i = 0; i < chunk; i++) {
                mono[i] = samples[i * 2U];
            }

            lingxin_record_enqueue_after_vad(recorder, mono, chunk * sizeof(mono[0]));
            samples += chunk * 2U;
            frames -= chunk;
        }
    } else {
        lingxin_record_enqueue_after_vad(recorder,
                                         event->record_buffer,
                                         sample_count * LINGXIN_RECORDER_CHANNEL_BYTES);
    }
}

#ifndef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
lingxin_recorder_t lingxin_recorder_create()
{
    LOGI("[Lingxin] recorder create");
    lingxin_recorder_ctx_t *recorder = lisa_mem_calloc(1, sizeof(*recorder));
    if (!recorder) {
        LOGE("[Lingxin] recorder create failed: no memory");
        return NULL;
    }

    recorder->frame_size = LINGXIN_RECORDER_RATE_HZ * LINGXIN_RECORDER_CHANNEL_BYTES / 1000 *
                           LINGXIN_RECORDER_FRAME_MS;
    __atomic_store_n(&recorder->sender_exited, true, __ATOMIC_RELEASE);
    return recorder;
}

void lingxin_recorder_open(lingxin_recorder_t recorder, lingxin_recorder_open_param_t *param, lingxin_recorder_callback_t callback)
{
    lingxin_recorder_ctx_t *ctx = (lingxin_recorder_ctx_t *)recorder;
    int result = -1;

    LOGI("[Lingxin] recorder open begin, recorder=%p, requested_frame_size=%d",
         recorder,
         param ? param->frame_size : 0);

    if (ctx) {
        if (param && param->frame_size > 0) {
            ctx->frame_size = param->frame_size;
        }

#if LINGXIN_RECORDER_BYPASS_AUDIO0
        lingxin_trace_set("chat:recorder_open_bypass_audio0");
        LOGW("[Lingxin] recorder open bypass audio0/register_callback for freeze bisection");
        ctx->opened = true;
        result = 0;
#else
        ctx->audio_dev = lisa_device_get("audio0");
        if (ctx->audio_dev) {
            s_record_frame_count = 0;
            s_record_bytes_total = 0;
            s_record_drop_count = 0;
            s_record_send_count = 0;
            ctx->pending_len = 0;
            ctx->auto_end_speech_seen = false;
            ctx->auto_end_requested = false;
            ctx->vad_upload_enabled = false;
            ctx->auto_end_total_ms = 0;
            ctx->auto_end_speech_ms = 0;
            ctx->auto_end_silence_ms = 0;
            ctx->vad_preroll_write = 0;
            ctx->vad_preroll_len = 0;
            if (lingxin_record_sender_start(ctx)) {
                int ret = lisa_audio_register_callback(ctx->audio_dev, lingxin_record_audio_callback, ctx);
                if (ret == LISA_DEVICE_OK) {
                    ctx->opened = true;
                    s_active_recorder = ctx;
                    result = 0;
                    LOGI("[Lingxin] recorder open ok, audio_dev=%p, frame_size=%d", ctx->audio_dev, ctx->frame_size);
                } else {
                    (void)lingxin_record_sender_stop(ctx);
                    LOGE("[Lingxin] recorder open failed: callback register ret=%d", ret);
                }
            } else {
                LOGE("[Lingxin] recorder open failed: sender start failed");
            }
            if (result == 0) {
                ctx->opened = true;
            }
        } else {
            LOGE("[Lingxin] recorder open failed: audio0 not ready");
        }
#endif
    } else {
        LOGE("[Lingxin] recorder open failed: recorder is null");
    }

    if (callback) {
        LOGI("[Lingxin] recorder open callback result=%d", result);
        callback(result);
    }
}

void lingxin_recorder_close(lingxin_recorder_t recorder, lingxin_recorder_callback_t callback)
{
    lingxin_recorder_ctx_t *ctx = (lingxin_recorder_ctx_t *)recorder;
    int result = -1;

    if (ctx) {
        if (s_active_recorder == ctx) {
            s_active_recorder = NULL;
        }
        if (ctx->audio_dev && ctx->opened) {
            lisa_audio_unregister_callback(ctx->audio_dev, lingxin_record_audio_callback);
        }
        ctx->opened = false;
        lingxin_record_flush_pending(ctx);
        result = lingxin_record_sender_stop(ctx) ? 0 : -1;
    }

    LOGI("[Lingxin] recorder close result=%d, frames=%u, bytes=%u, sent=%u, drop=%u",
         result,
         s_record_frame_count,
         s_record_bytes_total,
         s_record_send_count,
         s_record_drop_count);

    if (callback) {
        callback(result);
    }
}

void lingxin_recorder_destroy(lingxin_recorder_t recorder)
{
    lingxin_recorder_ctx_t *ctx = (lingxin_recorder_ctx_t *)recorder;
    if (!ctx) {
        return;
    }

    if (s_active_recorder == ctx) {
        s_active_recorder = NULL;
    }

    if (ctx->audio_dev && ctx->opened) {
        lisa_audio_unregister_callback(ctx->audio_dev, lingxin_record_audio_callback);
    }
    ctx->opened = false;
    lingxin_record_flush_pending(ctx);
    if (!lingxin_record_sender_stop(ctx)) {
        LOGE("[Lingxin] recorder destroy deferred because sender is still active");
        return;
    }
    LOGI("[Lingxin] recorder destroy, frames=%u, bytes=%u, sent=%u, drop=%u",
         s_record_frame_count,
         s_record_bytes_total,
         s_record_send_count,
         s_record_drop_count);
    lisa_mem_free(ctx);
}

int lingxin_recorder_get_size_per_ms()
{
    return 32;
}
#endif

lingxin_local_player_t lingxin_local_player_create()
{
    LOGI("[Lingxin] local player create");
    lingxin_local_player_ctx_t *player = lisa_mem_calloc(1, sizeof(*player));
    if (player) {
        player->volume = 60;
    } else {
        LOGE("[Lingxin] local player create failed: no memory");
    }
    return player;
}

void lingxin_local_player_play(lingxin_local_player_t player, lingxin_local_player_play_param_t *param, lingxin_local_player_callback_t callback)
{
    lingxin_local_player_ctx_t *ctx = (lingxin_local_player_ctx_t *)player;
    if (!ctx || !param || !param->audio_path || param->audio_path[0] == '\0') {
        LOGE("[Lingxin] local player play failed: invalid param, player=%p", player);
        if (callback) {
            callback(-1);
        }
        return;
    }

    LOGI("[Lingxin] local player play path=%s, volume=%d, initial_volume=%d",
         param->audio_path,
         ctx->volume,
         param->initial_volume);
    ctx->callback = callback;
    s_current_local_player = ctx;
    if (param->initial_volume >= 0 && param->initial_volume <= 100) {
        ctx->volume = param->initial_volume;
    }
    lingxin_compat_player_volume(PLAYER_T_TONE, (uint8_t)ctx->volume);
    (void)lingxin_compat_player_play(PLAYER_T_TONE, param->audio_path, lingxin_local_player_status_cb, 0U);
}

void lingxin_local_player_set_volume(lingxin_local_player_t player, int volume)
{
    lingxin_local_player_ctx_t *ctx = (lingxin_local_player_ctx_t *)player;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    if (ctx) {
        ctx->volume = volume;
    }
    LOGI("[Lingxin] local player set volume=%d", volume);
    lingxin_compat_player_volume(PLAYER_T_TONE, (uint8_t)volume);
}

void lingxin_local_player_destory(lingxin_local_player_t player)
{
    lingxin_local_player_ctx_t *ctx = (lingxin_local_player_ctx_t *)player;
    if (!ctx) {
        return;
    }

    if (s_current_local_player == ctx) {
        s_current_local_player = NULL;
    }
    LOGI("[Lingxin] local player destroy");
    lisa_mem_free(ctx);
}

void module_bufferPlay_audioInit(PlaybackEventHandler callback, void *user_data)
{
    s_stream_response_ready = false;
    uint32_t generation = lingxin_playback_advance_generation();
    if (generation == 0U) return;
    s_stream_response_id = generation;
    if (!lingxin_playback_lifecycle_lock()) {voice_chat_fail_current_round(); return;}
    if (!lingxin_ensure_termination_worker()) {
        lingxin_playback_lifecycle_unlock();
        voice_chat_fail_current_round();
        return;
    }
    if (!lingxin_stream_writer_stop()) {
        lingxin_playback_lifecycle_unlock();
        voice_chat_fail_current_round();
        return; /* An old worker still owns its buffers: never reuse them. */
    }
    if ((s_stream_playing || s_stream_mode_started || s_player_cleanup_started) &&
        !lingxin_compat_player_stop_sync(LINGXIN_PLAYBACK_PLAYER_TYPE)) {
        lingxin_playback_lifecycle_unlock();
        voice_chat_fail_current_round();
        return;
    }
    if (generation != s_playback_generation) {lingxin_playback_lifecycle_unlock(); return;}
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    s_stream_owner_generation = service_offline_action_generation();
#endif
    s_playback_callback = callback;
    s_playback_user_data = user_data;
    s_stream_epoch = voice_chat_current_epoch();
    s_stream_playing = false;
    s_stream_mode_started = false;
    s_stream_player_started = false;
    s_stream_codex_prefill = __atomic_load_n(&s_next_codex_prefill, __ATOMIC_ACQUIRE);
    s_stream_write_failed = false;
    s_stream_suppress_stop_event = false;
    s_stream_audio_ended = false;
    s_stream_finish_submitted = false;
    lingxin_tts_suppress_new_response(&s_tts_suppress_policy);
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
    if (lingxin_server_asr_probe_active())
        lingxin_tts_suppress_request(&s_tts_suppress_policy);
#endif
    s_stream_chunk_count = s_stream_bytes_total = 0U;
    s_stream_decode_frame_count = s_stream_pcm_bytes_total = 0U;
    lingxin_resample_reset();
    s_stream_pcm_written_bytes = s_stream_queue_peak_bytes = 0U;
    lingxin_stream_pcm_prefill_reset();
    s_stream_response_ready = true;
    lingxin_playback_release_input(generation);
    lingxin_playback_lifecycle_unlock();
    if (callback && generation == s_playback_generation)
        callback(Lingxin_Download_Audio_InitEnd, user_data);
}

/* Slow the next receive when TTS arrives faster than audible playback.
 * Caller must release the lifecycle lock first: wake/termination must remain
 * able to replace this response. Throttling adds at most 500ms per callback
 * before the shared websocket worker services transmit and control messages.
 * This is bounded burst smoothing, not permission to exceed the queue cap;
 * an oversized message or a persistently stopped consumer still fails. */
static void lingxin_stream_throttle_producer(uint32_t generation)
{
    uint32_t started = lingxin_playback_now_ms();
    for (;;) {
        taskENTER_CRITICAL();
        bool current = generation == s_playback_generation && s_stream_writer_running &&
                       s_stream_response_ready && !s_stream_write_failed && !s_stream_audio_ended;
        uint32_t queued = s_stream_queue_bytes;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        uint32_t owner = s_stream_owner_generation;
#endif
        taskEXIT_CRITICAL();
        if (!current || queued <= LINGXIN_STREAM_THROTTLE_HIGH_BYTES) return;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (!service_offline_action_online_current(owner)) return;
#endif
        uint32_t elapsed = lingxin_playback_now_ms() - started;
        if (elapsed >= LINGXIN_STREAM_THROTTLE_MAX_MS) return;
        uint32_t remaining = LINGXIN_STREAM_THROTTLE_MAX_MS - elapsed;
        lisa_thread_mdelay(remaining < LINGXIN_STREAM_THROTTLE_POLL_MS
                              ? remaining : LINGXIN_STREAM_THROTTLE_POLL_MS);
    }
}

void module_bufferPlay_data(void *buf, int rlen)
{
    uint32_t generation = s_playback_generation;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(s_stream_owner_generation)) return;
#endif
    if (!s_stream_response_ready || buf == NULL || rlen <= 0 || s_stream_audio_ended || s_stream_write_failed) return;
    if (!lingxin_playback_lifecycle_lock()) {lingxin_stream_fail(generation, "producer_busy"); return;}
    if (generation != s_playback_generation || !s_stream_response_ready) {
        lingxin_playback_lifecycle_unlock();
        return;
    }
    s_stream_chunk_count++;
    s_stream_bytes_total += (uint32_t)rlen;
    if (lingxin_tts_suppress_should_drop_audio(&s_tts_suppress_policy)) {
        lingxin_playback_lifecycle_unlock();
        return;
    }
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
    service_codex_voice_cache_chunk(generation, buf, (uint32_t)rlen);
#endif
    if (s_stream_chunk_count == 1U) lingxin_server_diag_timing_mark("buffer_play_first_chunk", rlen);
    bool ok = lingxin_stream_start_playback(generation) &&
              lingxin_stream_enqueue_copy(buf, (uint32_t)rlen, false, generation);
    lingxin_playback_lifecycle_unlock();
    if (!ok)
        lingxin_stream_fail(generation, "mp3_queue_limit_or_start");
    else
        lingxin_stream_throttle_producer(generation);
}

void module_bufferPlay_audioEnd(void)
{
    if (!s_stream_response_ready) return;
    uint32_t generation = s_playback_generation;
    uint32_t response_id = s_stream_response_id;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(s_stream_owner_generation))
        return; /* Offline owner has already cancelled this cloud response. */
#endif
    if (!lingxin_playback_lifecycle_lock()) {lingxin_stream_fail(generation, "end_busy"); return;}
    if (response_id != s_stream_response_id || !s_stream_response_ready ||
        s_stream_audio_ended || s_stream_write_failed) {
        lingxin_playback_lifecycle_unlock();
        return;
    }
    generation = s_playback_generation;
    s_stream_audio_ended = true;
    if (lingxin_tts_suppress_is_active(&s_tts_suppress_policy)) {
        if (!lingxin_stream_writer_stop()) {
            lingxin_playback_lifecycle_unlock();
            voice_chat_fail_epoch(s_stream_epoch); return;
        }
        lingxin_stream_pcm_prefill_reset();
        lingxin_playback_lifecycle_unlock();
        lingxin_post_playback_event_async(Lingxin_Download_Audio_PlayEnd, "suppressed_for_local_action", generation);
        return;
    }
    if (s_stream_chunk_count == 0U) {
        lingxin_playback_lifecycle_unlock();
        lingxin_post_playback_event_async(Lingxin_Download_Audio_PlayEnd, "empty_audio", generation);
        return;
    }
    bool ok = s_stream_mode_started && lingxin_stream_enqueue_copy(NULL, 0, true, generation);
    lingxin_playback_lifecycle_unlock();
    if (!ok)
        lingxin_stream_fail(generation, "mp3_end_enqueue");
}

static void lingxin_stream_terminate_thread(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    while (generation == s_playback_generation) {
        if (!lingxin_playback_lifecycle_lock()) {lisa_thread_mdelay(20); continue;}
        if (generation != s_playback_generation) {lingxin_playback_lifecycle_unlock(); return;}
        bool stopped = lingxin_stream_writer_stop();
        if (stopped && (s_stream_playing || s_stream_mode_started || s_player_cleanup_started))
            stopped = lingxin_compat_player_stop_sync(LINGXIN_PLAYBACK_PLAYER_TYPE);
        if (!stopped) {
            lingxin_playback_lifecycle_unlock();
            lisa_thread_mdelay(100);
            continue; /* Complete the pending termination when cleanup returns. */
        }
        if (generation != s_playback_generation) {lingxin_playback_lifecycle_unlock(); return;}
        s_stream_playing = false;
        s_stream_mode_started = false;
        s_stream_player_started = false;
        s_stream_audio_ended = true;
        lingxin_stream_pcm_prefill_reset();
        lingxin_playback_release_input(generation);
        lingxin_playback_lifecycle_unlock();
        voice_msg_pub_timeout(VOICE_MSG_PLAYER_TTS_STOPED, NULL, 0, 0);
        while (generation == s_playback_generation &&
               !lingxin_post_playback_event_async(Lingxin_Download_Audio_TerminateEnd, "terminate", generation))
            lisa_thread_mdelay(100);
        return;
    }
}

static void lingxin_termination_worker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t generation = __atomic_load_n(&s_pending_termination_generation, __ATOMIC_ACQUIRE);
        if (generation != 0U) {
            lingxin_stream_terminate_thread((void *)(uintptr_t)generation);
            (void)__atomic_compare_exchange_n(&s_pending_termination_generation, &generation, 0U,
                                              false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        } else {
            lisa_thread_mdelay(20);
        }
    }
}

/* Reserve the cleanup executor before allocating a response's audio buffers. */
static bool lingxin_ensure_termination_worker(void)
{
    if (s_termination_worker != NULL) return true;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lx.play.term", .stack_size = 4096,
        .priority = LISA_OS_PRIORITY_LOW,
    };
    s_termination_worker = lisa_thread_create(&attr, lingxin_termination_worker_thread, NULL);
    return s_termination_worker != NULL;
}

void module_bufferPlay_terminate(void)
{
    s_stream_response_ready = false;
    uint32_t generation = lingxin_playback_advance_generation();
    if (generation == 0U) return;
    s_stream_response_id = generation;
    s_stream_writer_running = false;
    __atomic_store_n(&s_pending_termination_generation, generation, __ATOMIC_RELEASE);
    if (s_termination_worker == NULL && lingxin_playback_lifecycle_lock()) {
        bool ready = lingxin_ensure_termination_worker();
        lingxin_playback_lifecycle_unlock();
        if (!ready) LOGE("[Lingxin] terminate executor allocation failed gen=%u", generation);
    }
}

void lingxin_audio_suppress_current_tts(const char *reason)
{
    if (lingxin_tts_suppress_is_active(&s_tts_suppress_policy)) {
        return;
    }
    uint32_t generation = lingxin_playback_advance_generation();
    if (generation == 0U) return;

    if (!lingxin_playback_lifecycle_lock()) {voice_chat_fail_epoch(s_stream_epoch); return;}
    if (generation != s_playback_generation) {lingxin_playback_lifecycle_unlock(); return;}

    LOGI("[Lingxin] suppress current cloud TTS: %s", reason ? reason : "local_action");
    lingxin_tts_suppress_request(&s_tts_suppress_policy);
    if (!lingxin_stream_writer_stop()) {
        lingxin_playback_lifecycle_unlock();
        voice_chat_fail_epoch(s_stream_epoch); return;
    }
    if (s_stream_playing || s_stream_mode_started) {
        s_stream_suppress_stop_event = true;
        if (!lingxin_compat_player_stop_sync(LINGXIN_PLAYBACK_PLAYER_TYPE)) {
            lingxin_playback_lifecycle_unlock();
            voice_chat_fail_epoch(s_stream_epoch);
            return;
        }
    }
    if (generation != s_playback_generation) {lingxin_playback_lifecycle_unlock(); return;}
    s_stream_playing = false;
    s_stream_mode_started = false;
    s_stream_player_started = false;
    s_stream_write_failed = false;
    lingxin_stream_pcm_prefill_reset();
    lingxin_playback_release_input(generation);
    bool response_ended = s_stream_audio_ended;
    lingxin_playback_lifecycle_unlock();
    voice_msg_pub_timeout(VOICE_MSG_PLAYER_TTS_STOPED, NULL, 0, 0);
    /* EOF can precede execution of a local music/dance action. Its old
     * COMPLETE was invalidated above, so finish the suppressed generation
     * here rather than waiting for an EOF that has already arrived. */
    if (response_ended)
        lingxin_post_playback_event_async(Lingxin_Download_Audio_PlayEnd,
                                          "suppressed_for_local_action", generation);
}

void module_bufferPlay_setVolume(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    lingxin_compat_player_volume(PLAYER_T_CLOUD, (uint8_t)volume);
    lingxin_compat_player_volume(LINGXIN_PLAYBACK_PLAYER_TYPE, (uint8_t)volume);
    LOGI("[Lingxin] stream set volume=%d", volume);
}

void lingxin_system_abort()
{
    LOGE("Lingxin requested system abort");
}
