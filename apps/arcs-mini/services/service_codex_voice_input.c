#include "service_codex_voice_input.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "alarm_ring.h"
#include "app_netcfg_portal.h"
#include "app_tone.h"
#include "lingxin_adapter.h"
#include "lingxin_server.h"
#include "mbedtls/md.h"
#include "ota_manager.h"
#include "service_codex_status.h"
#include "service_offline_action.h"
#include "service_v2_voice_standby.h"
#include "service_volume.h"
#include "sysheap.h"
#include "tone.h"
#include "voice_player.h"

#define SAMPLE_RATE 16000U
#define MAX_SAMPLES (SAMPLE_RATE * 6U)
#define MIN_VOICED_SAMPLES (SAMPLE_RATE / 5U)
#define END_SILENCE_SAMPLES (SAMPLE_RATE * 3U / 4U)
#define CHUNK_BYTES 1024U
#define NONCE_BYTES 32U
#define PROOF_BYTES 64U
#define AGENT_PROMPT_TIMEOUT_MS 14000U
#define CUE_CACHE_BYTES (64U * 1024U)
#define CUE_CACHE_LIFETIME_MS (15U * 60U * 1000U)

typedef enum { IDLE, CONNECT, READY, CUE, RECORD, UPLOAD, RESULT, FEEDBACK } voice_state_t;
static voice_state_t s_state;
static bool s_requested;
static uint32_t s_started;
static uint32_t s_owner;
static char s_reply[sizeof("choice-ready ") + NONCE_BYTES + 1U + PROOF_BYTES];
static char s_nonce[NONCE_BYTES + 1U];
static int16_t *s_audio;
static uint32_t s_samples;
static uint32_t s_voiced;
static uint32_t s_silence;
static bool s_capturing;
static uint32_t s_sent;
static uint32_t s_wake_ms;
static uint32_t s_ready_ms;
static bool s_prompt_agent;
static bool s_prompt_cached;
static bool s_prompt_played;
static uint32_t s_prompt_generation;
static uint16_t s_prompt_tone;

typedef struct {
    uint8_t *bytes;
    uint32_t size;
    uint32_t saved_at;
    char url[64];
} cue_cache_t;
static cue_cache_t s_cue_cache[2];
static cue_cache_t *s_cue_capture;
static uint32_t s_cue_before;
static uint32_t s_cue_generation;
static bool s_cue_seen;
static bool s_cue_overflow;

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void set_state(voice_state_t state)
{
    s_state = state;
    s_started = now_ms();
}

bool service_codex_voice_route_wakeup(void)
{
    if (!service_codex_status_office_mode() || strlen(CONFIG_APP_CODEX_VOICE_KEY) != 64U) return false;
    taskENTER_CRITICAL();
    s_requested = true;
    taskEXIT_CRITICAL();
    service_v2_voice_standby_note_activity();
    return true;
}

void service_codex_voice_capture(const int16_t *samples, uint32_t count)
{
    taskENTER_CRITICAL();
    if (s_capturing && count && count <= MAX_SAMPLES - s_samples) {
        uint32_t magnitude = 0U;
        for (uint32_t i = 0; i < count; ++i) {
            int32_t sample = samples[i];
            magnitude += (uint32_t)(sample < 0 ? -sample : sample);
        }
        if (magnitude / count >= 180U) {
            s_voiced += count;
            s_silence = 0U;
        } else if (s_voiced >= MIN_VOICED_SAMPLES) {
            s_silence += count;
        }
        memcpy(s_audio + s_samples, samples, count * sizeof(*samples));
        s_samples += count;
        if (s_samples == MAX_SAMPLES || s_silence >= END_SILENCE_SAMPLES) s_capturing = false;
    }
    taskEXIT_CRITICAL();
}

void service_codex_voice_reply(const char *message, uint32_t length)
{
    if (length == 0U || length >= sizeof(s_reply)) return;
    taskENTER_CRITICAL();
    memcpy(s_reply, message, length);
    s_reply[length] = '\0';
    taskEXIT_CRITICAL();
}

