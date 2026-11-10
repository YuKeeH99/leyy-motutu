#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "power_manager.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "queue.h"

#define TAG "voice_player"
#include "lisa_audio.h"
#include "lisa_device.h"
#include "lisa_log.h"
#include "lisa_time.h"
#include "sysutils.h"
#include "lisa_http.h"
#include "sysheap.h"

#include "alarm_ring.h"

#include "sys_init.h"
#include "voice_msg.h"
#include "app_datas.h"
#include "sys_network_diag.h"
#include "sys_network_manager.h"
#include "sys_network_probe.h"
#include "sys_wifi.h"
#include "voice_cloud.h"
#ifdef CONFIG_VOICE_LINK_LINGXIN
#include "lingxin_server.h"
#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
#include "lingxin_protocol_manager.h"
#endif
#endif
#include "tone.h"
#include "app_tone.h"
#include "app_player.h"
#include "voice_player_comm.h"
#include "voice_player.h"
#include "voice_music_list.h"
#include "voice_intent_mgr.h"
#include "voice_intent/voice_intent_music.h"
#include "voice_intent/voice_intent_photo_flow.h"
#include "services/voice_car_control.h"
#include "services/voice_motutu_control.h"
#include "services/service_v2_voice_standby.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "services/service_v2_low_battery.h"
#include "services/service_v2_rgb_effect.h"
#endif
#include "service_sd_music.h"
#include "service_button.h"
#define ALARM_TTS_BUF_SIZE 512

/* 异步播放队列相关定义 */
#define ASYNC_PLAY_URL_MAX 256
#define ASYNC_PLAY_QUEUE_SIZE 5
#define ASYNC_PLAY_TASK_STACK_SIZE 2048
#define ASYNC_PLAY_TASK_PRIORITY 5
#define PUSHUP_TTS_READY_POLL_MS 1000
#define PUSHUP_TTS_READY_STABLE_COUNT 2
#define PUSHUP_TTS_READY_MAX_ATTEMPTS 21
#define PUSHUP_TTS_HTTP_TIMEOUT_MS 15000
#define PUSHUP_TTS_DOWNLOAD_MAX_BYTES (3U * 1024U * 1024U)
#define CLOUD_OPEN_INFO_STATUS_BIND 3U

/* 提示音排队播放：短时间内多次请求 tone 时，按顺序逐个播完，避免打架 */
#define TONE_PENDING_QUEUE_SIZE 5
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
/* AEC handles speaker echo; a fixed mute after DAC drain clips the first word. */
#define WAKEUP_UPLINK_TAIL_GUARD_MS 0U
#else
#define WAKEUP_UPLINK_TAIL_GUARD_MS 200U
#endif
#define LOCAL_COMMAND_TAIL_GUARD_MS 200U
#define WAKEUP_UPLINK_GATE_FAILSAFE_MS 5000U

typedef struct {
    uint8_t type;
    uint8_t bypass_tts_gate;
    uint8_t wait_mp3_ready;
    uint8_t reserved;
    app_player_t *player;  /* 播放器实例指针 */
    char url[ASYNC_PLAY_URL_MAX];
    uint32_t owner_generation;
} async_play_request_t;

typedef struct {
    const char *url;
    bool prompt_tone;
    uint32_t wakeup_gate_generation;
    uint32_t offline_generation;
    uint32_t local_generation;
    uint16_t tokens[32];
    unsigned token_count, token_index;
} tone_play_request_t;

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
#include "service_offline_action_policy.h"
#include "service_local_version.h"
#include "project_version.h"
static uint32_t s_current_offline_generation;
static tone_play_request_t s_current_local_request;
#endif

typedef enum {
    ASYNC_PLAY_REQUEST_URL = 0,
    ASYNC_PLAY_REQUEST_RESUME,
} async_play_request_type_t;

static QueueHandle_t s_async_play_queue = NULL;
static TaskHandle_t s_async_play_task = NULL;

static bool s_disconnect_tone_played = false;
static bool s_cloud_unstable_pending = false;
static TickType_t s_cloud_unstable_pending_tick = 0;
static bool s_cloud_reconnect_pending = false;
static bool s_cloud_success_tone_played = false;
static bool s_suppress_next_cloud_success_tone = false;
static TimerHandle_t s_cloud_success_tone_timer = NULL;
static TickType_t s_boot_tick = 0;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static bool s_rgb_boot_tone;

static void voice_player_boot_rgb_event(app_player_event_t event)
{
    if (!s_rgb_boot_tone) return;
    if (event == APP_PLAYER_EVENT_PLAYING) {
        service_v2_rgb_effect_show_start(SERVICE_V2_RGB_SHOW_BOOT);
    } else if (event == APP_PLAYER_EVENT_COMPLETED || event == APP_PLAYER_EVENT_STOPPED ||
               event == APP_PLAYER_EVENT_ERROR || event == APP_PLAYER_EVENT_PAUSED) {
        s_rgb_boot_tone = false;
        service_v2_rgb_effect_show_stop(SERVICE_V2_RGB_SHOW_BOOT);
    }
}
#endif
static char s_current_tts_url[ASYNC_PLAY_URL_MAX] = {0};
/* TTS 活跃标志：在调用 async_play_url 前置 true，事件回调收到 stop/error/complete
 * 时清零。读取无需获取播放器的 operation_lock，避免 ebus 回调在 prepare 阶段被
 * 长时间阻塞（async_play 线程跑 HTTP recv 时持锁可能达数秒）。 */
static volatile bool s_tts_active = false;

/* 提示音排队播放状态 */
static tone_play_request_t s_tone_pending_queue[TONE_PENDING_QUEUE_SIZE];
static uint8_t s_tone_pending_head = 0;
static uint8_t s_tone_pending_count = 0;
static bool s_tone_playing = false;
static const char *s_current_tone_url = NULL;
static uint32_t s_beat_music_sequence, s_beat_music_ticket, s_beat_music_owner;
static int s_beat_music_result;
static bool s_current_tone_prompt = false;
static uint32_t s_current_tone_wakeup_generation = 0U;
static uint32_t s_latest_wakeup_gate_generation = 0U;
static uint32_t s_wakeup_gate_timer_generation = 0U;
static TickType_t s_wakeup_gate_not_before_tick = 0U;
static TimerHandle_t s_wakeup_gate_timer = NULL;
static bool s_boot_hello_played = false;

/* Only active during an explicit power-off request; ordinary conversation
 * keeps the pre-merge queue, callbacks and intent transitions below. */
static volatile bool s_shutdown_prompt_in_progress = false;
static volatile uint32_t s_audio_message_inflight;
static volatile uint32_t s_offline_audio_cancel_generation;
static volatile bool s_shutdown_tone_playing = false;
static volatile int s_shutdown_tone_result = 0;
static volatile bool s_local_tone_active, s_local_tone_tail_pending;
static volatile uint32_t s_upload_cue_epoch;
static volatile uint32_t s_upload_cue_cancelled_epoch;
static uint32_t s_upload_cue_notified_epoch;
static uint32_t s_upload_cue_playing_epoch;
static bool s_upload_cue_playback_started;
static voice_upload_cue_stats_t s_upload_cue_stats;
static const char s_upload_cue_tag[] = "upload_complete";
typedef struct { uint32_t round_epoch, owner_generation; } upload_cue_request_t;
static void voice_player_start_upload_cue(void *data, uint32_t len);
static TickType_t s_local_tone_tail_until;
extern void boot_watchdog_feed(void);
static void voice_player_drain_wakeup_output(void);

static void voice_player_local_tone_gate_begin(void)
{
    taskENTER_CRITICAL();
    s_local_tone_active = true;
    s_local_tone_tail_pending = false;
    taskEXIT_CRITICAL();
}

static void voice_player_local_tone_gate_end(void)
{
    taskENTER_CRITICAL();
    if (s_local_tone_active) {
        s_local_tone_tail_until = xTaskGetTickCount() + pdMS_TO_TICKS(LOCAL_COMMAND_TAIL_GUARD_MS);
        s_local_tone_tail_pending = true;
        s_local_tone_active = false;
    }
    taskEXIT_CRITICAL();
}

bool voice_player_local_tone_blocks_uplink(void)
{
    taskENTER_CRITICAL();
    if (s_local_tone_tail_pending &&
        (int32_t)(xTaskGetTickCount() - s_local_tone_tail_until) >= 0) {
        s_local_tone_tail_pending = false;
    }
    bool blocked = s_local_tone_active;
#ifndef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    blocked = blocked || s_local_tone_tail_pending;
#endif
    taskEXIT_CRITICAL();
    /* Never take the player's operation lock in the recording path. Queued
     * callbacks also hold the gate until a pending PLAYING can reassert it. */
    return blocked || __atomic_load_n(&s_beat_music_owner, __ATOMIC_ACQUIRE) != 0U ||
           (tone_player && !app_player_callbacks_idle(tone_player)) ||
           (beat_player && !app_player_callbacks_idle(beat_player));
}

bool voice_player_mode_command_blocked(void)
{
    /* AP ingress holds a critical section: do not take any player mutex.
     * s_tone_playing also covers queued callbacks after an older STOPPED. */
    taskENTER_CRITICAL();
    bool blocked = s_tts_active || s_tone_playing || s_local_tone_active ||
                   __atomic_load_n(&s_beat_music_owner, __ATOMIC_ACQUIRE) != 0U ||
                   (s_local_tone_tail_pending &&
                    (int32_t)(xTaskGetTickCount() - s_local_tone_tail_until) < 0);
    taskEXIT_CRITICAL();
    return blocked;
}

bool voice_player_offline_audio_cancel_ready(uint32_t generation)
{
    /* Called outside ingress critical sections. The bus cancellation can be
     * queued, and stop() can leave STOPPED callbacks behind after returning. */
    return s_offline_audio_cancel_generation == generation &&
           __atomic_load_n(&s_beat_music_owner, __ATOMIC_ACQUIRE) == 0U &&
           (!beat_player || app_player_callbacks_idle(beat_player)) &&
           (!tone_player || app_player_callbacks_idle(tone_player)) &&
           (!tts_player || app_player_callbacks_idle(tts_player));
}

static bool voice_player_local_tone_event(app_player_event_t event)
{
    bool output_drained = false;
    if (event == APP_PLAYER_EVENT_PLAYING) {
        voice_player_local_tone_gate_begin();
    } else if (s_local_tone_active && (event == APP_PLAYER_EVENT_COMPLETED ||
               event == APP_PLAYER_EVENT_STOPPED || event == APP_PLAYER_EVENT_ERROR)) {
        if (event == APP_PLAYER_EVENT_COMPLETED) {
            voice_player_drain_wakeup_output();
            output_drained = true;
        }
        voice_player_local_tone_gate_end();
    }
    return output_drained;
}

static void voice_player_tts_track_url(const char *url);
static void voice_player_flush_tone_queue(void);
static void voice_player_schedule_wakeup_gate_release(uint32_t generation,
                                                      uint32_t delay_ms,
                                                      const char *reason);

static void voice_player_drain_wakeup_output(void)
{
    lisa_device_t *audio_dev = lisa_device_get("audio0");
    uint64_t started_ms = lisa_os_get_tick_ms();

    if (audio_dev == NULL || !lisa_device_ready(audio_dev)) {
        LOGW("cannot drain wakeup output: audio0 is not ready");
        return;
    }

    int ret = lisa_audio_play_flush(audio_dev);
    uint32_t elapsed_ms = (uint32_t)(lisa_os_get_tick_ms() - started_ms);
    if (ret == LISA_DEVICE_OK) {
        LOGI("wakeup output drained in %ums", (unsigned)elapsed_ms);
    } else {
        LOGW("wakeup output drain failed: ret=%d elapsed=%ums", ret, (unsigned)elapsed_ms);
    }
}

#define CLOUD_SUCCESS_TONE_DELAY_MS 2000
#define CLOUD_UNSTABLE_TONE_GRACE_MS 800

typedef struct {
    uint8_t *buf;
    size_t len;
    bool overflow;
    uint32_t owner_generation;
} pushup_tts_download_ctx_t;

static bool voice_player_url_is_mp3(const char *url)
{
    const char *query;
    size_t len;

    if (url == NULL) {
        return false;
    }

    query = strchr(url, '?');
    len = query ? (size_t)(query - url) : strlen(url);

    return len >= 4 &&
           url[len - 4] == '.' &&
           url[len - 3] == 'm' &&
           url[len - 2] == 'p' &&
           url[len - 1] == '3';
}

static bool voice_player_tts_url_is_latest(const char *url)
{
    bool latest;

    if (url == NULL) {
        return false;
    }

    taskENTER_CRITICAL();
    latest = strncmp(s_current_tts_url, url, sizeof(s_current_tts_url)) == 0;
    taskEXIT_CRITICAL();

    return latest;
}

static void pushup_tts_http_ignore_data(lisa_http_data_t *data)
{
    (void)data;
}

static void *pushup_tts_http_headers(void)
{
    return (void *)"Accept: audio/mpeg";
}

static int pushup_tts_download_on_chunk(lisa_http_data_t *data)
{
    pushup_tts_download_ctx_t *ctx;
    size_t need;

    if (data == NULL || data->user == NULL || data->len <= 0) {
        return 0;
    }

    ctx = (pushup_tts_download_ctx_t *)data->user;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(ctx->owner_generation)) return -1;
#endif
    need = ctx->len + (size_t)data->len;
    if (need > PUSHUP_TTS_DOWNLOAD_MAX_BYTES) {
        ctx->overflow = true;
        return -1;
    }

    memcpy(ctx->buf + ctx->len, data->buf, (size_t)data->len);
    ctx->len = need;

    return 0;
}

static int voice_player_download_pushup_tts_mp3(const char *url, uint8_t *buf, size_t *out_len,
                                               uint32_t owner_generation)
{
    pushup_tts_download_ctx_t ctx = {
        .buf = buf,
        .owner_generation = owner_generation,
    };
    lisa_http_request_t req = {
        .method = LISA_HTTP_GET,
        .url = (uint8_t *)url,
        .headers = (uint8_t *)pushup_tts_http_headers,
        .timeout = PUSHUP_TTS_HTTP_TIMEOUT_MS,
        .user = &ctx,
        .on_data = pushup_tts_http_ignore_data,
    };
    lisa_http_t *http;
    lisa_http_err_e ret;

    if (url == NULL || buf == NULL || out_len == NULL) {
        return -1;
    }

    *out_len = 0;

    http = lisa_http_init(&req);
    if (http == NULL) {
        LOGE("pushup tts mp3 http init failed");
        return -1;
    }

    ret = lisa_http_perform_chunked_with_cb(http, pushup_tts_download_on_chunk);
    lisa_http_cleanup(http);

    if (ret != LISA_HTTP_OK || ctx.overflow || ctx.len == 0) {
        LOGW("pushup tts mp3 download failed, ret=%d, len=%u, overflow=%d",
             ret,
             (unsigned int)ctx.len,
             ctx.overflow ? 1 : 0);
        return -1;
    }

    *out_len = ctx.len;
    return 0;
}

static int voice_player_wait_pushup_tts_mp3_ready(const char *url, uint32_t owner_generation)
{
    uint8_t *buf = NULL;
    size_t latest_len = 0;
    bool has_download = false;
    uint8_t stable_count = 0;

    buf = (uint8_t *)psram_malloc(PUSHUP_TTS_DOWNLOAD_MAX_BYTES);
    if (buf == NULL) {
        LOGE("pushup tts mp3 psram malloc failed, size=%u",
             (unsigned int)PUSHUP_TTS_DOWNLOAD_MAX_BYTES);
        return -1;
    }

    for (uint8_t attempt = 1; attempt <= PUSHUP_TTS_READY_MAX_ATTEMPTS; attempt++) {
        size_t len = 0;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (!service_offline_action_online_current(owner_generation)) {
            psram_free(buf);
            return 1;
        }
#endif

        if (!voice_player_tts_url_is_latest(url)) {
            LOGD("drop stale pushup tts mp3 url: %s", url);
            psram_free(buf);
            return 1;
        }

        if (voice_player_download_pushup_tts_mp3(url, buf, &len, owner_generation) != 0) {
            LOGW("pushup tts mp3 ready attempt %u/%u failed",
                 (unsigned int)attempt,
                 (unsigned int)PUSHUP_TTS_READY_MAX_ATTEMPTS);
            psram_free(buf);
            return -1;
        }

        if (has_download && len == latest_len) {
            stable_count++;
        } else {
            stable_count = 1;
        }

        latest_len = len;
        has_download = true;

        LOGI("pushup tts mp3 ready attempt %u/%u, size=%u, stable=%u/%u",
             (unsigned int)attempt,
             (unsigned int)PUSHUP_TTS_READY_MAX_ATTEMPTS,
             (unsigned int)latest_len,
             (unsigned int)stable_count,
             (unsigned int)PUSHUP_TTS_READY_STABLE_COUNT);

        if (stable_count >= PUSHUP_TTS_READY_STABLE_COUNT) {
            break;
        }

        if (attempt < PUSHUP_TTS_READY_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(PUSHUP_TTS_READY_POLL_MS));
        }
    }

    if (!has_download) {
        psram_free(buf);
        return -1;
    }

    if (!voice_player_tts_url_is_latest(url)) {
        LOGD("drop stale pushup tts mp3 url before play: %s", url);
        psram_free(buf);
        return 1;
    }

    if (stable_count < PUSHUP_TTS_READY_STABLE_COUNT) {
        LOGW("pushup tts mp3 still growing after %u ms, fallback direct url, latest size=%u",
             (unsigned int)((PUSHUP_TTS_READY_MAX_ATTEMPTS - 1U) * PUSHUP_TTS_READY_POLL_MS),
             (unsigned int)latest_len);
        psram_free(buf);
        return -1;
    }

    LOGI("pushup tts mp3 ready, size=%u, play direct url", (unsigned int)latest_len);
    psram_free(buf);
    return 0;
}

/* 异步播放任务 */
static void async_play_task(void *pvParameters)
{
    async_play_request_t request;

    while (1) {
        /* 从队列中获取播放请求，阻塞等待 */
        if (xQueueReceive(s_async_play_queue, &request, portMAX_DELAY) == pdTRUE) {
            LOGI("async play task received request, type=%u, player=%p, wait_mp3=%u, url=%s",
                 (unsigned int)request.type,
                 request.player,
                 (unsigned int)request.wait_mp3_ready,
                 request.url);

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
            if (request.player == tts_player && !service_offline_action_online_current(request.owner_generation)) {
                s_tts_active = false;
                continue;
            }
#endif
            if (request.type == ASYNC_PLAY_REQUEST_RESUME) {
                app_player_resume(request.player);
                continue;
            }

            if (request.player == tts_player &&
                !request.bypass_tts_gate &&
                voice_intent_photo_flow_should_gate_tts()) {
                continue;
            }

            if (request.player == tts_player &&
                request.wait_mp3_ready &&
                voice_player_url_is_mp3(request.url)) {
                int ready_ret = voice_player_wait_pushup_tts_mp3_ready(request.url, request.owner_generation);
                if (ready_ret > 0) {
                    continue;
                }
                if (ready_ret < 0) {
                    LOGW("pushup tts mp3 ready workaround failed, fallback to direct url");
                }
            }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
            if (request.player == tts_player && !service_offline_action_online_current(request.owner_generation)) {
                s_tts_active = false;
                continue;
            }
#endif
            /* 在独立任务中执行播放操作，不会阻塞ebus线程 */
            app_player_play(request.player, request.url);
        }
    }
}

/* 初始化异步播放队列和任务 */
static int async_play_init(void)
{
    if (s_async_play_queue != NULL) {
        return 0; /* 已经初始化 */
    }

    /* 创建播放请求队列 */
    s_async_play_queue = xQueueCreate(ASYNC_PLAY_QUEUE_SIZE, sizeof(async_play_request_t));
    if (s_async_play_queue == NULL) {
        LOGE("failed to create async play queue");
        return -1;
    }

    /* 创建异步播放任务 */
    BaseType_t ret = xTaskCreate(
        async_play_task,
        "async_play",
        ASYNC_PLAY_TASK_STACK_SIZE,
        NULL,
        ASYNC_PLAY_TASK_PRIORITY,
        &s_async_play_task
    );

    if (ret != pdPASS) {
        LOGE("failed to create async play task");
        vQueueDelete(s_async_play_queue);
        s_async_play_queue = NULL;
        return -1;
    }

    LOGI("async play task created successfully");
    return 0;
}