static bool prompt(uint16_t tone)
{
    const char *url = app_tone_get_url(tone);
    return url && !service_volume_is_muted() && voice_player_try_play_prompt_tone_now_url(url);
}

void service_codex_voice_cache_chunk(uint32_t generation, const void *data, uint32_t size)
{
    if (!__atomic_load_n(&s_cue_capture, __ATOMIC_ACQUIRE)) return;
    taskENTER_CRITICAL();
    cue_cache_t *cache = __atomic_load_n(&s_cue_capture, __ATOMIC_RELAXED);
    if (cache && generation != s_cue_before && !s_cue_overflow) {
        if (!s_cue_seen) {
            s_cue_generation = generation;
            s_cue_seen = true;
        }
        if (generation != s_cue_generation || size > CUE_CACHE_BYTES - cache->size) {
            s_cue_overflow = true;
        } else {
            memcpy(cache->bytes + cache->size, data, size);
            cache->size += size;
        }
    }
    taskEXIT_CRITICAL();
}

static void finish_cue_cache(const lingxin_playback_stats_t *audio)
{
    taskENTER_CRITICAL();
    cue_cache_t *cache = __atomic_load_n(&s_cue_capture, __ATOMIC_RELAXED);
    __atomic_store_n(&s_cue_capture, NULL, __ATOMIC_RELEASE);
    bool valid = cache && audio && audio->generation == s_cue_generation &&
                 !audio->failed && !audio->suppressed && s_cue_seen &&
                 !s_cue_overflow && cache->size == audio->bytes && cache->size != 0U;
    taskEXIT_CRITICAL();
    if (!valid) return;
    snprintf(cache->url, sizeof(cache->url), "mem://addr=%usize=%u",
             (unsigned)(uintptr_t)cache->bytes, (unsigned)cache->size);
    cache->saved_at = now_ms();
}

static bool start_prompt(voice_state_t state, uint16_t tone, const char *text, bool allow_agent)
{
    s_prompt_agent = false;
    s_prompt_cached = false;
    s_prompt_played = false;
    s_prompt_tone = tone;
    if (allow_agent && !service_volume_is_muted() && !lingxin_server_chat_is_active() &&
        !voice_player_is_audio_active() && !voice_player_mode_command_blocked()) {
        lingxin_playback_stats_t audio;
        lingxin_audio_get_playback_stats(&audio);
        if (state == CUE) {
            cue_cache_t *cache = &s_cue_cache[tone == TONE_ID_139];
            if (cache->saved_at && now_ms() - cache->saved_at < CUE_CACHE_LIFETIME_MS &&
                voice_player_try_play_prompt_tone_now_url(cache->url)) {
                s_prompt_cached = true;
                set_state(state);
                return true;
            }
            if (!cache->bytes) cache->bytes = psram_malloc(CUE_CACHE_BYTES);
            if (cache->bytes) {
                taskENTER_CRITICAL();
                cache->saved_at = cache->size = 0U;
                s_cue_before = audio.generation;
                s_cue_seen = s_cue_overflow = false;
                __atomic_store_n(&s_cue_capture, cache, __ATOMIC_RELEASE);
                taskEXIT_CRITICAL();
            }
        }
        if (lingxin_server_chat_start_owned_text(text, s_owner) == 0) {
            s_prompt_agent = true;
            s_prompt_generation = audio.generation;
        } else {
            finish_cue_cache(NULL);
        }
    }
    if (!s_prompt_agent && !prompt(tone)) return false;
    set_state(state);
    return true;
}