/* 异步播放接口，发送播放请求到队列 */
static void async_play_url_internal(app_player_t *player, const char *url, bool bypass_tts_gate)
{
    if (s_async_play_queue == NULL || url == NULL) {
        LOGE("async play queue not ready or url is null");
        return;
    }

    async_play_request_t request = {0};
    request.type = ASYNC_PLAY_REQUEST_URL;
    request.bypass_tts_gate = bypass_tts_gate ? 1U : 0U;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    request.owner_generation = service_offline_action_generation();
    if (player == tts_player && !service_offline_action_online_current(request.owner_generation)) return;
#endif
    request.player = player;
    strncpy(request.url, url, sizeof(request.url) - 1);
    request.url[sizeof(request.url) - 1] = '\0';

    if (player == tts_player) {
        s_tts_active = true;
    }

    /* 发送到队列，最多等待100ms */
    if (xQueueSend(s_async_play_queue, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOGE("failed to send play request to queue");
    } else {
        LOGI("play request sent to async queue, player=%p", player);
    }
}

static void async_play_url(app_player_t *player, const char *url)
{
    async_play_url_internal(player, url, false);
}

static void async_play_tts_mp3_ready_url(const char *url)
{
    if (s_async_play_queue == NULL || url == NULL) {
        LOGE("async play queue not ready or url is null");
        return;
    }

    async_play_request_t request = {0};
    request.type = ASYNC_PLAY_REQUEST_URL;
    request.wait_mp3_ready = 1U;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    request.owner_generation = service_offline_action_generation();
    if (!service_offline_action_online_current(request.owner_generation)) return;
#endif
    request.player = tts_player;
    strncpy(request.url, url, sizeof(request.url) - 1);
    request.url[sizeof(request.url) - 1] = '\0';

    s_tts_active = true;

    if (xQueueSend(s_async_play_queue, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOGE("failed to send tts mp3 ready request to queue");
    } else {
        LOGI("tts mp3 ready request sent to async queue");
    }
}

static void voice_player_queue_pushup_tts_ready_check(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        LOGW("pushup tts ready check skipped: empty url");
        return;
    }

    LOGD("queue pushup tts ready check every %u ms: %s",
         (unsigned int)PUSHUP_TTS_READY_POLL_MS,
         url);
    async_play_tts_mp3_ready_url(url);
}

static void async_resume_player(app_player_t *player)
{
    if (s_async_play_queue == NULL || player == NULL) {
        LOGE("async play queue not ready or player is null");
        return;
    }

    async_play_request_t request = {0};
    request.type = ASYNC_PLAY_REQUEST_RESUME;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    request.owner_generation = service_offline_action_generation();
    if (player == tts_player && !service_offline_action_online_current(request.owner_generation)) return;
#endif
    request.player = player;

    if (player == tts_player) {
        s_tts_active = true;
    }

    if (xQueueSend(s_async_play_queue, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
        LOGE("failed to send resume request to queue");
    } else {
        LOGI("resume request sent to async queue, player=%p", player);
    }
}

static void voice_player_tts_track_url(const char *url)
{
    if (!url || url[0] == '\0') {
        return;
    }

    taskENTER_CRITICAL();
    strncpy(s_current_tts_url, url, sizeof(s_current_tts_url) - 1);
    s_current_tts_url[sizeof(s_current_tts_url) - 1] = '\0';
    taskEXIT_CRITICAL();
    LOGI("refresh latest tts url: %s", s_current_tts_url);
}

void voice_player_replay_tts_url(const char *url)
{
    if (!url || url[0] == '\0') {
        LOGW("replay tts url: empty url");
        return;
    }
    LOGI("replay tts url: %s", url);
    async_play_url_internal(tts_player, url, true);
}

void voice_player_play_tts_url_async(const char *url)
{
    if (!url || url[0] == '\0') {
        LOGW("play tts url async: empty url");
        return;
    }
    voice_player_tts_track_url(url);
    async_play_url_internal(tts_player, url, true);
}

bool voice_player_latest_tts_url_copy(char *url_buf, size_t buf_len)
{
    bool has_url = false;

    taskENTER_CRITICAL();
    has_url = s_current_tts_url[0] != '\0';
    if (has_url && url_buf != NULL && buf_len > 0) {
        strncpy(url_buf, s_current_tts_url, buf_len - 1);
        url_buf[buf_len - 1] = '\0';
    }
    taskEXIT_CRITICAL();

    return has_url;
}

bool voice_player_tts_is_active(void)
{
    return s_tts_active;
}

/* 闹钟播放阶段管理 */
typedef enum {
    ALARM_PLAY_PHASE_TONE_FIRST = 0,  // 首次播放默认提示音
    ALARM_PLAY_PHASE_TTS,              // 播放云端TTS
    ALARM_PLAY_PHASE_TONE_LOOP,        // 循环播放默认提示音
} alarm_play_phase_t;

static struct {
    alarm_play_phase_t phase;
    char text[ALARM_TTS_BUF_SIZE];
} s_alarm_play_ctx __psram_bss__;

static uint32_t xorshift32(void)
{
    static uint32_t state = 0;
    if (state == 0) {
        state = lisa_rand32() | 1;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static char *voice_player_get_wakeup_tone_url(void)
{
    static bool s_wakeup_tone_inited = false;
    static uint16_t s_wakeup_tone_ids[5] = {0};
    static uint8_t s_wakeup_tone_cnt = 0;
    static bool s_last_wakeup_tone_valid = false;
    static uint16_t s_last_wakeup_tone_id = TONE_ID_0;


    if (!s_wakeup_tone_inited) {
        for (uint16_t tone_id = TONE_ID_0; tone_id <= TONE_ID_4; tone_id++) {
            if (app_tone_get_url(tone_id) != NULL) {
                s_wakeup_tone_ids[s_wakeup_tone_cnt++] = tone_id;
            }
        }

        if (s_wakeup_tone_cnt == 0) {
            s_wakeup_tone_ids[s_wakeup_tone_cnt++] = TONE_ID_0;
        }

        s_wakeup_tone_inited = true;
        LOGI("wakeup tone candidates count: %u", (unsigned int)s_wakeup_tone_cnt);
    }

    uint16_t tone_id = s_wakeup_tone_ids[0];
    if (s_wakeup_tone_cnt > 1) {
        uint8_t attempts = 0;
        do {
            tone_id = s_wakeup_tone_ids[xorshift32() % s_wakeup_tone_cnt];
        } while (s_last_wakeup_tone_valid && tone_id == s_last_wakeup_tone_id && ++attempts < 8);
    }

    LOGI("Selected wakeup tone id: %u", tone_id);

    char *tone_url = app_tone_get_url(tone_id);
    if (tone_url == NULL) {
        tone_id = TONE_ID_0;
        tone_url = app_tone_get_url(tone_id);
    }

    if (tone_url != NULL) {
        s_last_wakeup_tone_id = tone_id;
        s_last_wakeup_tone_valid = true;
    }

    return tone_url;
}

static int voice_player_prompt_tone_enter(void)
{
    if (voice_intent_contains(INTENT_PROMPT_TONE)) {
        return 0;
    }

    return voice_intent_push(INTENT_PROMPT_TONE);
}

static void voice_player_prompt_tone_exit(void)
{
    if (voice_intent_contains(INTENT_PROMPT_TONE)) {
        voice_intent_pop(INTENT_PROMPT_TONE);
    }
}

static bool voice_player_network_tone_allowed(const char *url)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (url && !service_offline_action_network_prompt_allowed()) {
        /* Explicit provisioning acknowledgement (70) is user feedback, not a background reminder. */
        static const uint16_t network_ids[] = {59,60,64,65,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,105,111};
        for (unsigned i = 0; i < sizeof(network_ids)/sizeof(network_ids[0]); ++i) {
            const char *network_url = app_tone_get_url(network_ids[i]);
            if (network_url && strcmp(url, network_url) == 0) return false;
        }
    }
#endif
    return true;
}

static bool voice_player_start_tone_request(const tone_play_request_t *request)
{
    bool entered_prompt = false;
    tone_play_request_t local;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (request && request->local_generation) {
        if (!service_offline_action_local_audio_current(request->local_generation) ||
            request->token_index >= request->token_count) return false;
        local = *request;
        local.url = app_tone_get_url(local.tokens[local.token_index]);
        request = &local;
        if (!local.url) LOGE("local voice resource missing id=%u generation=%u",
                            local.tokens[local.token_index], (unsigned)local.local_generation);
    }
#endif
    if (request && !voice_player_network_tone_allowed(request->url)) return false;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (request && request->offline_generation &&
        !service_offline_action_prompt_current(request->offline_generation)) return false;
#endif

    if (request == NULL || request->url == NULL) {
        LOGW("tone request is null, skip");
        return false;
    }

    if (request->prompt_tone && !voice_intent_contains(INTENT_PROMPT_TONE)) {
        if (voice_player_prompt_tone_enter() == 0) {
            entered_prompt = true;
        } else {
            LOGW("failed to enter PROMPT_TONE intent, play without music resume focus");
        }
    }

    voice_player_local_tone_gate_begin();
    s_tone_playing = true;
    s_current_tone_url = request->url;
    s_current_tone_prompt = request->prompt_tone && voice_intent_contains(INTENT_PROMPT_TONE);
    s_current_tone_wakeup_generation = request->wakeup_gate_generation;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    s_current_offline_generation = request->offline_generation;
    s_current_local_request = *request;
#endif

    if (tone_player == NULL) {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_offline_action_prompt_finished(s_current_offline_generation, false);
        s_current_offline_generation = 0;
#endif
        LOGW("failed to play tone url: tone_player is null, url=%s", request->url);
        voice_player_local_tone_gate_end();
        s_tone_playing = false;
        s_current_tone_url = NULL;
        s_current_tone_prompt = false;
        s_current_tone_wakeup_generation = 0U;
        if (entered_prompt) {
            voice_player_prompt_tone_exit();
        }
        return false;
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    const char *boot_url = app_tone_get_url(TONE_ID_106);
    s_rgb_boot_tone = boot_url != NULL && strcmp(request->url, boot_url) == 0;
#endif
    int play_ret = app_player_play(tone_player, request->url);
    if (play_ret != APP_PLAYER_OK) {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_offline_action_prompt_finished(s_current_offline_generation, false);
        s_current_offline_generation = 0;
        voice_player_boot_rgb_event(APP_PLAYER_EVENT_ERROR);
#endif
        LOGW("failed to play tone url: ret=%d, url=%s", play_ret, request->url);
        voice_player_local_tone_gate_end();
        s_tone_playing = false;
        s_current_tone_url = NULL;
        s_current_tone_prompt = false;
        s_current_tone_wakeup_generation = 0U;
        if (entered_prompt) {
            voice_player_prompt_tone_exit();
        }
        return false;
    }

    return true;
}

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static bool voice_player_advance_local_sequence(void)
{
    tone_play_request_t next = s_current_local_request;
    s_current_local_request.local_generation = 0;
    if (!next.local_generation) return false;
    if (!service_offline_action_local_audio_current(next.local_generation)) {
        service_offline_action_local_audio_finished(next.local_generation, false);
        return false;
    }
    if (++next.token_index < next.token_count) {
        if (voice_player_start_tone_request(&next)) return true;
        service_offline_action_local_audio_finished(next.local_generation, false);
        s_current_local_request.local_generation = 0;
        return false;
    }
    service_offline_action_local_audio_finished(next.local_generation, true);
    return false;
}
#endif

static void voice_player_start_next_queued_tone(void)
{
    while (s_tone_pending_count > 0) {
        tone_play_request_t request = s_tone_pending_queue[s_tone_pending_head];
        s_tone_pending_head = (s_tone_pending_head + 1) % TONE_PENDING_QUEUE_SIZE;
        s_tone_pending_count--;

        if (voice_player_start_tone_request(&request)) {
            return;
        }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_offline_action_local_audio_finished(request.local_generation, false);
#endif
    }

    s_tone_playing = false;
    s_current_tone_url = NULL;
    s_current_tone_prompt = false;
    s_current_tone_wakeup_generation = 0U;
}

static bool voice_player_play_tone_url_internal(const char *url, bool prompt_tone,
                                                uint32_t wakeup_gate_generation)
{
    if (!voice_player_network_tone_allowed(url)) return false;
    if (s_shutdown_prompt_in_progress) {
        return false;
    }
    if (url == NULL) {
        LOGW("tone url is null, skip");
        return false;
    }

    if (s_tone_playing) {
        if (s_tone_pending_count < TONE_PENDING_QUEUE_SIZE) {
            s_tone_pending_queue[(s_tone_pending_head + s_tone_pending_count) % TONE_PENDING_QUEUE_SIZE] =
                (tone_play_request_t){
                    .url = url,
                    .prompt_tone = prompt_tone,
                    .wakeup_gate_generation = wakeup_gate_generation,
                };
            s_tone_pending_count++;
            LOGD("tone queued, pending: %u", (unsigned int)s_tone_pending_count);
            return true;
        } else {
            LOGW("tone queue full, drop");
            return false;
        }
    }

    return voice_player_start_tone_request(&(tone_play_request_t){
        .url = url,
        .prompt_tone = prompt_tone,
        .wakeup_gate_generation = wakeup_gate_generation,
    });
}

static bool voice_player_tone_url_is_active_or_queued(const char *url)
{
    if (url == NULL) {
        return false;
    }

    if (s_tone_playing && s_current_tone_url != NULL && strcmp(s_current_tone_url, url) == 0) {
        return true;
    }

    for (uint8_t i = 0; i < s_tone_pending_count; i++) {
        uint8_t index = (s_tone_pending_head + i) % TONE_PENDING_QUEUE_SIZE;
        const char *queued_url = s_tone_pending_queue[index].url;
        if (queued_url != NULL && strcmp(queued_url, url) == 0) {
            return true;
        }
    }
    return false;
}

static bool voice_player_queue_priority_tone(const char *url)
{
    if (!s_tone_playing) {
        return voice_player_play_tone_url_internal(url, false, 0U);
    }

    if (s_tone_pending_count == TONE_PENDING_QUEUE_SIZE) {
        uint8_t dropped_index =
            (s_tone_pending_head + s_tone_pending_count - 1U) % TONE_PENDING_QUEUE_SIZE;
        uint32_t dropped_wakeup_generation =
            s_tone_pending_queue[dropped_index].wakeup_gate_generation;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_offline_action_prompt_finished(s_tone_pending_queue[dropped_index].offline_generation, false);
#endif

        /* Reserve room by dropping the least urgent (last) pending tone. */
        s_tone_pending_count--;
        LOGW("tone queue full, replace last pending tone with network reminder");
        if (dropped_wakeup_generation != 0U) {
            voice_player_schedule_wakeup_gate_release(dropped_wakeup_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "wakeup_tone_priority_replaced");
        }
    }

    s_tone_pending_head =
        (s_tone_pending_head + TONE_PENDING_QUEUE_SIZE - 1U) % TONE_PENDING_QUEUE_SIZE;
    s_tone_pending_queue[s_tone_pending_head] = (tone_play_request_t){
        .url = url,
        .prompt_tone = false,
        .wakeup_gate_generation = 0U,
    };
    s_tone_pending_count++;
    return true;
}

/* voice.ebus and the tone completion callback share this reminder budget.
 * Reserve under the scheduler lock, but never call the player while locked. */
#define NETWORK_REMINDER_INTERVAL_MS 15000U
static bool s_network_reminder_started;
static bool s_network_reminder_pending;
static uint32_t s_network_reminder_ms;

static bool voice_player_play_network_tone(uint32_t tone_id)
{
    const char *url = app_tone_get_url(tone_id);
    if (!voice_player_network_tone_allowed(url)) return false;
    bool was_playing;

    if (voice_player_tone_url_is_active_or_queued(url)) {
        sys_network_diag_mark("prompt", "network_unavailable_tone_already_pending",
                              tone_id, "active_or_queued");
        LOGI("network unavailable tone already active or queued");
        return true;
    }

    taskENTER_CRITICAL();
    uint32_t now = (uint32_t)lisa_os_get_tick_ms();
    if (s_network_reminder_pending) {
        taskEXIT_CRITICAL();
        return false; /* Another submit is in flight; its outcome is unknown. */
    }
    if (s_network_reminder_started &&
        (uint32_t)(now - s_network_reminder_ms) < NETWORK_REMINDER_INTERVAL_MS) {
        taskEXIT_CRITICAL();
        return true; /* This outage has already been announced recently. */
    }
    s_network_reminder_pending = true;
    taskEXIT_CRITICAL();

    was_playing = s_tone_playing;
    bool accepted = voice_player_queue_priority_tone(url);
    taskENTER_CRITICAL();
    if (accepted) {
        s_network_reminder_ms = now;
        s_network_reminder_started = true;
    }
    s_network_reminder_pending = false;
    taskEXIT_CRITICAL();
    sys_network_diag_mark("prompt",
                          accepted ? "network_unavailable_tone_accepted"
                                   : "network_unavailable_tone_failed",
                          tone_id,
                          was_playing ? "priority_queued" : "start_now");
    LOGI("network unavailable tone %s", accepted ? "accepted" : "failed");
    return accepted;
}

static bool voice_player_play_network_unavailable_tone(void)
{
    return voice_player_play_network_tone(TONE_ID_64);
}

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static void voice_player_offline_wake_network_feedback(uint32_t generation)
{
    sys_network_status_t network = {0};
    if (service_offline_action_prompt_current(generation) &&
        service_offline_action_wake_network_allowed(generation) &&
        service_offline_action_network_prompt_allowed() &&
        sys_network_get_status(&network) == 0 && !network.connected) {
        (void)voice_player_play_network_unavailable_tone();
    }
}
#endif

void voice_player_play_tone_url(const char *url)
{
    (void)voice_player_play_tone_url_internal(url, false, 0U);
}

void voice_player_play_prompt_tone_url(const char *url)
{
    (void)voice_player_play_tone_url_internal(url, true, 0U);
}

bool voice_player_try_play_prompt_tone_url(const char *url)
{
    return voice_player_play_tone_url_internal(url, true, 0U);
}

bool voice_player_try_play_prompt_tone_now_url(const char *url)
{
    /* IMU feedback describes what is happening now. Queuing it behind an
     * existing tone makes it sound like a false trigger after motion stops. */
    if (s_tone_playing) {
        LOGI("drop immediate prompt because tone player is busy");
        return false;
    }

    if (s_shutdown_prompt_in_progress) {
        return false;
    }

    return voice_player_start_tone_request(&(tone_play_request_t){
        .url = url,
        .prompt_tone = true,
    });
}

typedef struct { uint32_t ticket, generation; } beat_music_request_t;

static void voice_player_set_beat_result(uint32_t ticket, int result)
{
    taskENTER_CRITICAL();
    if (s_beat_music_ticket == ticket) s_beat_music_result = result;
    taskEXIT_CRITICAL();
}

static void voice_player_beat_music_event(app_player_t *player, app_player_event_t event, void *context)
{
    (void)player;
    uint32_t ticket = (uint32_t)(uintptr_t)context;
    if (ticket != __atomic_load_n(&s_beat_music_owner, __ATOMIC_ACQUIRE)) return;
    /* Natural EOF precedes the DAC tail. Cancellation must not flush the
     * shared output, which may already contain a new wakeup acknowledgement. */
    if (event == APP_PLAYER_EVENT_COMPLETED) voice_player_drain_wakeup_output();
    taskENTER_CRITICAL();
    if (s_beat_music_owner != ticket) { taskEXIT_CRITICAL(); return; }
    if (event == APP_PLAYER_EVENT_COMPLETED || event == APP_PLAYER_EVENT_STOPPED ||
        event == APP_PLAYER_EVENT_ERROR)
        __atomic_store_n(&s_beat_music_owner, 0U, __ATOMIC_RELEASE);
    if (ticket != s_beat_music_ticket) { taskEXIT_CRITICAL(); return; }
    if (event == APP_PLAYER_EVENT_PLAYING)
        __atomic_store_n(&s_beat_music_result, 0, __ATOMIC_RELEASE);
    else if (event == APP_PLAYER_EVENT_COMPLETED || event == APP_PLAYER_EVENT_STOPPED ||
             event == APP_PLAYER_EVENT_ERROR)
        __atomic_store_n(&s_beat_music_result,
                         event == APP_PLAYER_EVENT_COMPLETED ? 1 : -ECANCELED, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL();
}

static const char *voice_player_beat_music_url(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    extern const unsigned char music_131_data[];
    extern const unsigned int music_131_size;
    static char url[64];
    if (!url[0]) snprintf(url, sizeof(url), "mem://addr=%usize=%u",
                          (unsigned)(uintptr_t)music_131_data, music_131_size);
    return url;
#else
    return NULL;
#endif
}

static void voice_player_start_beat_music(void *data, uint32_t len)
{
    if (!data || len != sizeof(beat_music_request_t)) return;
    const beat_music_request_t *request = data;
    if (__atomic_load_n(&s_beat_music_ticket, __ATOMIC_ACQUIRE) != request->ticket) return;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (request->generation != service_offline_action_generation()) {
        voice_player_set_beat_result(request->ticket, -ECANCELED);
        return;
    }
#endif
    const char *url = voice_player_beat_music_url();
    if (!url || !beat_player || s_tone_playing || s_shutdown_prompt_in_progress ||
        __atomic_load_n(&s_beat_music_owner, __ATOMIC_ACQUIRE) ||
        !app_player_callbacks_idle(beat_player) ||
        app_player_get_state(beat_player) == APP_PLAYER_STATE_PLAYING) {
        voice_player_set_beat_result(request->ticket, -EBUSY);
        return;
    }
#ifdef CONFIG_VOICE_LINK_LINGXIN
    lingxin_server_suppress_current_tts("local_beat_music");
#endif
    /* The explicit song replaces old music without a resumable prompt intent. */
    if (music_player) (void)app_player_stop(music_player);
    if (__atomic_load_n(&s_beat_music_ticket, __ATOMIC_ACQUIRE) != request->ticket) return;
    __atomic_store_n(&s_beat_music_owner, request->ticket, __ATOMIC_RELEASE);
    app_player_register_callback(beat_player, voice_player_beat_music_event,
                                 (void *)(uintptr_t)request->ticket);
    if (app_player_play(beat_player, url) != APP_PLAYER_OK) {
        __atomic_store_n(&s_beat_music_owner, 0U, __ATOMIC_RELEASE);
        voice_player_set_beat_result(request->ticket, -EIO);
    }
}

uint32_t voice_player_beat_music_begin(void)
{
    uint32_t ticket = __atomic_add_fetch(&s_beat_music_sequence, 1U, __ATOMIC_RELAXED);
    if (!ticket) ticket = __atomic_add_fetch(&s_beat_music_sequence, 1U, __ATOMIC_RELAXED);
    beat_music_request_t request = {.ticket = ticket};
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    request.generation = service_offline_action_generation();
#endif
    taskENTER_CRITICAL();
    __atomic_store_n(&s_beat_music_result, -EAGAIN, __ATOMIC_RELEASE);
    __atomic_store_n(&s_beat_music_ticket, ticket, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL();
    if (voice_invoke(voice_player_start_beat_music, &request, sizeof(request)) != 0) {
        voice_player_set_beat_result(ticket, -EIO);
    }
    return ticket;
}

int voice_player_beat_music_position(uint32_t ticket, uint32_t *position_ms)
{
    if (!ticket || !position_ms ||
        __atomic_load_n(&s_beat_music_ticket, __ATOMIC_ACQUIRE) != ticket) return -ECANCELED;
    int result = __atomic_load_n(&s_beat_music_result, __ATOMIC_ACQUIRE);
    if (result != 0) return result;
    if (!beat_player || app_player_get_state(beat_player) != APP_PLAYER_STATE_PLAYING)
        return -EAGAIN; /* Terminal callback publishes the final result. */
    return app_player_get_position(beat_player, position_ms) == APP_PLAYER_OK ? 0 : -EIO;
}

static void voice_player_stop_beat_music(void *data, uint32_t len)
{
    if (!data || len != sizeof(uint32_t)) return;
    uint32_t ticket = *(const uint32_t *)data;
    /* Never stop the wakeup acknowledgement or a newer copy of the song. */
    if (__atomic_load_n(&s_beat_music_owner, __ATOMIC_ACQUIRE) == ticket && beat_player) {
        app_player_stop(beat_player);
    }
}

void voice_player_beat_music_end(uint32_t ticket)
{
    if (!ticket) return;
    uint32_t expected = ticket;
    (void)__atomic_compare_exchange_n(&s_beat_music_ticket, &expected, 0U, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    (void)voice_invoke(voice_player_stop_beat_music, &ticket, sizeof(ticket));
}

void voice_player_beat_music_cancel(void)
{
    voice_player_beat_music_end(__atomic_load_n(&s_beat_music_ticket, __ATOMIC_ACQUIRE));
}

static bool voice_player_upload_epoch_current(uint32_t epoch)
{
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    VoiceChatTransportStats stats = {0};
    voice_chat_get_transport_stats(&stats);
    return epoch && stats.epoch == epoch && !stats.terminating;
#else
    (void)epoch;
    return false;
#endif
}

static void voice_player_release_upload_cue(uint32_t epoch)
{
    (void)__atomic_compare_exchange_n(&s_upload_cue_epoch, &epoch, 0U, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void voice_player_upload_cue_note(uint32_t *counter, uint32_t epoch)
{
    taskENTER_CRITICAL();
    ++*counter;
    s_upload_cue_stats.last_epoch = epoch;
    taskEXIT_CRITICAL();
}

void voice_player_get_upload_cue_stats(voice_upload_cue_stats_t *stats)
{
    if (!stats) return;
    taskENTER_CRITICAL();
    *stats = s_upload_cue_stats;
    stats->pending_epoch = s_upload_cue_epoch;
    stats->playing_epoch = s_upload_cue_playing_epoch;
    taskEXIT_CRITICAL();
}

static void voice_player_upload_cue_event(app_player_event_t event)
{
    taskENTER_CRITICAL();
    uint32_t epoch = s_upload_cue_playing_epoch;
    if (epoch) {
        if (event == APP_PLAYER_EVENT_PLAYING && !s_upload_cue_playback_started) {
            s_upload_cue_playback_started = true;
            ++s_upload_cue_stats.started;
        } else if (event == APP_PLAYER_EVENT_COMPLETED || event == APP_PLAYER_EVENT_STOPPED ||
                   event == APP_PLAYER_EVENT_ERROR) {
            if (event == APP_PLAYER_EVENT_COMPLETED) ++s_upload_cue_stats.completed;
            else if (event == APP_PLAYER_EVENT_STOPPED) ++s_upload_cue_stats.stopped;
            else ++s_upload_cue_stats.playback_failures;
            s_upload_cue_playing_epoch = 0;
            /* A late completion belongs to the playing cue, not a newer
             * request already queued on the voice worker. */
            voice_player_release_upload_cue(epoch);
        }
    }
    taskEXIT_CRITICAL();
}

void voice_player_notify_upload_complete(uint32_t epoch)
{
    if (!voice_player_upload_epoch_current(epoch) ||
        __atomic_load_n(&s_upload_cue_cancelled_epoch, __ATOMIC_ACQUIRE) == epoch) {
        voice_player_upload_cue_note(&s_upload_cue_stats.skipped_stale, epoch);
        return;
    }
    upload_cue_request_t request = {.round_epoch = epoch};
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    request.owner_generation = service_offline_action_generation();
#endif
    taskENTER_CRITICAL();
    ++s_upload_cue_stats.valid_requests;
    s_upload_cue_stats.last_epoch = epoch;
    if (s_upload_cue_notified_epoch == epoch) {
        ++s_upload_cue_stats.duplicate_requests;
        taskEXIT_CRITICAL();
        return;
    }
    s_upload_cue_notified_epoch = epoch;
    __atomic_store_n(&s_upload_cue_epoch, epoch, __ATOMIC_RELEASE);
    taskEXIT_CRITICAL();
    /* Same voice event worker as wake/stop: local tone ownership is serialized. */
    if (voice_invoke(voice_player_start_upload_cue, &request, sizeof(request)) != 0) {
        voice_player_upload_cue_note(&s_upload_cue_stats.enqueue_failures, epoch);
        voice_player_release_upload_cue(epoch);
    }
}

void voice_player_cancel_upload_cue(uint32_t epoch)
{
    /* Cancels an enqueued cue without stopping another round's local tone. */
    taskENTER_CRITICAL();
    if (epoch && s_upload_cue_epoch == epoch && s_upload_cue_cancelled_epoch != epoch)
        ++s_upload_cue_stats.cancelled;
    __atomic_store_n(&s_upload_cue_cancelled_epoch, epoch, __ATOMIC_RELEASE);
    voice_player_release_upload_cue(epoch);
    taskEXIT_CRITICAL();
}

void voice_player_wait_upload_cue(void)
{
    uint32_t epoch = __atomic_load_n(&s_upload_cue_epoch, __ATOMIC_ACQUIRE);
    for (unsigned ms = 0; epoch && ms < 1000U; ms += 5U) {
        if (__atomic_load_n(&s_upload_cue_epoch, __ATOMIC_ACQUIRE) != epoch) return;
        if (!voice_player_upload_epoch_current(epoch)) {
            voice_player_release_upload_cue(epoch);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (epoch && __atomic_load_n(&s_upload_cue_epoch, __ATOMIC_ACQUIRE) == epoch) {
        voice_player_upload_cue_note(&s_upload_cue_stats.wait_timeouts, epoch);
        voice_player_release_upload_cue(epoch);
    }
}

static void voice_player_start_upload_cue(void *data, uint32_t len)
{
    if (!data || len != sizeof(upload_cue_request_t)) return;
    const upload_cue_request_t *request = data;
    uint32_t epoch = request->round_epoch;
    uint32_t *skip = NULL;
    if (__atomic_load_n(&s_upload_cue_epoch, __ATOMIC_ACQUIRE) != epoch ||
        __atomic_load_n(&s_upload_cue_cancelled_epoch, __ATOMIC_ACQUIRE) == epoch ||
        !voice_player_upload_epoch_current(epoch)
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        || !service_offline_action_online_current(request->owner_generation)
#endif
    ) skip = &s_upload_cue_stats.skipped_stale;
    else if (!tone_player || s_tone_playing || s_shutdown_prompt_in_progress)
        skip = &s_upload_cue_stats.skipped_busy;
    else if (s_tts_active) skip = &s_upload_cue_stats.skipped_tts;
    else if (!app_player_callbacks_idle(tone_player)) skip = &s_upload_cue_stats.skipped_callbacks;
    if (skip) {
        voice_player_upload_cue_note(skip, epoch);
        voice_player_release_upload_cue(epoch);
        return;
    }
    /* 80 ms 1 kHz sine with 8 ms fades. The player waits for PREPARED inside
     * its first write, before finish_stream can signal EOF. Match the proven
     * cloud prefill (>8 KiB) with a silent tail; never submit an undersized
     * 2560-byte first write that can stall the voice worker for 10 seconds. */
    static int16_t pcm[4608];
    static const int16_t sine[16] = {0,1531,2828,3696,4000,3696,2828,1531,
                                   0,-1531,-2828,-3696,-4000,-3696,-2828,-1531};
    for (unsigned i = 0; i < 1280U; ++i) {
        unsigned gain = i < 128U ? i : (i >= 1152U ? 1279U-i : 128U);
        pcm[i] = (int16_t)(sine[i % 16U] * (int)gain / 128);
    }
    s_tone_playing = true;
    s_current_tone_url = s_upload_cue_tag;
    s_current_tone_prompt = false;
    s_current_tone_wakeup_generation = 0U;
    taskENTER_CRITICAL();
    s_upload_cue_playing_epoch = epoch;
    s_upload_cue_playback_started = false;
    taskEXIT_CRITICAL();
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    s_current_offline_generation = 0U;
    memset(&s_current_local_request, 0, sizeof(s_current_local_request));
#endif
    voice_player_local_tone_gate_begin();
    if (app_player_play_stream(tone_player, 16000, 1, 16) != APP_PLAYER_OK ||
        app_player_write_stream(tone_player, (const uint8_t *)pcm, sizeof(pcm), 200) != sizeof(pcm) ||
        app_player_finish_stream(tone_player) != APP_PLAYER_OK) {
        voice_player_upload_cue_event(APP_PLAYER_EVENT_ERROR);
        (void)app_player_stop(tone_player);
        s_tone_playing = false;
        s_current_tone_url = NULL;
        voice_player_local_tone_gate_end();
        voice_player_release_upload_cue(epoch);
    }
}

static void voice_player_flush_tone_queue(void)
{
    taskENTER_CRITICAL();
    if (s_upload_cue_epoch || s_upload_cue_playing_epoch) ++s_upload_cue_stats.cancelled;
    __atomic_store_n(&s_upload_cue_epoch, 0U, __ATOMIC_RELEASE);
    s_upload_cue_playing_epoch = 0;
    taskEXIT_CRITICAL();
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_offline_action_local_audio_finished(s_current_local_request.local_generation, false);
    s_current_local_request.local_generation = 0;
    service_offline_action_prompt_finished(s_current_offline_generation, false);
    s_current_offline_generation = 0;
    for (uint8_t i = 0; i < s_tone_pending_count; ++i) {
        uint8_t index = (s_tone_pending_head + i) % TONE_PENDING_QUEUE_SIZE;
        service_offline_action_prompt_finished(s_tone_pending_queue[index].offline_generation, false);
        service_offline_action_local_audio_finished(s_tone_pending_queue[index].local_generation, false);
    }
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    voice_player_boot_rgb_event(APP_PLAYER_EVENT_STOPPED);
#endif
    bool dropped_wakeup_tone = s_current_tone_wakeup_generation != 0U;
    for (uint8_t i = 0; i < s_tone_pending_count && !dropped_wakeup_tone; i++) {
        uint8_t index = (s_tone_pending_head + i) % TONE_PENDING_QUEUE_SIZE;
        dropped_wakeup_tone = s_tone_pending_queue[index].wakeup_gate_generation != 0U;
    }

    s_tone_pending_head = 0;
    s_tone_pending_count = 0;
    s_tone_playing = false;
    s_current_tone_url = NULL;
    s_current_tone_prompt = false;
    s_current_tone_wakeup_generation = 0U;
    voice_player_prompt_tone_exit();

    if (dropped_wakeup_tone) {
        voice_player_schedule_wakeup_gate_release(s_latest_wakeup_gate_generation,
                                                  WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                  "wakeup_tone_aborted");
    }
}

void voice_player_abort_shutdown_prompt(void)
{
    s_shutdown_prompt_in_progress = false;
}

/* Shutdown owns audio after all accepted bus operations and old callbacks drain. */
int voice_player_play_shutdown_prompt_sync(void)
{
    uint16_t tone_id = TONE_ID_107;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (service_v2_low_battery_consume_shutdown_prompt_request()) {
        tone_id = TONE_ID_116;
    }
#endif
    const char *url = app_tone_get_url(tone_id);
    if (url == NULL && tone_id != TONE_ID_107) {
        LOGE("shutdown tone id=%u missing, fall back to id=%u",
             (unsigned)tone_id, (unsigned)TONE_ID_107);
        tone_id = TONE_ID_107;
        url = app_tone_get_url(tone_id);
    }
    if (tone_player == NULL || url == NULL) {
        LOGE("power-off tone unavailable: tone_player=%p url=%p id=%u",
             tone_player, url, (unsigned)tone_id);
        return -ENODEV;
    }
    LOGI("power-off tone start: id=%u url=%s", (unsigned)tone_id, url);
    s_shutdown_prompt_in_progress = true;
    TickType_t started = xTaskGetTickCount();
    while (s_audio_message_inflight != 0U) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(4000U)) return -ETIMEDOUT;
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    if (tts_player) { (void)app_player_stop(tts_player); }
    if (music_player) { (void)app_player_stop(music_player); }
    voice_player_beat_music_cancel();
    if (beat_player) { (void)app_player_stop(beat_player); }
    if (alert_player) { (void)app_player_stop(alert_player); }
    s_tone_pending_head = 0;
    s_tone_pending_count = 0;

    /* Stop the old producer, then wait for both queued and executing user
     * callbacks. A stale STOPPED must not be interpreted as the 107 result. */
    started = xTaskGetTickCount();
    while (!app_player_callbacks_idle(tone_player)) {
        /* A callback admitted before shutdown may still be starting its next
         * tone. Let it finish before stopping that producer. */
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(4000U)) return -ETIMEDOUT;
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    (void)app_player_stop(tone_player);
    started = xTaskGetTickCount();
    while (!app_player_callbacks_idle(tone_player)) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(4000U)) {
            return -ETIMEDOUT;
        }
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    s_shutdown_tone_result = -EINPROGRESS;
    s_shutdown_tone_playing = true;
    if (!voice_player_start_tone_request(&(tone_play_request_t){ .url = url, .prompt_tone = false })) {
        s_shutdown_tone_playing = false;
        return -EIO;
    }
    started = xTaskGetTickCount();
    while (s_shutdown_tone_result == -EINPROGRESS) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(6000U)) {
            s_shutdown_tone_result = -ETIMEDOUT;
            break;
        }
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    int result = s_shutdown_tone_result;
    s_shutdown_tone_playing = false;
    if (result != 0) {
        (void)app_player_stop(tone_player);
        voice_player_flush_tone_queue();
    }
    LOGI("power-off tone finished: result=%d", result);
    return result;
}

static void voice_player_wakeup_gate_timer_cb(TimerHandle_t timer)
{
    (void)timer;
#ifdef CONFIG_VOICE_LINK_LINGXIN
    taskENTER_CRITICAL();
    uint32_t generation = s_wakeup_gate_timer_generation;
    TickType_t not_before = s_wakeup_gate_not_before_tick;
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - not_before) < 0) {
        TickType_t remaining = not_before - now;
        (void)xTimerChangePeriod(s_wakeup_gate_timer, remaining > 0U ? remaining : 1U, 0);
        taskEXIT_CRITICAL();
        return;
    }
    taskEXIT_CRITICAL();
    lingxin_server_audio_uplink_gate_open(generation,
                                          "wakeup_tone_tail_elapsed");
#endif
}

static void voice_player_schedule_wakeup_gate_release(uint32_t generation,
                                                      uint32_t delay_ms,
                                                      const char *reason)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (generation == 0U) {
        return;
    }

    taskENTER_CRITICAL();
    if (generation != s_latest_wakeup_gate_generation) {
        taskEXIT_CRITICAL();
        return; /* A late completion cannot replace a newer wake's failsafe. */
    }
    s_wakeup_gate_timer_generation = generation;
    s_wakeup_gate_not_before_tick = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    if (delay_ms == 0U) {
        taskEXIT_CRITICAL();
        /* FreeRTOS timers reject zero periods. The old failsafe may still
         * fire; generation validation and idempotent open make it harmless. */
        lingxin_server_audio_uplink_gate_open(generation, reason);
        return;
    }
    /* Nonblocking timer queue submission shares the generation lock, so an
     * old 5s command cannot land behind a newer 200ms tail command. */
    bool scheduled = s_wakeup_gate_timer != NULL &&
        xTimerChangePeriod(s_wakeup_gate_timer, pdMS_TO_TICKS(delay_ms), 0) == pdPASS;
    taskEXIT_CRITICAL();
    if (!scheduled) {
        LOGW("wakeup audio gate timer unavailable, release immediately: generation=%u reason=%s",
             (unsigned)generation, reason ? reason : "");
        lingxin_server_audio_uplink_gate_open(generation, "gate_timer_unavailable");
        return;
    }
    LOGI("wakeup audio gate release scheduled: generation=%u delay=%ums reason=%s",
         (unsigned)generation, (unsigned)delay_ms, reason ? reason : "");
#else
    (void)generation;
    (void)delay_ms;
    (void)reason;
#endif
}

static uint32_t voice_player_begin_wakeup_gate(const char *reason)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    uint32_t generation = lingxin_server_audio_uplink_gate_hold(reason);
    taskENTER_CRITICAL();
    s_latest_wakeup_gate_generation = generation;
    taskEXIT_CRITICAL();
    voice_player_schedule_wakeup_gate_release(generation,
                                              WAKEUP_UPLINK_GATE_FAILSAFE_MS,
                                              "wakeup_tone_failsafe");
    return generation;
#else
    (void)reason;
    return 0U;
#endif
}

static void voice_player_play_wakeup_tone(uint32_t wakeup_gate_generation)
{
    char *tone_url = voice_player_get_wakeup_tone_url();
    if (tone_url == NULL) {
        LOGE("wakeup tone url is null");
        voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                  WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                  "wakeup_tone_url_missing");
        return;
    }

    if (!voice_player_play_tone_url_internal(tone_url, false, wakeup_gate_generation)) {
        voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                  WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                  "wakeup_tone_start_failed");
    }
}

static void voice_player_reconnected_wakeup(void *data, uint32_t len)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    uint32_t generation, current_generation;
    if (len != sizeof(generation)) {
        return;
    }
    voice_player_flush_tone_queue();
    memcpy(&generation, data, sizeof(generation));
    (void)lingxin_server_audio_uplink_gate_is_open(&current_generation);
    if (generation != current_generation) {
        return; /* A newer button/keyword already owns the acknowledgement. */
    }
    taskENTER_CRITICAL();
    s_latest_wakeup_gate_generation = generation;
    taskEXIT_CRITICAL();
    voice_player_schedule_wakeup_gate_release(generation, WAKEUP_UPLINK_GATE_FAILSAFE_MS,
                                              "reconnected_wakeup_failsafe");
    voice_player_play_wakeup_tone(generation);
#else
    (void)data;
    (void)len;
#endif
}

void voice_player_begin_shutdown_prompt(void)
{
    /* Claim audio before changing the offline generation, whose cancellation
     * message can be delivered after the power-off tone has started. */
    taskENTER_CRITICAL();
    s_shutdown_prompt_in_progress = true;
    taskEXIT_CRITICAL();
}

int voice_player_request_reconnected_wakeup_tone(void)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    uint32_t generation = lingxin_server_audio_uplink_gate_close("reconnected_wakeup");
    int ret = voice_invoke(voice_player_reconnected_wakeup, &generation, sizeof(generation));
    if (ret != 0) {
        lingxin_server_audio_uplink_gate_open(generation, "reconnected_prompt_queue_failed");
    }
    return ret;
#else
    return -1;
#endif
}

static void voice_player_play_network_success_tone(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_network_prompt_allowed()) return;
#endif
    static TickType_t s_network_success_tone_tick = 0;
    TickType_t now = xTaskGetTickCount();

    if (service_v2_voice_standby_background_audio_suppressed()) {
        LOGI("skip network success tone during voice standby");
        return;
    }

    if (s_network_success_tone_tick != 0 &&
        (now - s_network_success_tone_tick) < pdMS_TO_TICKS(3000U)) {
        return;
    }

    voice_player_play_tone_url(app_tone_get_url(TONE_ID_59));
    s_network_success_tone_tick = now;
}

void voice_player_play_network_recovery_tone(void)
{
    /* This explicit wakeup recovery can race the queued PROBE_SUCCESS event.
     * Clear the disconnect latch here so that either ordering produces one
     * success prompt and the next real outage is not suppressed. */
    s_disconnect_tone_played = false;
    if (service_v2_voice_standby_background_audio_suppressed()) {
        LOGI("skip explicit network recovery tone during voice standby");
        return;
    }
    voice_player_play_network_success_tone();
}

static void cloud_success_tone_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    voice_player_play_network_success_tone();
}

static void voice_player_cancel_cloud_connect_success_tone(void)
{
    if (s_cloud_success_tone_timer != NULL &&
        xTimerIsTimerActive(s_cloud_success_tone_timer) != pdFALSE &&
        xTimerStop(s_cloud_success_tone_timer, 0) != pdPASS) {
        LOGW("failed to stop cloud success tone timer");
    }

    s_cloud_success_tone_played = false;
}