/* A capture cue completes only after its playback and audio ownership end. */
static int poll_prompt(lisa_ws_t *ws, uint32_t elapsed)
{
    if (!s_prompt_agent) {
        if (elapsed >= 300U && !voice_player_is_audio_active()) {
            if (s_prompt_cached && ws) {
                (void)lisa_ws_send_text(ws, (const uint8_t *)"1 D agent input-cue-cached");
            }
            return 1;
        }
        return elapsed >= 2500U ? -1 : 0;
    }
    lingxin_playback_stats_t audio;
    lingxin_audio_get_playback_stats(&audio);
    if (!s_prompt_played && audio.generation != s_prompt_generation && audio.started &&
        audio.pcm_written && !audio.suppressed && !audio.failed) {
        s_prompt_played = true;
        if (ws) {
            const char *report = s_state == CUE ? "1 D agent input-cue-playing" :
                                                "1 D agent input-feedback-playing";
            (void)lisa_ws_send_text(ws, (const uint8_t *)report);
        }
    }
    if (!lingxin_server_chat_is_active() && !voice_player_is_audio_active() &&
        lingxin_audio_playback_input_ready()) {
        if (s_prompt_played && audio.ended) {
            finish_cue_cache(&audio);
            return 1;
        }
        if (elapsed >= 2000U) {
            finish_cue_cache(NULL);
            s_prompt_agent = false;
            if (!prompt(s_prompt_tone)) return -1;
            s_started = now_ms();
            return 0;
        }
    }
    return elapsed >= AGENT_PROMPT_TIMEOUT_MS ? -1 : 0;
}

static void reset(lisa_ws_t *ws, bool cancel)
{
    finish_cue_cache(NULL);
    taskENTER_CRITICAL();
    s_capturing = false;
    s_reply[0] = '\0';
    taskEXIT_CRITICAL();
    if (s_audio) psram_free(s_audio);
    s_audio = NULL;
    s_prompt_agent = false;
    if (cancel && ws && s_state >= READY && s_state <= RESULT) {
        (void)lisa_ws_send_text(ws, (const uint8_t *)"1 V cancel");
    }
    set_state(IDLE);
}

static void send_audio(lisa_ws_t *ws, uint32_t size)
{
    const uint8_t *data = (const uint8_t *)s_audio;
    for (unsigned i = 0; i < 16U && s_sent < size; ++i) {
        uint32_t remaining = size - s_sent;
        uint32_t bytes = remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES;
        if (lisa_ws_send_binary(ws, data + s_sent, bytes) != LISA_WS_OK) break;
        s_sent += bytes;
    }
}

static int hex_value(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return -1;
}