static void voice_player_schedule_cloud_connect_success_tone(void)
{
    if (s_cloud_success_tone_timer == NULL) {
        voice_player_play_network_success_tone();
        return;
    }

    if (xTimerReset(s_cloud_success_tone_timer, 0) != pdPASS) {
        LOGW("failed to start cloud success tone timer");
        voice_player_play_network_success_tone();
    }
}

static void voice_player_handle_cloud_connect_success(bool force_play)
{
    s_disconnect_tone_played = false;
    s_cloud_unstable_pending = false;
    s_cloud_unstable_pending_tick = 0;

    if (!force_play && !s_cloud_reconnect_pending) {
        return;
    }

    if (s_cloud_success_tone_played) {
        s_cloud_reconnect_pending = false;
        return;
    }

    voice_player_schedule_cloud_connect_success_tone();
    s_cloud_success_tone_played = true;
    s_cloud_reconnect_pending = false;
}

static void voice_player_skip_cloud_connect_success_tone(void)
{
    voice_player_cancel_cloud_connect_success_tone();
    s_disconnect_tone_played = false;
    s_cloud_unstable_pending = false;
    s_cloud_unstable_pending_tick = 0;
    s_cloud_reconnect_pending = false;
    s_cloud_success_tone_played = false;
}

static bool voice_player_is_cloud_unbound_active(void)
{
    return voice_cloud_is_connected() &&
           voice_cloud_get_state() == VOICE_CLOUD_STATE_CONNECT_FAILED;
}

static bool voice_player_should_skip_disconnect_tone(const sys_network_status_t *network_status,
                                                     bool network_status_ok)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_network_prompt_allowed()) return true;
#endif
    if (service_v2_voice_standby_background_audio_suppressed()) {
        s_disconnect_tone_played = false;
        s_cloud_unstable_pending = false;
        s_cloud_unstable_pending_tick = 0;
        s_cloud_reconnect_pending = false;
        s_cloud_success_tone_played = false;
        LOGI("skip network disconnect tone during voice standby");
        return true;
    }

    if (network_status_ok && network_status->switching) {
        return true;
    }

    /*
     * Keep disconnect tones quiet in explicit provisioning flow:
     * saved APs have been cleared (e.g. user multi-click to enter netcfg).
     */
    if (!sys_wifi_has_ap()) {
        s_disconnect_tone_played = false;
        s_cloud_unstable_pending = false;
        s_cloud_unstable_pending_tick = 0;
        s_cloud_reconnect_pending = false;
        s_cloud_success_tone_played = false;
        LOGI("skip network disconnect tone because no saved AP");
        return true;
    }

    return false;
}

static bool voice_player_has_network_link(const sys_network_status_t *network_status,
                                          bool network_status_ok)
{
    if (!network_status_ok) {
        return false;
    }

    if (network_status->active_bearer == SYS_NETWORK_BEARER_WIFI) {
        return network_status->wifi_connected;
    }

    if (network_status->active_bearer == SYS_NETWORK_BEARER_MODEM) {
        return network_status->modem_connected;
    }

    return false;
}

static bool voice_player_network_is_unavailable(const sys_network_status_t *network_status,
                                                bool network_status_ok)
{
    return !voice_player_has_network_link(network_status, network_status_ok) ||
           !network_status->connected;
}

static bool voice_player_should_play_cloud_unstable_tone(const sys_network_status_t *network_status,
                                                         bool network_status_ok)
{
    if (!voice_player_has_network_link(network_status, network_status_ok)) {
        return false;
    }

    /*
     * Cloud disconnected while bearer link is still alive but probe already
     * reports internet unavailable: this is the "network unstable" case.
     */
    return !network_status->connected;
}

static bool voice_player_network_event_current(uint32_t msg_id, void *data, uint32_t len,
                                                const sys_network_status_t *status, bool status_ok)
{
    if (!status_ok || status->switching) return false;
    if (data != NULL) {
        uint32_t generation;
        if (len != sizeof(generation)) return false;
        memcpy(&generation, data, sizeof(generation));
        if (!sys_network_generation_is_current(generation)) return false;
    } else if (len != 0U) {
        return false;
    }
    /* Legacy producers have no token. In either case require the current
     * physical state, so a queued outage cannot speak after DHCP recovered. */
    bool link = status->active_bearer == SYS_NETWORK_BEARER_WIFI ? status->wifi_connected :
                status->active_bearer == SYS_NETWORK_BEARER_MODEM && status->modem_connected;
    if (msg_id == VOICE_MSG_SYSTEM_NETWORK_CONNECTED) return link;
    return status->active_bearer == SYS_NETWORK_BEARER_NONE &&
           !status->wifi_connected && !status->modem_connected;
}

static bool voice_player_cloud_is_ready(const struct app_datas *app_datas)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    /* Lingxin opens the per-turn WebSocket lazily after wakeup.  The legacy
     * voice_cloud_connected flag is only updated by the background probe and
     * can still be zero while Wi-Fi already has a usable bearer. */
    sys_network_status_t network_status;
    (void)app_datas;
    if (sys_network_get_status(&network_status) != 0 || !network_status.connected) {
        return false;
    }
    return true;
#else
    (void)app_datas;
    return voice_cloud_is_connected();
#endif
}

static void voice_player_reset_cloud_unstable_pending(void)
{
    s_cloud_unstable_pending = false;
    s_cloud_unstable_pending_tick = 0;
}

static void voice_player_try_play_cloud_unstable_tone(const sys_network_status_t *network_status,
                                                      bool network_status_ok)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_network_prompt_allowed()) return;
#endif
    TickType_t now;

    if (s_disconnect_tone_played) {
        sys_network_diag_mark("prompt", "congestion_tone_suppressed", 0, "disconnect_tone_already_played");
        return;
    }

    if (!voice_player_should_play_cloud_unstable_tone(network_status, network_status_ok)) {
        sys_network_diag_mark("prompt", "congestion_tone_not_eligible", 0, "link_or_probe_state");
        voice_player_reset_cloud_unstable_pending();
        return;
    }

    now = xTaskGetTickCount();
    if (!s_cloud_unstable_pending) {
        s_cloud_unstable_pending = true;
        s_cloud_unstable_pending_tick = now;
        sys_network_diag_mark("prompt", "congestion_grace_started", CLOUD_UNSTABLE_TONE_GRACE_MS,
                              "wait_for_second_failure_event");
        return;
    }

    if ((now - s_cloud_unstable_pending_tick) < pdMS_TO_TICKS(CLOUD_UNSTABLE_TONE_GRACE_MS)) {
        sys_network_diag_mark("prompt", "congestion_grace_active", CLOUD_UNSTABLE_TONE_GRACE_MS,
                              "tone_deferred");
        return;
    }

    sys_network_diag_mark("prompt", "play_network_congested", TONE_ID_65, "065_net_error");
    s_disconnect_tone_played = voice_player_play_network_tone(TONE_ID_65);
    voice_player_reset_cloud_unstable_pending();
}