static bool sign(char proof[PROOF_BYTES + 1U], const char *prefix, size_t prefix_size,
                 const uint8_t *data, uint32_t size)
{
    const char *hex = CONFIG_APP_CODEX_VOICE_KEY;
    uint8_t key[32], digest[32];
    for (unsigned i = 0; i < sizeof(key); ++i) {
        int high = hex_value(hex[2U * i]);
        int low = hex_value(hex[2U * i + 1U]);
        if (high < 0 || low < 0) return false;
        key[i] = (uint8_t)((high << 4) | low);
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
              mbedtls_md_hmac_starts(&ctx, key, sizeof(key)) == 0 &&
              mbedtls_md_hmac_update(&ctx, (const uint8_t *)prefix, prefix_size) == 0 &&
              (size == 0U || mbedtls_md_hmac_update(&ctx, data, size) == 0) &&
              mbedtls_md_hmac_finish(&ctx, digest) == 0;
    mbedtls_md_free(&ctx);
    static const char digits[] = "0123456789abcdef";
    if (!ok) return false;
    for (unsigned i = 0; i < sizeof(digest); ++i) {
        proof[2U * i] = digits[digest[i] >> 4U];
        proof[2U * i + 1U] = digits[digest[i] & 15U];
    }
    proof[PROOF_BYTES] = '\0';
    return true;
}

static bool accept_ready(const char *reply, bool *choice)
{
    const size_t choice_prefix = sizeof("choice-ready ") - 1U;
    *choice = strncmp(reply, "choice-ready ", choice_prefix) == 0;
    const size_t start = *choice ? choice_prefix : sizeof("ready ") - 1U;
    const size_t prefix_size = start + NONCE_BYTES;
    if ((!*choice && strncmp(reply, "ready ", start)) ||
        strlen(reply) != prefix_size + 1U + PROOF_BYTES || reply[prefix_size] != ' ' ||
        strspn(reply + start, "0123456789abcdef") != NONCE_BYTES) return false;
    char expected[PROOF_BYTES + 1U];
    if (!sign(expected, reply, prefix_size, NULL, 0U)) return false;
    unsigned different = 0U;
    for (unsigned i = 0; i < PROOF_BYTES; ++i) different |= expected[i] ^ reply[prefix_size + 1U + i];
    if (different) return false;
    memcpy(s_nonce, reply + start, NONCE_BYTES);
    s_nonce[NONCE_BYTES] = '\0';
    return true;
}

bool service_codex_voice_poll(lisa_ws_t *ws)
{
    char reply[sizeof(s_reply)];
    taskENTER_CRITICAL();
    bool requested = s_requested;
    s_requested = false;
    memcpy(reply, s_reply, sizeof(reply));
    s_reply[0] = '\0';
    taskEXIT_CRITICAL();

    if (requested && s_state == IDLE && service_codex_status_office_mode() &&
        !voice_player_is_audio_active() && !lingxin_server_chat_is_active()) {
        s_owner = service_offline_action_generation();
        s_wake_ms = now_ms();
        set_state(CONNECT);
    }
    if (s_state == IDLE) return false;
    ota_state_e ota = ota_manager_get_state();
    bool owns_prompt = s_prompt_agent && (s_state == CUE || s_state == FEEDBACK);
    if (!service_codex_status_office_mode() || s_owner != service_offline_action_generation() ||
        (lingxin_server_chat_is_active() && !owns_prompt) || alarm_ring_is_active() ||
        !service_offline_action_background_allowed() || app_netcfg_portal_is_active() ||
        ota == OTA_STATE_CHECKING || ota == OTA_STATE_PACKAGE_INFO || ota == OTA_STATE_UPDATING) {
        reset(ws, true);
        return false;
    }

    uint32_t elapsed = now_ms() - s_started;
    bool fail = !ws && s_state != CONNECT && s_state != FEEDBACK;
    if (fail) {
        reset(ws, true);
        (void)prompt(TONE_ID_137);
        return false;
    }
    switch (s_state) {
    case CONNECT:
        if (ws && service_v2_voice_standby_get_state() == SERVICE_V2_VOICE_STANDBY_ACTIVE &&
            !voice_player_is_audio_active() &&
            lisa_ws_send_text(ws, (const uint8_t *)"1 V start") == LISA_WS_OK) {
            set_state(READY);
        } else if (elapsed >= 8000U) fail = true;
        break;
    case READY: {
        bool choice;
        if (accept_ready(reply, &choice)) {
            s_audio = psram_malloc(MAX_SAMPLES * sizeof(*s_audio));
            fail = !s_audio || !start_prompt(CUE, choice ? TONE_ID_139 : TONE_ID_136,
                     choice ? "请只朗读：请说选第几项。" : "请只朗读：请说下一步任务。", true);
        } else if (strcmp(reply, "wait-turn") == 0 || strcmp(reply, "no-target") == 0) {
            const char *text = reply[0] == 'w' ? "请只朗读：请等当前任务结束，再唤醒我。" :
                                                "请只朗读：请先在电脑上完成一轮任务，再唤醒我。";
            reset(ws, false);
            return start_prompt(FEEDBACK, TONE_ID_137, text, true);
        } else if (reply[0] || elapsed >= 2500U) fail = true;
        break;
    }
    case CUE: {
        int result = poll_prompt(ws, elapsed);
        if (result > 0) {
            taskENTER_CRITICAL();
            s_samples = s_voiced = s_silence = 0U;
            s_capturing = true;
            taskEXIT_CRITICAL();
            s_sent = 0U;
            s_ready_ms = now_ms() - s_wake_ms;
            set_state(RECORD);
            (void)lisa_ws_send_text(ws, (const uint8_t *)"1 D agent input-capture-ready");
        } else if (result < 0) fail = true;
        break;
    }
    case RECORD: {
        taskENTER_CRITICAL();
        if (elapsed >= 6200U) s_capturing = false;
        bool done = !s_capturing;
        uint32_t voiced = s_voiced;
        uint32_t captured = s_samples * sizeof(*s_audio);
        taskEXIT_CRITICAL();
        if (voice_player_is_audio_active()) fail = true;
        else if (done) {
            if (voiced < MIN_VOICED_SAMPLES) {
                reset(ws, true);
                return start_prompt(FEEDBACK, TONE_ID_137, "请只朗读：没听清，请再说一次。", true);
            }
            set_state(UPLOAD);
        } else if (voiced >= MIN_VOICED_SAMPLES) {
            send_audio(ws, captured - captured % CHUNK_BYTES);
        }
        break;
    }
    case UPLOAD: {
        uint32_t size = s_samples * sizeof(*s_audio);
        const uint8_t *data = (const uint8_t *)s_audio;
        send_audio(ws, size);
        if (s_sent == size) {
            char frame[8U + PROOF_BYTES + 1U] = "1 V end ";
            if (!sign(frame + 8U, s_nonce, NONCE_BYTES, data, size)) fail = true;
            else {
                char timing[40];
                snprintf(timing, sizeof(timing), "1 V timing %u %u",
                         (unsigned)s_ready_ms, (unsigned)elapsed);
                (void)lisa_ws_send_text(ws, (const uint8_t *)timing);
                if (lisa_ws_send_text(ws, (const uint8_t *)frame) != LISA_WS_OK) {
                    fail = elapsed >= 4000U;
                    break;
                }
                psram_free(s_audio);
                s_audio = NULL;
                set_state(RESULT);
                break;
            }
        }
        if (elapsed >= 4000U) fail = true;
        break;
    }
    case RESULT: {
        const char *error = NULL;
        if (strcmp(reply, "empty") == 0)
            error = "请只朗读：没听清，请再说一次。";
        else if (strcmp(reply, "wait-turn") == 0)
            error = "请只朗读：当前任务有变化，等任务结束后再唤醒我。";
        else if (strcmp(reply, "asr-error") == 0)
            error = "请只朗读：电脑语音识别暂时不可用，请检查助手。";
        else if (strcmp(reply, "submit-error") == 0)
            error = "请只朗读：任务没能送到，请检查电脑上的 Codex。";
        if (error) {
            reset(ws, false);
            return start_prompt(FEEDBACK, TONE_ID_137, error, true);
        }
        if (strcmp(reply, "answered") == 0 || strcmp(reply, "queued") == 0 || strcmp(reply, "retry") == 0) {
            uint16_t tone = strcmp(reply, "queued") == 0 ? TONE_ID_138 :
                            strcmp(reply, "answered") == 0 ? TONE_ID_140 : TONE_ID_141;
            const char *text = tone == TONE_ID_138 ? "请只朗读：指令已排队。" :
                               tone == TONE_ID_140 ? "请只朗读：已收到你的选择。" :
                                                     "请只朗读：没听清选第几项，请再唤醒重试。";
            reset(ws, false);
            return start_prompt(FEEDBACK, tone, text, true);
        }
        if (reply[0] || elapsed >= 21000U) fail = true;
        break;
    }
    case FEEDBACK:
        if (poll_prompt(ws, elapsed) != 0) reset(ws, false);
        break;
    default:
        break;
    }
    if (fail) {
        const char *text = s_state == CONNECT ? "请只朗读：电脑助手未连接。" :
                                                "请只朗读：指令没有提交，请再试一次。";
        reset(ws, true);
        return start_prompt(FEEDBACK, TONE_ID_137, text, true);
    }
    return s_state != IDLE;
}