static void voice_player_handle_msg(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    if (s_shutdown_prompt_in_progress) return;

    struct app_datas *app_datas = get_app_datas();
    sys_network_status_t network_status = {0};
    bool network_status_ok = sys_network_get_status(&network_status) == 0;

    if (app_datas == NULL) {
        LOGW("Invalid app_datas");
        return;
    }
    switch (msg_id) {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    case VOICE_MSG_OFFLINE_AUDIO_CANCEL:
        if (!data || len != sizeof(uint32_t) || *(uint32_t *)data != service_offline_action_generation()) break;
        voice_player_beat_music_cancel();
        if (beat_player) (void)app_player_stop(beat_player);
        voice_player_cancel_cloud_connect_success_tone();
        voice_player_flush_tone_queue();
        app_player_stop(tone_player);
        app_player_stop(tts_player);
        s_offline_audio_cancel_generation = *(uint32_t *)data;
        break;
    case VOICE_MSG_OFFLINE_LOCAL_VOICE: {
        if (!data || len != sizeof(voice_msg_offline_local_voice_t)) break;
        const voice_msg_offline_local_voice_t *message = data;
        if (!service_offline_action_local_audio_current(message->generation)) break;
        tone_play_request_t request = {.local_generation = message->generation};
        if (message->command == OFFLINE_CMD_VERSION) {
            request.token_count = service_local_version_tokens(PROJECT_VERSION_STR, request.tokens, 32);
        } else {
            request.token_count = 1;
            request.tokens[0] = message->command == OFFLINE_CMD_ENTER_OFFLINE ? 117 : 118;
        }
        if (request.token_count == 0) {
            LOGE("invalid local system version: %s", PROJECT_VERSION_STR);
            service_offline_action_local_audio_finished(message->generation, false);
        } else if (s_tone_playing && s_tone_pending_count < TONE_PENDING_QUEUE_SIZE) {
            s_tone_pending_queue[(s_tone_pending_head + s_tone_pending_count++) % TONE_PENDING_QUEUE_SIZE] = request;
        } else if (s_tone_playing || !voice_player_start_tone_request(&request)) {
            service_offline_action_local_audio_finished(message->generation, false);
        }
        break;
    }
    case VOICE_MSG_OFFLINE_ACTION_PROMPT: {
        if (!data || len != sizeof(uint32_t)) break;
        uint32_t generation = *(uint32_t *)data;
        if (!service_offline_action_prompt_current(generation)) break;
        /* A wake acknowledgement must not queue behind a network reminder. */
        tone_play_request_t request = {
            .url = voice_player_get_wakeup_tone_url(), .offline_generation = generation,
        };
        if (s_tone_playing && s_tone_pending_count < TONE_PENDING_QUEUE_SIZE) {
            s_tone_pending_queue[(s_tone_pending_head + s_tone_pending_count++) % TONE_PENDING_QUEUE_SIZE] = request;
        } else if (s_tone_playing || !voice_player_start_tone_request(&request)) {
            service_offline_action_prompt_finished(generation, false);
        }
        break;
    }
#endif
    case VOICE_MSG_WAKEUP_BUTTON_START: {
        if (!service_button_wakeup_event_current(data, len)) break;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (service_offline_action_forced_offline() ||
            (network_status_ok && !network_status.connected)) break;
#endif
        uint32_t wakeup_gate_generation = voice_player_begin_wakeup_gate("button_wakeup_tone");

        if (service_sd_music_is_syncing()) {
            LOGI("ignore button wakeup tone during SD music sync");
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "button_tone_skipped_sd_sync");
            return;
        }

        if ((app_datas->voice_work_mode & VOICE_WORK_MODE_BUTTON_WAKEUP) == 0) {
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "button_tone_mode_disabled");
            return;
        }

        if (!app_datas->can_wakeup) {
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "button_tone_wakeup_disabled");
            return;
        }

        if (voice_player_network_is_unavailable(&network_status, network_status_ok)) {
            (void)voice_player_play_network_unavailable_tone();
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "button_cloud_unavailable");
        } else if (voice_player_cloud_is_ready(app_datas)) {
            voice_player_play_wakeup_tone(wakeup_gate_generation);
        } else if (app_datas->auth_failed) {
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_105));
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "button_auth_failed");
        } else {
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_85));
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "button_cloud_not_ready");
        }
    } break;
    case VOICE_MSG_WAKEUP_KEYWORD: {
        uint32_t wakeup_gate_generation = voice_player_begin_wakeup_gate("keyword_wakeup_tone");

        if (service_sd_music_is_syncing()) {
            LOGI("ignore keyword wakeup tone during SD music sync");
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "keyword_tone_skipped_sd_sync");
            return;
        }

        if ((app_datas->voice_work_mode & VOICE_WORK_MODE_VOICE_WAKEUP) == 0) {
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "keyword_tone_mode_disabled");
            return;
        }

        if (!app_datas->can_wakeup) {
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "keyword_tone_wakeup_disabled");
            return;
        }

        if (voice_player_network_is_unavailable(&network_status, network_status_ok)) {
            (void)voice_player_play_network_unavailable_tone();
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "keyword_cloud_unavailable");
        } else if (voice_player_cloud_is_ready(app_datas)) {
            voice_player_play_wakeup_tone(wakeup_gate_generation);
        } else if (app_datas->auth_failed) {
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_105));
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "keyword_auth_failed");
        } else {
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_85));
            voice_player_schedule_wakeup_gate_release(wakeup_gate_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "keyword_cloud_not_ready");
        }
    } break;
    case VOICE_MSG_CLOUD_CLOUD_AUTH_SUCCESS: {
#ifdef CONFIG_VOICE_LINK_LINGXIN
        LOGI("ignore legacy cloud auth success tone in Lingxin mode");
        app_datas->auth_failed = 0;
        break;
#endif
        app_datas->auth_failed = 0;
        voice_player_handle_cloud_connect_success(true);
    } break;
    case VOICE_MSG_CLOUD_CLOUD_AUTH_FAILED: {
#ifdef CONFIG_VOICE_LINK_LINGXIN
        LOGI("ignore legacy cloud auth failed tone in Lingxin mode");
        break;
#endif
        voice_player_cancel_cloud_connect_success_tone();
        app_datas->auth_failed = 1;
        voice_player_play_tone_url(app_tone_get_url(TONE_ID_105));
    } break;
    case VOICE_MSG_CLOUD_OPEN_INFO: {
        uint32_t status = 0;

        if (data == NULL || len != sizeof(status)) {
            break;
        }

        memcpy(&status, data, sizeof(status));
        if (status == CLOUD_OPEN_INFO_STATUS_BIND) {
            voice_player_cancel_cloud_connect_success_tone();
            if (voice_player_is_cloud_unbound_active()) {
                voice_player_flush_tone_queue();
                app_player_stop(tone_player);
                app_player_stop(tts_player);
                break;
            }
            /* Replace the former 071 binding prompt with 059. Reuse the
             * success-prompt deduplication and never report success offline. */
            if (network_status_ok && network_status.connected) {
                voice_player_play_network_success_tone();
            }
        }
    } break;
    case VOICE_MSG_BLE_AUTH_INFO_DONE: {
        s_suppress_next_cloud_success_tone =
            (voice_cloud_get_state() == VOICE_CLOUD_STATE_CONNECT_FAILED);
    } break;
    case VOICE_MSG_CLOUD_CONNECTED: {
#ifdef CONFIG_VOICE_LINK_LINGXIN
        app_datas->auth_failed = 0;
#endif
        if (s_suppress_next_cloud_success_tone) {
            s_suppress_next_cloud_success_tone = false;
            voice_player_skip_cloud_connect_success_tone();
            break;
        }
        voice_player_handle_cloud_connect_success(false);
    } break;
    case VOICE_MSG_CLOUD_DISCONNECTED: {
        sys_network_diag_mark("prompt", "cloud_disconnected_event", 0, "evaluate_congestion_tone");
        voice_player_cancel_cloud_connect_success_tone();
        if (voice_player_should_skip_disconnect_tone(&network_status, network_status_ok)) {
            sys_network_diag_mark("prompt", "cloud_disconnect_suppressed", 0, "no_saved_ap_or_switching");
            break;
        }
        s_cloud_reconnect_pending = true;
        s_cloud_success_tone_played = false;
        voice_player_try_play_cloud_unstable_tone(&network_status, network_status_ok);
    } break;
    case VOICE_MSG_SYSTEM_NETWORK_CONNECTED: {
        if (!voice_player_network_event_current(msg_id, data, len, &network_status, network_status_ok)) break;
        sys_network_diag_mark("prompt", "bearer_connected_event", 0, "clear_congestion_pending");
        voice_player_reset_cloud_unstable_pending();
    } break;
    case VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL: {
        if (!network_probe_event_current(data, len, false)) break;
        sys_network_diag_mark("prompt", "probe_failed_event", 0, "evaluate_congestion_tone");
        if (voice_player_should_skip_disconnect_tone(&network_status, network_status_ok)) {
            sys_network_diag_mark("prompt", "probe_failure_suppressed", 0, "no_saved_ap_or_switching");
            break;
        }
        /* Probe has already confirmed the failure. Its edge is sent once,
         * so waiting for a second cloud failure would leave the user silent. */
        if (network_status_ok && !network_status.connected && !s_disconnect_tone_played) {
            s_disconnect_tone_played = voice_player_play_network_unavailable_tone();
        }
    } break;
    case VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS: {
        if (!network_status_ok || !network_status.connected) break;
        if (!network_probe_event_current(data, len, true)) break;
        sys_network_diag_mark("prompt", "probe_success_event", s_disconnect_tone_played ? 1 : 0,
                              "clear_or_play_recovery");
        voice_player_reset_cloud_unstable_pending();
        if (s_disconnect_tone_played) {
            s_disconnect_tone_played = false;
            voice_player_play_network_success_tone();
        }
    } break;
    case VOICE_MSG_SYSTEM_NETWORK_DISCONNECTED: {
        if (!voice_player_network_event_current(msg_id, data, len, &network_status, network_status_ok)) break;
        sys_network_diag_mark("prompt", "bearer_disconnected_event", 0, "evaluate_disconnect_tone");
        voice_player_reset_cloud_unstable_pending();
        if (voice_player_should_skip_disconnect_tone(&network_status, network_status_ok)) {
            sys_network_diag_mark("prompt", "disconnect_suppressed", 0, "no_saved_ap_or_switching");
            break;
        }

        if (!s_disconnect_tone_played) {
            sys_network_diag_mark("prompt", "play_network_disconnected", TONE_ID_60, "060_network_fail");
            s_disconnect_tone_played = voice_player_play_network_tone(TONE_ID_60);
        }
    } break;
    case VOICE_MSG_BLE_CONNECT_DONE: {
        voice_player_play_tone_url(app_tone_get_url(TONE_ID_72));
    } break;
    case VOICE_MSG_CLOUD_TTS_URL: {
        if (data == NULL) {
            LOGE("Invalid data");
            break;
        }

        if (voice_car_control_consume_cloud_tts_suppress() ||
            voice_motutu_control_consume_cloud_tts_suppress()) {
            LOGI("suppress cloud TTS after local motion command");
            voice_msg_pub(VOICE_MSG_PLAYER_TTS_STOPED, NULL, 0);
            break;
        }

        LOGI("Ready to play tts url asynchronously: %s", (char *)data);
        voice_player_tts_track_url((char *)data);
        if (voice_intent_photo_flow_should_gate_tts()) {
            break;
        }
        /* 使用异步播放接口，避免在ebus回调中阻塞HTTP下载 */
        async_play_url(tts_player, (char *)data);

    } break;
    case VOICE_MSG_CLOUD_PUSHUP_TTS_URL: {
        if (data == NULL) {
            LOGE("Invalid data");
            break;
        }

        voice_player_cancel_cloud_connect_success_tone();
        if (voice_player_is_cloud_unbound_active()) {
            voice_player_flush_tone_queue();
            app_player_stop(tone_player);
            app_player_stop(tts_player);
        }

        LOGI("Ready to play tts url asynchronously: %s", (char *)data);
        voice_player_tts_track_url((char *)data);
        if (voice_intent_photo_flow_should_gate_tts()) {
            break;
        }
        /* 临时规避：pushup MP3 可能还在云端生成，下载到连续两次大小一致后再播。 */
        voice_player_queue_pushup_tts_ready_check((char *)data);

    } break;
    case VOICE_MSG_CLOUD_MCP_CALL_RESP:
        break;
    default:
        break;
    }
}

static void voice_player_restart_music_intent(void)
{
    if (voice_intent_contains(INTENT_MUSIC)) {
        voice_intent_pop(INTENT_MUSIC);
    }
    if (voice_intent_contains(INTENT_VOICE_SESSION)) {
        LOGI("force finish voice session before music playback");
        voice_intent_pop(INTENT_VOICE_SESSION);
    }
    voice_intent_music_set_user_paused(false);
    voice_intent_push(INTENT_MUSIC);
}

/* 云端歌曲列表到达（MCP kuwo / audio_url 等工具）。
 * 替换当前在线歌单为收到的曲目，从头开始播放，并通知 UI 显示第一首歌名。 */
static void voice_player_audio_item(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    struct voice_msg_audio_items *msg_items = (struct voice_msg_audio_items *)data;
    if (msg_items == NULL || msg_items->cnt == 0) {
        LOGE("Invalid data");
        return;
    }

    int count = (msg_items->cnt > 50) ? 50 : (int)msg_items->cnt;

    music_item_t *tracks = lisa_mem_alloc(count * sizeof(music_item_t));
    if (tracks == NULL) {
        LOGE("Failed to alloc tracks");
        return;
    }
    memset(tracks, 0, count * sizeof(music_item_t));

    for (int i = 0; i < count; i++) {
        memcpy(tracks[i].mid, msg_items->items[i].id, sizeof(msg_items->items[i].id));
        memcpy(tracks[i].m_name, msg_items->items[i].name, sizeof(msg_items->items[i].name));
        /* play_audio_link 链路会预填充 url，直接复制即可跳过 URL 解析 */
        if (msg_items->items[i].url[0]) {
            memcpy(tracks[i].m_url, msg_items->items[i].url, sizeof(msg_items->items[i].url));
        }
    }

    char first_track_name[AUIDO_OUT_NAME_LEN] = {0};
    if (tracks[0].m_name[0]) {
        strncpy(first_track_name, tracks[0].m_name, sizeof(first_track_name) - 1);
    }

    if (voice_music_list_set(MUSIC_LIST_ONLINE, tracks, count) != 0) {
        LOGE("Failed to set online music list");
        lisa_mem_free(tracks);
        return;
    }
    voice_music_list_set_active(MUSIC_LIST_ONLINE);
    voice_music_list_set_current_index(0);
    lisa_mem_free(tracks);

    /* apps-ui 显示歌曲名称*/
    if (first_track_name[0]) {
        voice_msg_pub(VOICE_MSG_CLOUD_MUSIC_NAME, first_track_name, strlen(first_track_name) + 1);
    }

    voice_player_restart_music_intent();
}

/* 用户主动播放控制（MCP ls.playback_control 工具驱动）。
 *
 * PLAY/PAUSE 只改变播放状态，不改变 MUSIC intent（仍在栈上）。
 * PAUSE 通过 s_user_paused 标记阻止后续自动 resume。
 * NEXT/PREVIOUS/REPLAY 切换曲目，同时清除 s_user_paused。
 * STOP 直接 pop MUSIC → on_exit → stop 播放器。 */
static void voice_player_play_control(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    switch (msg_id) {

    /* ---- 播放状态控制 ---- */
    case VOICE_MSG_PLAY_CONTROL_PLAY: {
        voice_intent_music_set_user_paused(false);
        app_player_resume(music_player);
    } break;
    case VOICE_MSG_PLAY_CONTROL_PAUSE: {
        voice_intent_music_set_user_paused(true);
        app_player_pause(music_player);
        voice_msg_pub(VOICE_MSG_CLOUD_MUSIC_NAME, NULL, 0);
    } break;

    /* ---- 曲目切换 ---- */
    case VOICE_MSG_PLAY_CONTROL_NEXT: {
        voice_intent_music_set_user_paused(false);
        music_item_t next_track;
        if (voice_music_list_get_next(&next_track) == 0) {
            if (voice_player_play_music_url(next_track.m_url) == APP_PLAYER_OK &&
                next_track.m_name[0]) {
                voice_msg_pub(VOICE_MSG_CLOUD_MUSIC_NAME, next_track.m_name, strlen(next_track.m_name) + 1);
            }
        }
    } break;
    case VOICE_MSG_PLAY_CONTROL_PREVIOUS: {
        voice_intent_music_set_user_paused(false);
        music_item_t prev_track;
        if (voice_music_list_get_prev(&prev_track) == 0) {
            if (voice_player_play_music_url(prev_track.m_url) == APP_PLAYER_OK &&
                prev_track.m_name[0]) {
                voice_msg_pub(VOICE_MSG_CLOUD_MUSIC_NAME, prev_track.m_name, strlen(prev_track.m_name) + 1);
            }
        }
    } break;
    case VOICE_MSG_PLAY_CONTROL_REPLAY: {
        voice_intent_music_set_user_paused(false);
        music_item_t curr_track;
        if (voice_music_list_get_current(&curr_track) == 0) {
            if (voice_player_play_music_url(curr_track.m_url) == APP_PLAYER_OK &&
                curr_track.m_name[0]) {
                voice_msg_pub(VOICE_MSG_CLOUD_MUSIC_NAME, curr_track.m_name, strlen(curr_track.m_name) + 1);
            }
        }
    } break;

    /* ---- 停止 ---- */
    case VOICE_MSG_PLAY_CONTROL_STOP: {
        voice_msg_pub(VOICE_MSG_CLOUD_MUSIC_NAME, NULL, 0);
        voice_intent_pop(INTENT_MUSIC);
    } break;
    default:
        break;
    }
}

static void voice_alarm_ring_play_once(const char *text)
{
    struct app_datas *app_datas = get_app_datas();

    switch (s_alarm_play_ctx.phase) {
        case ALARM_PLAY_PHASE_TONE_FIRST:
            /* 第一阶段：播放默认闹钟提示音（一次） */
            LISA_LOGI(TAG, "alarm play phase: TONE_FIRST");
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_94));
            s_alarm_play_ctx.phase = ALARM_PLAY_PHASE_TTS;
            if (text) {
                strncpy(s_alarm_play_ctx.text, text, sizeof(s_alarm_play_ctx.text) - 1);
            }
            break;

        case ALARM_PLAY_PHASE_TTS:
            /* 第二阶段：播放云端TTS（一次） */
            LISA_LOGI(TAG, "alarm play phase: TTS");
            if (app_datas == NULL || app_datas->voice_cloud_connected == 0 ||
                !s_alarm_play_ctx.text || s_alarm_play_ctx.text[0] == 0) {
                /* 云端未连接或文本为空，跳过TTS，直接进入循环播放提示音 */
                LISA_LOGI(TAG, "alarm TTS skipped: no cloud or no text");
                s_alarm_play_ctx.phase = ALARM_PLAY_PHASE_TONE_LOOP;
                voice_player_play_tone_url(app_tone_get_url(TONE_ID_94));
                break;
            }

            char *tts_text = lisa_mem_alloc(ALARM_TTS_BUF_SIZE);
            if (tts_text == NULL) {
                LISA_LOGW(TAG, "alarm TTS alloc failed, fallback to tone loop");
                s_alarm_play_ctx.phase = ALARM_PLAY_PHASE_TONE_LOOP;
                voice_player_play_tone_url(app_tone_get_url(TONE_ID_94));
                break;
            }

            snprintf(tts_text, ALARM_TTS_BUF_SIZE, "你有一个 %s 的提醒, 请不要忘记哦!", s_alarm_play_ctx.text);
            voice_cloud_tts_synth(tts_text);
            lisa_mem_free(tts_text);
            s_alarm_play_ctx.phase = ALARM_PLAY_PHASE_TONE_LOOP;
            break;

        case ALARM_PLAY_PHASE_TONE_LOOP:
            /* 第三阶段：循环播放默认闹钟提示音 */
            LISA_LOGI(TAG, "alarm play phase: TONE_LOOP");
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_94));
            break;
    }
}

static void voice_alarm_ring_force_stop(void)
{
    voice_player_flush_tone_queue();
    app_player_stop(tone_player);
    app_player_stop(tts_player);

    /* 重置闹钟播放状态机，确保下次触发时从头开始 */
    s_alarm_play_ctx.phase = ALARM_PLAY_PHASE_TONE_FIRST;
    memset(s_alarm_play_ctx.text, 0, sizeof(s_alarm_play_ctx.text));
}


void voice_player_play_msg(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    taskENTER_CRITICAL();
    if (s_shutdown_prompt_in_progress) {
        taskEXIT_CRITICAL();
        return;
    }
    ++s_audio_message_inflight;
    taskEXIT_CRITICAL();
    voice_player_handle_msg(unused, msg_id, data, len, user_data);
    taskENTER_CRITICAL();
    --s_audio_message_inflight;
    taskEXIT_CRITICAL();
}

static void on_tts_event(app_player_t *player, app_player_event_t event, void *user_data)
{
    LOGI("tts player event: %d", event);

    switch (event) {
    case APP_PLAYER_EVENT_PLAYING:
        voice_msg_pub(VOICE_MSG_PLAYER_TTS_PLAYING, NULL, 0);
        break;
    case APP_PLAYER_EVENT_PAUSED:
        voice_msg_pub(VOICE_MSG_PLAYER_TTS_PAUSED, NULL, 0);
        break;
    case APP_PLAYER_EVENT_COMPLETED:
        s_tts_active = false;
        voice_msg_pub(VOICE_MSG_PLAYER_TTS_STOPED, NULL, 0);
        if (alarm_ring_is_active()) {
            alarm_ring_notify_playback_complete();
        }
        break;
    case APP_PLAYER_EVENT_STOPPED:
    case APP_PLAYER_EVENT_ERROR:
        s_tts_active = false;
        /* 始终发布 STOPED，on_tts_stoped 中通过 voice_intent_top() 判断是否 pop，
         * preemption 时 VOICE_SESSION 不在栈顶，不会被误弹出。 */
        voice_msg_pub(VOICE_MSG_PLAYER_TTS_STOPED, NULL, 0);
        break;
    default:
        break;
    }
}


static void on_tone_event(app_player_t *player, app_player_event_t event, void *user_data)
{
    /* FLUSH waits another hardware period even for an empty queue. Share
     * this callback's drain result so prompt completion does not mute the
     * beginning of the user's next word for a second, unnecessary period. */
    bool output_drained = voice_player_local_tone_event(event);
    if (s_current_tone_url == s_upload_cue_tag) voice_player_upload_cue_event(event);
    if (s_shutdown_prompt_in_progress) {
        if (s_shutdown_tone_playing) {
            if (event == APP_PLAYER_EVENT_COMPLETED) {
                if (!output_drained) voice_player_drain_wakeup_output();
                s_shutdown_tone_result = 0;
            } else if (event == APP_PLAYER_EVENT_STOPPED || event == APP_PLAYER_EVENT_ERROR) {
                s_shutdown_tone_result = -EIO;
            }
        }
        return; /* Old callbacks cannot start queued tones during shutdown. */
    }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    voice_player_boot_rgb_event(event);
#endif
    switch (event) {
    case APP_PLAYER_EVENT_COMPLETED: {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        uint32_t completed_offline_generation = s_current_offline_generation;
        if (s_current_local_request.local_generation &&
            s_current_local_request.token_index + 1U >= s_current_local_request.token_count &&
            !output_drained) {
            voice_player_drain_wakeup_output();
            output_drained = true;
        }
        if (voice_player_advance_local_sequence()) break;
        if (s_current_offline_generation) {
            if (!output_drained) {
                voice_player_drain_wakeup_output();
                output_drained = true;
            }
            s_current_offline_generation = 0;
        }
#endif
        bool completed_prompt = s_current_tone_prompt;
        uint32_t completed_wakeup_generation = s_current_tone_wakeup_generation;

        s_current_tone_prompt = false;
        s_current_tone_wakeup_generation = 0U;

        if (completed_wakeup_generation != 0U &&
            completed_wakeup_generation == s_latest_wakeup_gate_generation) {
            if (!output_drained) voice_player_drain_wakeup_output();
            voice_player_schedule_wakeup_gate_release(completed_wakeup_generation,
                                                      WAKEUP_UPLINK_TAIL_GUARD_MS,
                                                      "wakeup_tone_completed");
        }
        voice_player_start_next_queued_tone();
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (completed_offline_generation) {
            /* The acknowledgement has already reached the speaker. */
            voice_player_offline_wake_network_feedback(completed_offline_generation);
            service_offline_action_prompt_finished(completed_offline_generation, true);
        }
#endif

        if (completed_prompt && !s_current_tone_prompt) {
            voice_player_prompt_tone_exit();
        }
        if (alarm_ring_is_active()) {
            alarm_ring_notify_playback_complete();
        }
        break;
    }
    case APP_PLAYER_EVENT_STOPPED:
    case APP_PLAYER_EVENT_ERROR:
        voice_player_flush_tone_queue();
        break;
    default:
        break;
    }
}

void voice_player_ready(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    LISA_LOGI(TAG, "voice_player_ready");

    s_boot_tick = xTaskGetTickCount();

    /* 初始化交互意图栈（含 ebus 订阅 + 各 intent 钩子注册） */
    voice_intent_mgr_init();

    /* 初始化异步播放队列和任务 */
    if (async_play_init() != 0) {
        LOGE("async play init failed");
    }

    sys_network_status_t network_status = {0};
    bool wifi_mode = (sys_network_get_status(&network_status) == 0) &&
                     (network_status.mode == SYS_NETWORK_MODE_WIFI);

#ifndef CONFIG_BOARD_ARCS_MINI_V2_1
    if (wifi_mode && !sys_wifi_has_ap()) {
        voice_player_play_tone_url(app_tone_get_url(TONE_ID_70));
    }

    if (!s_boot_hello_played) {
        s_boot_hello_played = true;
        LOGI("boot hello tone request, tone_player=%p, url=%s", tone_player, app_tone_get_url(TONE_ID_58));
        voice_player_play_prompt_tone_url(app_tone_get_url(TONE_ID_58));
        LOGI("boot hello tone queued");
    }
#endif


    if (s_cloud_success_tone_timer == NULL) {
        s_cloud_success_tone_timer = xTimerCreate("cloud.success", pdMS_TO_TICKS(CLOUD_SUCCESS_TONE_DELAY_MS),
                                                  pdFALSE, NULL, cloud_success_tone_timer_cb);
        if (s_cloud_success_tone_timer == NULL) {
            LOGE("failed to create cloud success tone timer");
        }
    }

    if (s_wakeup_gate_timer == NULL) {
        s_wakeup_gate_timer = xTimerCreate("wake.gate", pdMS_TO_TICKS(WAKEUP_UPLINK_GATE_FAILSAFE_MS),
                                           pdFALSE, NULL, voice_player_wakeup_gate_timer_cb);
        if (s_wakeup_gate_timer == NULL) {
            LOGE("failed to create wakeup audio gate timer");
        }
    }

    // 提示音播放 控制 tone_player
    voice_msg_sub(VOICE_MSG_WAKEUP_BUTTON_START, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_WAKEUP_KEYWORD, voice_player_play_msg, NULL);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    voice_msg_sub(VOICE_MSG_OFFLINE_ACTION_PROMPT, voice_player_play_msg, NULL);
#endif
    voice_msg_sub(VOICE_MSG_CLOUD_CONNECTED, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_CLOUD_AUTH_SUCCESS, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_CLOUD_AUTH_FAILED, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_OPEN_INFO, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_DISCONNECTED, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_CONNECTED, voice_player_play_msg, NULL);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    voice_msg_sub(VOICE_MSG_OFFLINE_LOCAL_VOICE, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_OFFLINE_AUDIO_CANCEL, voice_player_play_msg, NULL);
#endif
    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_DISCONNECTED, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_BLE_AUTH_INFO_DONE, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_TTS_URL, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_PUSHUP_TTS_URL, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_SESSION_STARTING, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_IAT_UPDATE, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_CLOUD_MCP_CALL_RESP, voice_player_play_msg, NULL);
    voice_msg_sub(VOICE_MSG_BLE_CONNECT_DONE, voice_player_play_msg, NULL);

    // 播放歌曲（通过 mcp_tool_kuwo.c / mcp_tool_audio_url.c 工具）控制 music_player
    voice_msg_sub(VOICE_MSG_CLOUD_AUDIO_ITEM, voice_player_audio_item, NULL);

    // 用户主动切歌（通过 mcp_tool_play_ctrl.c 工具） 控制 music_player
    voice_msg_sub(VOICE_MSG_PLAY_CONTROL_PLAY, voice_player_play_control, NULL);
    voice_msg_sub(VOICE_MSG_PLAY_CONTROL_PAUSE, voice_player_play_control, NULL);
    voice_msg_sub(VOICE_MSG_PLAY_CONTROL_NEXT, voice_player_play_control, NULL);
    voice_msg_sub(VOICE_MSG_PLAY_CONTROL_PREVIOUS, voice_player_play_control, NULL);
    voice_msg_sub(VOICE_MSG_PLAY_CONTROL_REPLAY, voice_player_play_control, NULL);
    voice_msg_sub(VOICE_MSG_PLAY_CONTROL_STOP, voice_player_play_control, NULL);

    alarm_ring_init(voice_alarm_ring_play_once, voice_alarm_ring_force_stop);
    
    app_player_register_callback(tts_player, on_tts_event, NULL);
    app_player_register_callback(tone_player, on_tone_event, NULL);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!s_boot_hello_played) {
        const char *power_on_url = app_tone_get_url(TONE_ID_106);
        s_boot_hello_played = true;
        LOGI("power-on tone request: id=%u tone_player=%p url=%s",
             (unsigned)TONE_ID_106, tone_player,
             power_on_url ? power_on_url : "(null)");
        voice_player_play_tone_url(power_on_url);
    }
#endif
}

void voice_player_pcm_output(char *data, int size)
{
    /* Product attenuation is linear amplitude, separate from user volume.
     * Scale before the DAC write so software AEC sees the same reference. */
    lisa_device_t *dev = lisa_device_get("audio0");
    if (!data || size <= 0 || !dev || !lisa_device_ready(dev)) return;
    const int16_t *input = (const int16_t *)data;
    int remaining = size / sizeof(*input);
    int16_t output[256];
    while (remaining > 0) {
        int count = remaining > 256 ? 256 : remaining;
        for (int i = 0; i < count; ++i)
            output[i] = (int16_t)((int32_t)input[i] * 3 / 5);
        if (lisa_audio_play_write(dev, output, count) != count) {
            LOGE("Speaker PCM write failed");
            return;
        }
        input += count;
        remaining -= count;
    }
}

static int voice_player_init(void)
{
    LISA_LOGI(TAG, "voice_player_init");

    voice_msg_sub(VOICE_MSG_PLATFORM_READY, voice_player_ready, NULL);

    return 0;
}

SYS_INIT(voice_player_init, SYS_INIT_LEVEL_PRE_APPLICATION, 50);
