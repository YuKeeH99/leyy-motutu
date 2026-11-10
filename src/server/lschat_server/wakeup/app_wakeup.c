#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "lisa_device.h"
#include "lisa_audio.h"

#include "cJSON.h"
#include "voice_msg.h"
#include "acomp_wakeup.h"
#include "voice_cloud.h"
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
#include "lingxin_clean_recorder.h"
#endif
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
#include "service_codex_voice_input.h"
#endif
#include "app_wakeup_debug.h"
#include "app_wakeup.h"
#include "app_datas.h"
#include "sys_network_manager.h"
#include "sys_network_diag.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
#endif

#define TAG "app_wakeup"
#include "lisa_log.h"
#include "shell.h"
#ifdef CONFIG_BOARD_ARCS_MINI
#include "lisa_kv.h"
#include "kv_user.h"
#include "cmd/cmd.h"
#include "Driver_ADC_PDM.h"
#include "Driver_DAC.h"
#endif // CONFIG_BOARD_ARCS_MINI

/* Audio parameters */
#define SAMPLE_RATE         LISA_AUDIO_RATE_16K
#define SAMPLE_BITS         LISA_AUDIO_BIT_16


#define BUFFER_COUNT        12
#define BUFFER_SAMPLES      256

#if defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE) && defined(CONFIG_LISA_AUDIO_PLAY_SOFT_ECHO)
#error "Hardware AEC reference cannot use playback soft echo"
#endif
#if defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE) && defined(CONFIG_LISA_AUDIO_PLAY_ECHO_ENABLE)
#error "Hardware AEC reference cannot use DAC echo DMA"
#endif
#if defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE) && defined(CONFIG_AUDIO_TDOA_ENABLE)
#error "Hardware AEC reference cannot use MIC0/MIC1 TDOA"
#endif
#if defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE) && CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC
#error "Hardware AEC reference currently supports the single-microphone ACOMP input only"
#endif
#if defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE) && !defined(CONFIG_LISA_AUDIO_PLAY_ECHO_NONE)
#error "Hardware analog AEC reference requires playback echo collection to be disabled"
#endif
#if defined(CONFIG_AUDIO_AEC_REFERENCE_SOFTWARE) && !defined(CONFIG_LISA_AUDIO_PLAY_SOFT_ECHO)
#error "Software AEC reference requires playback soft echo"
#endif

#ifdef CONFIG_AUDIO_TDOA_ENABLE
/*
 * The estimator runs on synchronized 16 kHz MIC0/MIC1 ADC frames.  Search one
 * whole sample beyond the configured physical delay so the edge peak can be
 * interpolated.  The Kconfig range keeps this compile-time value <= 8.
 */
#define TDOA_SAMPLE_RATE_HZ       16000U
#define TDOA_SOUND_SPEED_MM_PER_S 343000U
#define TDOA_DISTANCE_SCALE       10U
#define TDOA_DELAY_DENOMINATOR    (TDOA_SOUND_SPEED_MM_PER_S * TDOA_DISTANCE_SCALE)
#define TDOA_SEARCH_LAG_SAMPLES                                                                                        \
    (((CONFIG_AUDIO_TDOA_MIC_SPACING_TENTH_MM * TDOA_SAMPLE_RATE_HZ + TDOA_DELAY_DENOMINATOR - 1U) /                   \
      TDOA_DELAY_DENOMINATOR) +                                                                                        \
     1U)
#define TDOA_MIN_MEAN_ABS               ((uint32_t)CONFIG_AUDIO_TDOA_MIN_MEAN_ABS)
#define TDOA_MIN_PEAK                   ((uint32_t)CONFIG_AUDIO_TDOA_MIN_PEAK)
#define TDOA_CLIP_PEAK                  32000U
#define TDOA_MIN_CORRELATION            ((double)CONFIG_AUDIO_TDOA_MIN_CORRELATION_MILLI / 1000.0)
#define TDOA_REPORT_BLOCKS              63U
#define TDOA_MIN_ACTIVE_BLOCKS          ((uint32_t)CONFIG_AUDIO_TDOA_MIN_ACTIVE_BLOCKS)
#define TDOA_LEVEL_DOMINANCE_MILLI      ((int32_t)CONFIG_AUDIO_TDOA_LEVEL_DOMINANCE_MILLI)
#define TDOA_ECHO_GATE_PEAK             32U
#define TDOA_ECHO_HOLD_BLOCKS           16U
#define TDOA_SIDE_THRESHOLD_SAMPLES     0.25
#define TDOA_DELAY_FILTER_WINDOWS       3U
#define TDOA_FILTER_RESET_WINDOWS       2U
#define TDOA_SIDE_SWITCH_WINDOWS        2U
#define TDOA_CENTER_CONFIRM_WINDOWS     3U
#define TDOA_FAST_CONFIRM_ACTIVE_BLOCKS 8U
#endif

#ifdef CONFIG_BOARD_ARCS_MINI
static bool s_wakeup_started = false;
#endif
static bool s_audio_inited = false;
static bool s_wakeup_feed_enabled = false;
static TickType_t s_last_wakeup_keyword_tick = 0;
/* Diagnostic counters only: no audio/credential payload is retained. */
static volatile uint32_t s_diag_input_blocks;
static volatile uint32_t s_diag_submit_ok;
static volatile uint32_t s_diag_submit_failed;
static volatile uint32_t s_diag_alloc_failed;
static volatile uint32_t s_diag_results;
static volatile uint32_t s_diag_keywords;
static volatile uint32_t s_diag_keyword_accepted;
static volatile uint32_t s_diag_keyword_throttled;
static volatile uint32_t s_diag_parse_failed;
static volatile int s_diag_mode = -1;
static int s_diag_prepare_ret = -1;
static int s_diag_start_ret = -1;
static int s_diag_threshold_ret = -1;
/* Opt-in, RAM-only acoustic probe. Never dispatches recognized events while
 * active; regular boot configuration and the online conversation are unchanged. */
static volatile bool s_probe_active;
static volatile bool s_probe_stop;
static uint32_t s_probe_seconds;
static volatile uint32_t s_probe_commands;
static volatile uint32_t s_probe_keywords;

bool app_wakeup_product_ready(void)
{
    return s_audio_inited && s_wakeup_feed_enabled;
}

lisa_device_t *g_audio_dev = NULL;
#ifdef CONFIG_BOARD_ARCS_MINI
static int32_t g_record_mic_analog_gain = CONFIG_AUDIO_DEFAULT_MIC_GAIN_A_DB;
static int32_t g_record_mic_digital_gain = CONFIG_AUDIO_DEFAULT_MIC_GAIN_D_DB;
static int32_t g_record_ref_analog_gain = CONFIG_AUDIO_DEFAULT_REF_GAIN_A_DB;
static int32_t g_record_ref_digital_gain = CONFIG_AUDIO_DEFAULT_REF_GAIN_D_DB;
static int32_t g_play_analog_gain = CONFIG_AUDIO_DEFAULT_SPK_GAIN_A_DB;
static int32_t g_play_digital_gain = CONFIG_AUDIO_DEFAULT_SPK_GAIN_D_DB;
#else  // !CONFIG_BOARD_ARCS_MINI
/* Record gain configuration - Default values */
static int32_t g_record_analog_gain = 36;      /* 30 dB */
static int32_t g_record_digital_gain = 0;       /* 0 dB */

/* Play gain configuration - Default values */
static int32_t g_play_analog_gain = 6;       /* 6 dB */
static int32_t g_play_digital_gain = -20;
#endif // CONFIG_BOARD_ARCS_MINI

static app_wakeup_sensitivity_level_e s_sensitivity_level = APP_WAKEUP_SENSITIVITY_LEVEL_2;

static uint32_t app_wakeup_get_esr_timeout_ms(void)
{
    int timeout_ms = 0;
#ifdef CONFIG_BOARD_ARCS_MINI
    if (lisa_kv_get_int(KV_KEY_IDLE_EXIT_TIMEOUT_MS, &timeout_ms) == 0 && timeout_ms > 0) {
        return (uint32_t)timeout_ms;
    }
#endif
    return (uint32_t)CONFIG_CLOUD_IDLE_EXIT_TIMEOUT_MS;
}

#define WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX (0)
#define WAKEUP_AUDIO_MIX2CH_STREAM_CH_CNAME "stream.mix2ch"
#define WAKEUP_AUDIO_OUT_STREAM_CH_INDEX (1)
#define WAKEUP_AUDIO_OUT_STREAM_CH_CNAME "stream.tocloud"
#define WAKEUP_AUDIO_OUT_STREAM    (1)
#define WAKEUP_AUDIO_OUT_STREAM_KICK_AUTO   (1)

/* 唤醒防抖：过滤同一次唤醒的重复回调，不用于限制正常连续唤醒 */
#define WAKEUP_KEYWORD_PUB_MIN_INTERVAL_MS  200

typedef struct {
    short mic1;
    short mic2;
}mic_in_t;

typedef struct {
    short ref;
}ref_in_t;

#if CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC
typedef struct {
    short mic1;
    short mic2;
    short ref1;
    short ref2;
}mic_ref_in_t;
#else
typedef struct {
    short mic1;
    short ref1;
}mic_ref_in_t;
#endif

static mic_ref_in_t *mic_ref_in = NULL;
#if !defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)
static const ref_in_t echo_zero[BUFFER_SAMPLES] = {0};
#endif
static SemaphoreHandle_t rx_sem;

#ifdef CONFIG_AUDIO_TDOA_ENABLE
static app_audio_localization_snapshot_t s_localization_snapshot;
static uint32_t s_localization_snapshot_sequence;
#endif

static int app_algo_command_extract(const uint8_t *const in, uint8_t *text, int max_len, bool *is_wakeup_keyword)
{
	int ret = -1;
	if (in && text && is_wakeup_keyword) {
		cJSON *root = cJSON_Parse(in);
		if (root) {
			cJSON *rlt_json = cJSON_GetObjectItem(root, "rlt");
			if (rlt_json) {
				if (cJSON_GetArraySize(rlt_json) > 0) {
					cJSON *rlt0_json = cJSON_GetArrayItem(rlt_json, 0);
					if (rlt0_json) {
						cJSON *kid_json = cJSON_GetObjectItem(rlt0_json, "iresid");
                        cJSON *bmain_json = cJSON_GetObjectItem(rlt0_json, "bMain");
                        cJSON *intent_json = cJSON_GetObjectItem(rlt0_json, "intent");
                        const char *intent_text = NULL;
                        /*兼容旧的版本协议*/
                        if (intent_json == NULL) {
                            intent_json = cJSON_GetObjectItem(rlt0_json, "intentStr");
                        }

                        if (intent_json && cJSON_IsString(intent_json)) {
                            intent_text = intent_json->valuestring;
                        } else {
                            cJSON *keyword_json = cJSON_GetObjectItem(rlt0_json, "keyword");
                            if (keyword_json && cJSON_IsString(keyword_json)) {
                                intent_text = keyword_json->valuestring;
                            }
                        }

						if (intent_text != NULL && max_len > 0 && strlen(intent_text) < (size_t)max_len) {
                            snprintf((char *)text, max_len, "%s", intent_text);
                            if (cJSON_IsBool(bmain_json) || (cJSON_IsNumber(bmain_json) &&
                                (bmain_json->valuedouble == 0.0 || bmain_json->valuedouble == 1.0))) {
                                *is_wakeup_keyword = (bmain_json->valueint == 1);
                                ret = 0;
                            } else if (bmain_json == NULL && cJSON_IsNumber(kid_json)) {
                                *is_wakeup_keyword = (kid_json->valueint == 0);
                                ret = 0;
                            }
						}
					}
				}
			}
			cJSON_Delete(root);
		}
	}

	return ret;
}

static void app_wakeup_trace_netcfg(const uint8_t *result)
{
    const char *names[] = {"istart", "iframe", "iduration"};
    uint32_t values[] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    unsigned valid = 0;
    cJSON *root = cJSON_Parse((const char *)result);
    cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "rlt"), 0);
    for (unsigned i = 0; i < 3; ++i) {
        cJSON *field = cJSON_GetObjectItem(item, names[i]);
        if (cJSON_IsNumber(field) && field->valuedouble >= 0.0 &&
            field->valuedouble <= (double)UINT32_MAX &&
            field->valuedouble == (double)(uint32_t)field->valuedouble) {
            values[i] = (uint32_t)field->valuedouble;
            valid |= 1U << i;
        }
    }
    cJSON_Delete(root);
    /* Raw AP integers only: their units/origin are not a CP clock contract.
     * value bits 0/1/2 validate start/frame/duration independently; missing or
     * invalid fields use UINT32_MAX. Do not reject inconsistent ordering here. */
    char detail[64];
    snprintf(detail, sizeof(detail), "istart=%lu iframe=%lu iduration=%lu",
             (unsigned long)values[0], (unsigned long)values[1], (unsigned long)values[2]);
    sys_network_diag_mark("wakeup", "netcfg_result", (int)valid, detail);
}

static bool app_wakeup_keyword_should_publish(const uint8_t *result, TickType_t now)
{
    static bool seen, last_position_valid;
    static uint32_t last_start;
    bool position_valid = false;
    uint32_t start = 0;
    cJSON *root = cJSON_Parse((const char *)result);
    cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "rlt"), 0);
    cJSON *first = cJSON_GetObjectItem(item, "istart");
    cJSON *current = cJSON_GetObjectItem(item, "iframe");
    /* These are AP source-frame positions, not CP callback timestamps.
     * Validate before conversion; old resources may omit either field. */
    if (cJSON_IsNumber(first) && cJSON_IsNumber(current) &&
        first->valuedouble >= 0.0 && first->valuedouble <= (double)UINT32_MAX &&
        current->valuedouble >= first->valuedouble && current->valuedouble <= (double)UINT32_MAX) {
        start = (uint32_t)first->valuedouble;
        position_valid = first->valuedouble == (double)start &&
                         current->valuedouble == (double)(uint32_t)current->valuedouble;
    }
    cJSON_Delete(root);
    /* RPC delivery can bunch up distinct spoken keywords. Only suppress the
     * same source detection in that window; iframe may advance on a repeat
     * callback, so istart is the identity. Keep the legacy bounded fallback
     * without positions, and permit reused positions after an AP restart. */
    if (seen && (TickType_t)(now - s_last_wakeup_keyword_tick) <
                    pdMS_TO_TICKS(WAKEUP_KEYWORD_PUB_MIN_INTERVAL_MS) &&
        (!position_valid || !last_position_valid || start == last_start)) return false;
    seen = true;
    last_position_valid = position_valid;
    last_start = start;
    s_last_wakeup_keyword_tick = now;
    return true;
}

static void wakeup_event_handler(uint32_t event, void *event_data, uint32_t event_data_len, void *priv)
{
    uint8_t keyword[64] = {0};
    bool is_wakeup_keyword = false;
    int ret;

    if (event & WAKEUP_CB_EVENT_ENGINE_RLT) {
        ++s_diag_results;
        LISA_LOGI(TAG, "wakeup result:%d,%s", event_data_len, (char *)event_data);
        ret = app_algo_command_extract(event_data, keyword, sizeof(keyword), &is_wakeup_keyword);
        if (s_probe_active) {
            if (ret == 0) {
                if (is_wakeup_keyword) ++s_probe_keywords;
                else ++s_probe_commands;
                LISA_LOGI(TAG, "[OFFLINE_PROBE] recognized type=%s value=%s (no dispatch)",
                          is_wakeup_keyword ? "wakeup" : "command", keyword);
            } else {
                LISA_LOGW(TAG, "[OFFLINE_PROBE] parse failed, inspect raw wakeup result");
            }
            return;
        }
        if (ret == 0) {
            if (is_wakeup_keyword) {
                ++s_diag_keywords;
                TickType_t now = xTaskGetTickCount();
                if (app_wakeup_keyword_should_publish(event_data, now)) {
                    ++s_diag_keyword_accepted;
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
                    if (service_codex_voice_route_wakeup()) return;
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                    if (service_offline_action_handle_result((char *)keyword, true)) return;
#endif
                    LISA_LOGI(TAG, "publish VOICE_MSG_WAKEUP_KEYWORD keyword=%s", keyword);
                    voice_msg_pub(VOICE_MSG_WAKEUP_KEYWORD, keyword, strlen((char *)keyword) + 1);
                } else {
                    ++s_diag_keyword_throttled;
                    LISA_LOGD(TAG, "wakeup keyword throttled, last:%u now:%u",
                              (unsigned int)s_last_wakeup_keyword_tick, (unsigned int)now);
                }
            }
            else {
                if (strcmp((const char *)keyword, "jin4 ru4 pei4 wang3 mo2 shi4") == 0)
                    app_wakeup_trace_netcfg(event_data);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                if (service_offline_action_handle_result((char *)keyword, false)) return;
#endif
                LISA_LOGI(TAG, "publish VOICE_MSG_WAKEUP_COMMAND keyword=%s", keyword);
                voice_msg_pub(VOICE_MSG_WAKEUP_COMMAND, keyword, strlen((char *)keyword) + 1);
            }
        } else {
            ++s_diag_parse_failed;
        }
    } else if (event & WAKEUP_CB_EVENT_STREAM_UPDATE) {
        // LISA_LOGI(TAG, "wakeup stream update data:%p, len:%d", event_data, event_data_len);
        
#ifdef WAKEUP_AUDIO_OUT_STREAM_KICK_AUTO
        /* 自动触发模式：发送信号量通知接收线程 */
        if (rx_sem != NULL) {
            // BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            // xSemaphoreGiveFromISR(rx_sem, &xHigherPriorityTaskWoken);
            // portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

            xSemaphoreGive(rx_sem);
        }
#endif

    } else if (event & WAKEUP_CB_EVENT_ENGINE_TIMEOUT) {
        LISA_LOGI(TAG, "wakeup timeout");
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_offline_action_engine_idle();
#endif

    } else if (event & WAKEUP_CB_EVENT_ENGINE_ANGLE) {
        for (int i = 0; i < event_data_len/sizeof(short); i++) {
            short angle = *((short*)event_data + i);
            LISA_LOGD(TAG, "wakeup angle[%d]: %d", i, angle);
        }
    } else if(event & WAKEUP_CB_EVENT_ENGINE_SWITCH_MODE){
        acomp_wakeup_algo_mode_e mode = *((acomp_wakeup_algo_mode_e*)event_data);
        s_diag_mode = (int)mode;
        if (s_probe_active) {
            LISA_LOGI(TAG, "[OFFLINE_PROBE] algorithm mode=%d (no lifecycle dispatch)", (int)mode);
            return;
        }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (mode == ACOMP_WAKEUP_ALGO_MODE_WAKEUP) service_offline_action_engine_idle();
        return; /* Product worker owns the local lifecycle; never finish cloud via ESR. */
#endif
        static acomp_wakeup_algo_mode_e prev_mode = -1;
        if(mode != prev_mode){
            if(mode == ACOMP_WAKEUP_ALGO_MODE_WAKEUP){
                voice_msg_pub(VOICE_MSG_WAKEUP_RECOGNIZED_FINISH, NULL, 0);
            }
            else{
                voice_msg_pub(VOICE_MSG_WAKEUP_RECOGNIZED_START, NULL, 0);
            }
            LISA_LOGI(TAG, "wakeup switch mode to:%d", mode);
            prev_mode = mode;}
    }
    else {
        LISA_LOGW(TAG, "unknown wakeup event:0X%X", event);
    }
}

static void ref_data_scale(ref_in_t *ref_data, int sample_cnt, int db)
{

	double gain = pow(10.0, (double)(db / 20.0));
	int16_t *sample = (int16_t *)ref_data;
	for(uint32_t i = 0; i  < sample_cnt; i++){	
		int32_t temp = sample[i] * gain;
		// 处理溢出
        if (temp > SHRT_MAX) {
            sample[i] = SHRT_MAX;
        } else if (temp < SHRT_MIN) {
            sample[i] = SHRT_MIN;
        } else {
			sample[i] = (int16_t)temp;
		}
	}
}

static void stream_data_fusion(mic_ref_in_t *algo_buf_in, const mic_in_t *adc_data, const ref_in_t *ref_data,
                               uint32_t sample_cnt)
{
    for (uint32_t i = 0; i < sample_cnt; i++) {
#if defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)
        /* ADC left is the real microphone; ADC right is analog speaker feedback. */
        algo_buf_in[i].mic1 = adc_data[i].mic1;
        algo_buf_in[i].ref1 = adc_data[i].mic2;
#elif CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC
        /* Two real microphones share the completed playback PCM reference. */
        algo_buf_in[i].mic1 = adc_data[i].mic1;
        algo_buf_in[i].mic2 = adc_data[i].mic2;
        algo_buf_in[i].ref1 = ref_data[i].ref;
        algo_buf_in[i].ref2 = ref_data[i].ref;
#else
        /* Single real microphone plus completed playback PCM reference. */
        algo_buf_in[i].mic1 = adc_data[i].mic1;
        algo_buf_in[i].ref1 = ref_data[i].ref;
#endif
    }
#ifdef CONFIG_AUDIO_AEC_REFERENCE_HARDWARE
    (void)ref_data;
#endif
}

#ifdef CONFIG_AUDIO_TDOA_ENABLE
typedef struct {
    uint32_t block_count;
    uint32_t active_block_count;
    uint32_t sample_count;
    uint64_t mic0_abs_sum;
    uint64_t mic1_abs_sum;
    uint32_t mic0_peak;
    uint32_t mic1_peak;
    int32_t polarity_score;
    double best_candidate_correlation;
    double weighted_delay_sum;
    double weight_sum;
    double correlation_sum;
} tdoa_diag_state_t;

typedef struct {
    double delay[TDOA_DELAY_FILTER_WINDOWS];
    uint8_t count;
    uint8_t next;
    uint8_t inactive_windows;
} tdoa_delay_filter_t;

typedef struct {
    app_audio_localization_side_e stable;
    app_audio_localization_side_e pending;
    uint8_t pending_windows;
    uint8_t center_windows;
} tdoa_side_filter_t;

static tdoa_delay_filter_t s_tdoa_delay_filter;
static tdoa_side_filter_t s_tdoa_side_filter;

static void tdoa_delay_filter_reset(void)
{
    memset(&s_tdoa_delay_filter, 0, sizeof(s_tdoa_delay_filter));
}

static void tdoa_side_filter_reset(void)
{
    memset(&s_tdoa_side_filter, 0, sizeof(s_tdoa_side_filter));
}

static void tdoa_delay_filter_note_inactive(void)
{
    if (s_tdoa_delay_filter.inactive_windows < UINT8_MAX) {
        ++s_tdoa_delay_filter.inactive_windows;
    }
    if (s_tdoa_delay_filter.inactive_windows >= TDOA_FILTER_RESET_WINDOWS) {
        tdoa_delay_filter_reset();
        tdoa_side_filter_reset();
    }
}

/*
 * A three-window median rejects one reflected/out-of-sign report without
 * altering the underlying block estimator.  With only two history entries,
 * their mean deliberately resolves opposing signs to CENTER until a majority
 * exists.  Silence clears the history after two report windows.
 */
static double tdoa_delay_filter_push(double delay)
{
    s_tdoa_delay_filter.delay[s_tdoa_delay_filter.next] = delay;
    s_tdoa_delay_filter.next = (uint8_t)((s_tdoa_delay_filter.next + 1U) % TDOA_DELAY_FILTER_WINDOWS);
    if (s_tdoa_delay_filter.count < TDOA_DELAY_FILTER_WINDOWS) {
        ++s_tdoa_delay_filter.count;
    }
    s_tdoa_delay_filter.inactive_windows = 0U;

    if (s_tdoa_delay_filter.count == 1U) {
        return s_tdoa_delay_filter.delay[0];
    }
    if (s_tdoa_delay_filter.count == 2U) {
        return (s_tdoa_delay_filter.delay[0] + s_tdoa_delay_filter.delay[1]) / 2.0;
    }

    double a = s_tdoa_delay_filter.delay[0];
    double b = s_tdoa_delay_filter.delay[1];
    double c = s_tdoa_delay_filter.delay[2];
    if (a > b) {
        double tmp = a;
        a = b;
        b = tmp;
    }
    if (b > c) {
        b = c;
    }
    if (a > b) {
        b = a;
    }
    return b;
}

/*
 * Keep a confirmed side through short zero-delay/reflection windows.  A true
 * CENTER must persist for three valid reports, while an opposite side must
 * persist for two.  The inactive-window path resets this state between sound
 * events, so a new source is not biased by the previous one.
 */
static app_audio_localization_side_e tdoa_side_filter_apply(app_audio_localization_side_e candidate, bool fast_confirm)
{
    if (s_tdoa_side_filter.stable == APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN) {
        if (candidate != APP_AUDIO_LOCALIZATION_SIDE_CENTER && fast_confirm) {
            s_tdoa_side_filter.stable = candidate;
            s_tdoa_side_filter.pending = APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN;
            s_tdoa_side_filter.pending_windows = 0U;
            return candidate;
        }

        if (candidate != s_tdoa_side_filter.pending) {
            s_tdoa_side_filter.pending = candidate;
            s_tdoa_side_filter.pending_windows = 1U;
        } else if (s_tdoa_side_filter.pending_windows < UINT8_MAX) {
            ++s_tdoa_side_filter.pending_windows;
        }
        if (s_tdoa_side_filter.pending_windows >= TDOA_SIDE_SWITCH_WINDOWS) {
            s_tdoa_side_filter.stable = candidate;
            s_tdoa_side_filter.pending = APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN;
            s_tdoa_side_filter.pending_windows = 0U;
            return candidate;
        }
        return APP_AUDIO_LOCALIZATION_SIDE_CENTER;
    }

    if (candidate == s_tdoa_side_filter.stable) {
        s_tdoa_side_filter.pending = APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN;
        s_tdoa_side_filter.pending_windows = 0U;
        s_tdoa_side_filter.center_windows = 0U;
        return candidate;
    }

    if (candidate != APP_AUDIO_LOCALIZATION_SIDE_CENTER && fast_confirm) {
        s_tdoa_side_filter.stable = candidate;
        s_tdoa_side_filter.pending = APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN;
        s_tdoa_side_filter.pending_windows = 0U;
        s_tdoa_side_filter.center_windows = 0U;
        return candidate;
    }

    if (candidate == APP_AUDIO_LOCALIZATION_SIDE_CENTER) {
        s_tdoa_side_filter.pending = APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN;
        s_tdoa_side_filter.pending_windows = 0U;
        if (s_tdoa_side_filter.center_windows < UINT8_MAX) {
            ++s_tdoa_side_filter.center_windows;
        }
        if (s_tdoa_side_filter.center_windows >= TDOA_CENTER_CONFIRM_WINDOWS) {
            s_tdoa_side_filter.stable = APP_AUDIO_LOCALIZATION_SIDE_CENTER;
            s_tdoa_side_filter.center_windows = 0U;
        }
        return s_tdoa_side_filter.stable;
    }

    s_tdoa_side_filter.center_windows = 0U;
    if (s_tdoa_side_filter.stable == APP_AUDIO_LOCALIZATION_SIDE_CENTER) {
        s_tdoa_side_filter.stable = candidate;
        s_tdoa_side_filter.pending = APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN;
        s_tdoa_side_filter.pending_windows = 0U;
        return candidate;
    }

    if (candidate != s_tdoa_side_filter.pending) {
        s_tdoa_side_filter.pending = candidate;
        s_tdoa_side_filter.pending_windows = 1U;
    } else if (s_tdoa_side_filter.pending_windows < UINT8_MAX) {
        ++s_tdoa_side_filter.pending_windows;
    }

    if (s_tdoa_side_filter.pending_windows >= TDOA_SIDE_SWITCH_WINDOWS) {
        s_tdoa_side_filter.stable = candidate;
        s_tdoa_side_filter.pending = APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN;
        s_tdoa_side_filter.pending_windows = 0U;
    }
    return s_tdoa_side_filter.stable;
}

static const char *tdoa_side_name(app_audio_localization_side_e side)
{
    switch (side) {
    case APP_AUDIO_LOCALIZATION_SIDE_MIC0:
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        return "LEFT";
#else
        return "MIC0";
#endif
    case APP_AUDIO_LOCALIZATION_SIDE_CENTER:
        return "CENTER";
    case APP_AUDIO_LOCALIZATION_SIDE_MIC1:
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        return "FRONT";
#else
        return "MIC1";
#endif
    case APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static void tdoa_publish_snapshot(bool valid, bool echo_gated, app_audio_localization_side_e side,
                                  int32_t delay_milli_samples, int16_t axis_angle_deg, uint16_t correlation_milli,
                                  uint16_t active_blocks, uint16_t total_blocks, uint16_t mic0_mean_abs,
                                  uint16_t mic1_mean_abs, uint16_t mic0_peak, uint16_t mic1_peak,
                                  int16_t level_balance_milli, bool level_fused)
{
    uint32_t sequence = __atomic_load_n(&s_localization_snapshot_sequence, __ATOMIC_RELAXED);
    app_audio_localization_snapshot_t next = {
        .sequence = sequence + 2U,
        .timestamp_ms = (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS),
        .delay_milli_samples = delay_milli_samples,
        .axis_angle_deg = axis_angle_deg,
        .correlation_milli = correlation_milli,
        .active_blocks = active_blocks,
        .total_blocks = total_blocks,
        .mic0_mean_abs = mic0_mean_abs,
        .mic1_mean_abs = mic1_mean_abs,
        .mic0_peak = mic0_peak,
        .mic1_peak = mic1_peak,
        .level_balance_milli = level_balance_milli,
        .side = side,
        .valid = valid,
        .echo_gated = echo_gated,
        .level_fused = level_fused,
    };

    /* Sequence lock: odd means a writer is replacing the snapshot. */
    __atomic_store_n(&s_localization_snapshot_sequence, sequence + 1U, __ATOMIC_RELEASE);
    s_localization_snapshot = next;
    __atomic_store_n(&s_localization_snapshot_sequence, sequence + 2U, __ATOMIC_RELEASE);
}

bool app_audio_localization_get_snapshot(app_audio_localization_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    for (uint32_t attempt = 0; attempt < 4U; ++attempt) {
        uint32_t before = __atomic_load_n(&s_localization_snapshot_sequence, __ATOMIC_ACQUIRE);
        if ((before & 1U) != 0U) {
            continue;
        }

        *snapshot = s_localization_snapshot;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t after = __atomic_load_n(&s_localization_snapshot_sequence, __ATOMIC_ACQUIRE);
        if (before == after && (after & 1U) == 0U) {
            snapshot->sequence = after;
            return true;
        }
    }

    memset(snapshot, 0, sizeof(*snapshot));
    return false;
}

/*
 * Normalized short-lag correlation of first differences.  Positive delay
 * means MIC0 received the sound first; negative delay means MIC1 received it
 * first.  It is an array-axis estimate and cannot resolve the front/back
 * broadside ambiguity by itself.
 */
static bool tdoa_estimate_block(const mic_in_t *samples, uint32_t frame_count, double *delay_samples,
                                double *correlation, int8_t *polarity)
{
    int64_t cross[2U * TDOA_SEARCH_LAG_SAMPLES + 1U] = {0};
    uint64_t mic1_energy[2U * TDOA_SEARCH_LAG_SAMPLES + 1U] = {0};
    uint64_t mic0_energy = 0;
    uint64_t abs_sum = 0;
    uint32_t peak = 0;
    const uint32_t first = TDOA_SEARCH_LAG_SAMPLES + 1U;
    const uint32_t last = frame_count - TDOA_SEARCH_LAG_SAMPLES;

    if (samples == NULL || delay_samples == NULL || correlation == NULL || polarity == NULL ||
        frame_count <= 2U * TDOA_SEARCH_LAG_SAMPLES + 2U) {
        return false;
    }

    for (uint32_t i = 0; i < frame_count; ++i) {
        int32_t mic0 = samples[i].mic1;
        int32_t mic1 = samples[i].mic2;
        uint32_t mic0_abs = mic0 < 0 ? (uint32_t)-mic0 : (uint32_t)mic0;
        uint32_t mic1_abs = mic1 < 0 ? (uint32_t)-mic1 : (uint32_t)mic1;

        abs_sum += mic0_abs + mic1_abs;
        if (mic0_abs > peak) {
            peak = mic0_abs;
        }
        if (mic1_abs > peak) {
            peak = mic1_abs;
        }
    }

    if (abs_sum / (2U * frame_count) < TDOA_MIN_MEAN_ABS || peak < TDOA_MIN_PEAK || peak >= TDOA_CLIP_PEAK) {
        return false;
    }

    for (uint32_t i = first; i < last; ++i) {
        int32_t mic0_diff = (int32_t)samples[i].mic1 - (int32_t)samples[i - 1U].mic1;
        mic0_energy += (uint64_t)((int64_t)mic0_diff * mic0_diff);

        for (int32_t lag = -(int32_t)TDOA_SEARCH_LAG_SAMPLES; lag <= (int32_t)TDOA_SEARCH_LAG_SAMPLES; ++lag) {
            uint32_t j = (uint32_t)((int32_t)i + lag);
            int32_t mic1_diff = (int32_t)samples[j].mic2 - (int32_t)samples[j - 1U].mic2;
            uint32_t index = (uint32_t)(lag + (int32_t)TDOA_SEARCH_LAG_SAMPLES);

            cross[index] += (int64_t)mic0_diff * mic1_diff;
            mic1_energy[index] += (uint64_t)((int64_t)mic1_diff * mic1_diff);
        }
    }

    uint32_t best_index = 0;
    for (uint32_t i = 1U; i < 2U * TDOA_SEARCH_LAG_SAMPLES + 1U; ++i) {
        uint64_t candidate = cross[i] < 0 ? (uint64_t)-cross[i] : (uint64_t)cross[i];
        uint64_t current = cross[best_index] < 0 ? (uint64_t)-cross[best_index] : (uint64_t)cross[best_index];
        if (candidate > current) {
            best_index = i;
        }
    }

    if (best_index == 0U || best_index == 2U * TDOA_SEARCH_LAG_SAMPLES || cross[best_index] == 0 || mic0_energy == 0U ||
        mic1_energy[best_index] == 0U) {
        return false;
    }

    double signed_corr = (double)cross[best_index] / sqrt((double)mic0_energy * (double)mic1_energy[best_index]);
    double corr = signed_corr < 0.0 ? -signed_corr : signed_corr;

    double left = cross[best_index - 1U] < 0 ? -(double)cross[best_index - 1U] : (double)cross[best_index - 1U];
    double center = cross[best_index] < 0 ? -(double)cross[best_index] : (double)cross[best_index];
    double right = cross[best_index + 1U] < 0 ? -(double)cross[best_index + 1U] : (double)cross[best_index + 1U];
    double denominator = left - 2.0 * center + right;
    double fraction = 0.0;

    if (denominator < -1.0 || denominator > 1.0) {
        fraction = 0.5 * (left - right) / denominator;
        if (fraction < -0.5) {
            fraction = -0.5;
        } else if (fraction > 0.5) {
            fraction = 0.5;
        }
    }

    *delay_samples = (double)((int32_t)best_index - (int32_t)TDOA_SEARCH_LAG_SAMPLES) + fraction;
    *correlation = corr;
    *polarity = signed_corr < 0.0 ? -1 : 1;
    if (corr < TDOA_MIN_CORRELATION) {
        return false;
    }
    return true;
}

static void tdoa_diag_process(const mic_in_t *samples, uint32_t frame_count, bool echo_active)
{
    static tdoa_diag_state_t state;
    static uint32_t echo_hold_blocks;
    double delay_samples = 0.0;
    double correlation = 0.0;
    int8_t polarity = 0;

    if (echo_active) {
        echo_hold_blocks = TDOA_ECHO_HOLD_BLOCKS;
        tdoa_delay_filter_reset();
        tdoa_side_filter_reset();
    }
    if (echo_hold_blocks > 0U) {
        static uint32_t echo_report_blocks;
        static bool echo_gated;

        memset(&state, 0, sizeof(state));
        ++echo_report_blocks;
        if (!echo_gated || echo_report_blocks >= TDOA_REPORT_BLOCKS) {
            tdoa_publish_snapshot(false, true, APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  false);
            echo_gated = true;
            echo_report_blocks = 0U;
        }
        --echo_hold_blocks;
        if (echo_hold_blocks == 0U && !echo_active) {
            echo_gated = false;
            echo_report_blocks = 0U;
            tdoa_publish_snapshot(false, false, APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  false);
        }
        return;
    }

    ++state.block_count;
    for (uint32_t i = 0; i < frame_count; ++i) {
        int32_t mic0 = samples[i].mic1;
        int32_t mic1 = samples[i].mic2;
        uint32_t mic0_abs = mic0 < 0 ? (uint32_t)-mic0 : (uint32_t)mic0;
        uint32_t mic1_abs = mic1 < 0 ? (uint32_t)-mic1 : (uint32_t)mic1;

        state.mic0_abs_sum += mic0_abs;
        state.mic1_abs_sum += mic1_abs;
        if (mic0_abs > state.mic0_peak) {
            state.mic0_peak = mic0_abs;
        }
        if (mic1_abs > state.mic1_peak) {
            state.mic1_peak = mic1_abs;
        }
    }
    state.sample_count += frame_count;

    bool block_valid = tdoa_estimate_block(samples, frame_count, &delay_samples, &correlation, &polarity);
    if (correlation > state.best_candidate_correlation) {
        state.best_candidate_correlation = correlation;
    }
    if (block_valid) {
        double weight = correlation * correlation;
        state.weighted_delay_sum += delay_samples * weight;
        state.weight_sum += weight;
        state.correlation_sum += correlation;
        state.polarity_score += polarity;
        ++state.active_block_count;
    }

    if (state.block_count < TDOA_REPORT_BLOCKS) {
        return;
    }

    uint16_t mic0_mean_abs = state.sample_count > 0U ? (uint16_t)(state.mic0_abs_sum / state.sample_count) : 0U;
    uint16_t mic1_mean_abs = state.sample_count > 0U ? (uint16_t)(state.mic1_abs_sum / state.sample_count) : 0U;
    uint16_t mic0_peak = (uint16_t)state.mic0_peak;
    uint16_t mic1_peak = (uint16_t)state.mic1_peak;
    uint16_t best_candidate_correlation_milli = (uint16_t)(state.best_candidate_correlation * 1000.0 + 0.5);

    if (state.active_block_count >= TDOA_MIN_ACTIVE_BLOCKS && state.weight_sum > 0.0) {
        double raw_delay = state.weighted_delay_sum / state.weight_sum;
        double delay = tdoa_delay_filter_push(raw_delay);
        double max_delay = ((double)CONFIG_AUDIO_TDOA_MIC_SPACING_TENTH_MM * (double)TDOA_SAMPLE_RATE_HZ) /
                           (double)TDOA_DELAY_DENOMINATOR;
        double ratio = max_delay > 0.0 ? delay / max_delay : 0.0;
        app_audio_localization_side_e candidate = APP_AUDIO_LOCALIZATION_SIDE_CENTER;

        if (ratio < -1.0) {
            ratio = -1.0;
        } else if (ratio > 1.0) {
            ratio = 1.0;
        }

        double angle = asin(ratio) * (180.0 / 3.14159265358979323846);
        if (delay < -TDOA_SIDE_THRESHOLD_SAMPLES) {
            candidate = APP_AUDIO_LOCALIZATION_SIDE_MIC1;
        } else if (delay > TDOA_SIDE_THRESHOLD_SAMPLES) {
            candidate = APP_AUDIO_LOCALIZATION_SIDE_MIC0;
        }

        int32_t level_sum = (int32_t)mic0_mean_abs + (int32_t)mic1_mean_abs;
        int32_t level_balance_milli =
            level_sum > 0 ? (((int32_t)mic0_mean_abs - (int32_t)mic1_mean_abs) * 1000) / level_sum : 0;
        int32_t level_balance_abs = level_balance_milli < 0 ? -level_balance_milli : level_balance_milli;
        bool level_fused = TDOA_LEVEL_DOMINANCE_MILLI > 0 && level_balance_abs >= TDOA_LEVEL_DOMINANCE_MILLI;
        if (level_fused) {
            candidate = level_balance_milli > 0 ? APP_AUDIO_LOCALIZATION_SIDE_MIC0 : APP_AUDIO_LOCALIZATION_SIDE_MIC1;
        }
        bool fast_confirm = level_fused || state.active_block_count >= TDOA_FAST_CONFIRM_ACTIVE_BLOCKS;
        app_audio_localization_side_e side = tdoa_side_filter_apply(candidate, fast_confirm);

        int32_t delay_milli = (int32_t)(delay * 1000.0 + (delay >= 0.0 ? 0.5 : -0.5));
        int32_t raw_delay_milli = (int32_t)(raw_delay * 1000.0 + (raw_delay >= 0.0 ? 0.5 : -0.5));
        int16_t angle_deg = (int16_t)(angle + (angle >= 0.0 ? 0.5 : -0.5));
        uint16_t correlation_milli =
            (uint16_t)(state.correlation_sum * 1000.0 / (double)state.active_block_count + 0.5);

        tdoa_publish_snapshot(true, false, side, delay_milli, angle_deg, correlation_milli,
                              (uint16_t)state.active_block_count, (uint16_t)state.block_count, mic0_mean_abs,
                              mic1_mean_abs, mic0_peak, mic1_peak, (int16_t)level_balance_milli, level_fused);
        LISA_LOGI(TAG,
                  "tdoa: side=%s axis_angle=%d deg delay_milli=%ld "
                  "candidate=%s raw_delay_milli=%ld corr_milli=%u "
                  "active=%lu/%lu level_balance_milli=%ld fused=%u "
                  "polarity=%s level_mean=%u/%u level_peak=%u/%u",
                  tdoa_side_name(side), angle_deg, (long)delay_milli, tdoa_side_name(candidate), (long)raw_delay_milli,
                  correlation_milli, (unsigned long)state.active_block_count, (unsigned long)state.block_count,
                  (long)level_balance_milli, level_fused ? 1U : 0U, state.polarity_score < 0 ? "inverted" : "normal",
                  mic0_mean_abs, mic1_mean_abs, mic0_peak, mic1_peak);
    } else {
        tdoa_delay_filter_note_inactive();
        tdoa_publish_snapshot(false, false, APP_AUDIO_LOCALIZATION_SIDE_UNKNOWN, 0, 0, best_candidate_correlation_milli,
                              (uint16_t)state.active_block_count, (uint16_t)state.block_count, mic0_mean_abs,
                              mic1_mean_abs, mic0_peak, mic1_peak, 0, false);
        LISA_LOGI(TAG,
                  "tdoa: no coherent speech active=%lu/%lu "
                  "best_corr_milli=%u level_mean=%u/%u level_peak=%u/%u",
                  (unsigned long)state.active_block_count, (unsigned long)state.block_count,
                  best_candidate_correlation_milli, mic0_mean_abs, mic1_mean_abs, mic0_peak, mic1_peak);
    }

    memset(&state, 0, sizeof(state));
}
#else
bool app_audio_localization_get_snapshot(app_audio_localization_snapshot_t *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return false;
}
#endif

static void audio_stream_callback(const lisa_audio_event_t *event, void *user_data)
{
    uint8_t *buffer;
    uint32_t buf_size;
    uint16_t desc_idx;
#if !defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)
    const ref_in_t *algo_ref_data = echo_zero;
    ref_in_t partial_echo[BUFFER_SAMPLES] = {0};
    static bool echo_fallback_reported;
#endif

    (void)user_data;

    if (event == NULL || !s_wakeup_feed_enabled) {
        return;
    }

    bool has_record = event->record_buffer != NULL && event->record_samples > 0U;
#if !defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)
    bool has_echo = event->echo_buffer != NULL && event->echo_samples > 0U;
#endif
    if (!has_record || (event->record_samples & 1U) != 0U) {
        LISA_LOGE(LOG_TAG, "Invalid stereo audio event: record_samples=%lu", (unsigned long)event->record_samples);
        return;
    }

    uint32_t record_frames = event->record_samples / 2U;
    if (record_frames < BUFFER_SAMPLES) {
        LISA_LOGE(LOG_TAG, "Short stereo audio event: frames=%lu expected=%u", (unsigned long)record_frames,
                  BUFFER_SAMPLES);
        return;
    }
    ++s_diag_input_blocks;

#if !defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)
    if (has_echo) {
        uint32_t copy_samples = event->echo_samples < BUFFER_SAMPLES ? event->echo_samples : BUFFER_SAMPLES;
        if (copy_samples == BUFFER_SAMPLES) {
            algo_ref_data = (const ref_in_t *)event->echo_buffer;
        } else {
            memcpy(partial_echo, event->echo_buffer, copy_samples * sizeof(partial_echo[0]));
            algo_ref_data = partial_echo;
        }
    }

    if (!has_echo || event->echo_samples < BUFFER_SAMPLES) {
        if (!echo_fallback_reported) {
            LISA_LOGW(TAG, "soft echo unavailable/short (%lu), using zero reference",
                      (unsigned long)event->echo_samples);
            echo_fallback_reported = true;
        }
    } else {
        echo_fallback_reported = false;
    }
#endif

    buffer = acomp_wakeup_stream_tx_buffer_alloc(WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX, &buf_size, &desc_idx);

    if (buffer != NULL && buf_size > 0U) {
        uint32_t required_size = BUFFER_SAMPLES * sizeof(mic_ref_in_t);
        memset(buffer, 0, buf_size);
        if (buf_size >= required_size) {
#if !defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)
            stream_data_fusion((mic_ref_in_t *)buffer, (const mic_in_t *)event->record_buffer, algo_ref_data,
                               BUFFER_SAMPLES);
#else
            stream_data_fusion((mic_ref_in_t *)buffer, (const mic_in_t *)event->record_buffer, NULL, BUFFER_SAMPLES);
#endif
        } else {
            LISA_LOGE(TAG, "ACOMP input buffer short: %lu < %lu", (unsigned long)buf_size,
                      (unsigned long)required_size);
        }

        int submit_ret =
            acomp_wakeup_stream_tx_buffer_submit(WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX, buffer, buf_size, desc_idx);
        if (submit_ret != ACOMP_ERR_OK) {
            ++s_diag_submit_failed;
            LISA_LOGE(TAG, "ACOMP input submit failed: %d", submit_ret);
        } else {
            ++s_diag_submit_ok;
        }
    } else {
        ++s_diag_alloc_failed;
        LOGE("acomp_wakeup_stream_tx_buffer_alloc failed");
    }

#ifdef CONFIG_AUDIO_TDOA_ENABLE
    uint32_t echo_block_peak = 0U;
    if (has_echo) {
        const ref_in_t *echo_samples = (const ref_in_t *)event->echo_buffer;
        uint32_t count = event->echo_samples < BUFFER_SAMPLES ? event->echo_samples : BUFFER_SAMPLES;
        for (uint32_t i = 0; i < count; ++i) {
            int32_t echo = echo_samples[i].ref;
            uint32_t echo_abs = echo < 0 ? (uint32_t)-echo : (uint32_t)echo;
            if (echo_abs > echo_block_peak) {
                echo_block_peak = echo_abs;
            }
        }
    }
    tdoa_diag_process((const mic_in_t *)event->record_buffer, BUFFER_SAMPLES, echo_block_peak >= TDOA_ECHO_GATE_PEAK);
#endif
}

/**
 * @brief Configure and start recording
 */
static int start_recording(lisa_device_t *audio_dev)
{
    int ret;

    /* Configure recording parameters */
    lisa_audio_record_config_t record_config = {
        .format = {
            .sample_rate = SAMPLE_RATE,
            .channels = LISA_AUDIO_CH_STEREO,
            .sample_bits = SAMPLE_BITS,
        },
#ifdef CONFIG_BOARD_ARCS_MINI
        .gain_l = {
            .analog_gain = g_record_mic_analog_gain,
            .digital_gain = g_record_mic_digital_gain,
        },
        .gain_r = {
            .analog_gain = g_record_ref_analog_gain,
            .digital_gain = g_record_ref_digital_gain,
        },
#else // !CONFIG_BOARD_ARCS_MINI
        .gain = {
            .analog_gain = g_record_analog_gain,
            .digital_gain = g_record_digital_gain,
        },
#endif // CONFIG_BOARD_ARCS_MINI
        .differential_input = true,
        .enable_hpf = true,

    };

    ret = lisa_audio_record_config(audio_dev, &record_config);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(LOG_TAG, "Record config failed: %d", ret);
        return ret;
    }

    /* Start recording */
    ret = lisa_audio_record_start(audio_dev);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(LOG_TAG, "Record start failed: %d", ret);
        return ret;
    }

    LISA_LOGI(LOG_TAG, "Recording started");
    return LISA_DEVICE_OK;
}


/**
 * @brief Configure and start playback
 */
static int start_playback(lisa_device_t *audio_dev)
{
    int ret;

    /* Configure playback parameters */
    lisa_audio_play_config_t play_config = {
        .format = {
            .sample_rate = SAMPLE_RATE,
            .channels = LISA_AUDIO_CH_LEFT,
            .sample_bits = SAMPLE_BITS,
        },
        .gain = {
            .analog_gain = g_play_analog_gain,
            .digital_gain = g_play_digital_gain,
        },
        .buffer_count = BUFFER_COUNT,
        .buffer_samples = BUFFER_SAMPLES,
    };

    ret = lisa_audio_play_config(audio_dev, &play_config);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(LOG_TAG, "Play config failed: %d", ret);
        return ret;
    }

    /* Start playback */
    ret = lisa_audio_play_start(audio_dev);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(LOG_TAG, "Play start failed: %d", ret);
        return ret;
    }

#ifdef CONFIG_LISA_AUDIO_PLAY_SOFT_ECHO
    LISA_LOGI(LOG_TAG, "Playback started (software Echo reference enabled)");
#elif defined(CONFIG_LISA_AUDIO_PLAY_ECHO_ENABLE)
    LISA_LOGI(LOG_TAG, "Playback started (Echo collection enabled)");
#else
    LISA_LOGI(LOG_TAG, "Playback started");
#endif

    return LISA_DEVICE_OK;
}

static int audio_init(void)
{
    int ret;
    lisa_device_t *audio_dev = NULL;

    if (s_audio_inited) {
        return 0;
    }

#if defined(CONFIG_AUDIO_AEC_REFERENCE_HARDWARE)
    LISA_LOGI(TAG, "ACOMP input: 2-channel (MIC0, ADC-right hard-ref)");
    LISA_LOGI(TAG, "AEC reference: hardware analog feedback");
#elif CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC
    LISA_LOGI(TAG, "ACOMP input: 4-channel (MIC0, MIC1, ref0, ref1)");
    LISA_LOGI(TAG, "AEC reference: software playback PCM");
#else
    LISA_LOGI(TAG, "ACOMP input: 2-channel (MIC0, playback soft-ref)");
    LISA_LOGI(TAG, "AEC reference: software playback PCM");
#endif
#ifdef CONFIG_AUDIO_TDOA_ENABLE
    LISA_LOGI(TAG, "Raw TDOA enabled: MIC0+MIC1 spacing=%d.%dmm search=+/-%u samples",
              CONFIG_AUDIO_TDOA_MIC_SPACING_TENTH_MM / 10, CONFIG_AUDIO_TDOA_MIC_SPACING_TENTH_MM % 10,
              (unsigned int)TDOA_SEARCH_LAG_SAMPLES);
#endif

    /* Initialize audio device */
    
    GPDMA_Initialize();

    /* Get audio device */
    audio_dev = lisa_device_get("audio0");
    if (!audio_dev) {
        LISA_LOGE(TAG, "Failed to get audio device");
        return NULL;
    }
    LISA_LOGI(TAG, "Audio device acquired");

 
#ifdef CONFIG_BOARD_ARCS_MINI
    int gain_val = 0;
    if (lisa_kv_get_int(KV_KEY_USER_MIC_GAIN_DB, &gain_val) == 0) {
        if (gain_val < ADC_PDM_GAIN_A_MIN_DB) gain_val = ADC_PDM_GAIN_A_MIN_DB;
        if (gain_val > ADC_PDM_GAIN_A_MAX_DB) gain_val = ADC_PDM_GAIN_A_MAX_DB;
        g_record_mic_analog_gain = gain_val;
    }
    if (lisa_kv_get_int(KV_KEY_USER_AEC_GAIN_DB, &gain_val) == 0) {
        if (gain_val < ADC_PDM_GAIN_A_MIN_DB) gain_val = ADC_PDM_GAIN_A_MIN_DB;
        if (gain_val > ADC_PDM_GAIN_A_MAX_DB) gain_val = ADC_PDM_GAIN_A_MAX_DB;
        g_record_ref_analog_gain = gain_val;
    }
    if (lisa_kv_get_int(KV_KEY_USER_SPK_GAIN_DB, &gain_val) == 0) {
        if (gain_val < DAC_GAIN_A_MIN_DB) gain_val = DAC_GAIN_A_MIN_DB;
        if (gain_val > DAC_GAIN_A_MAX_DB) gain_val = DAC_GAIN_A_MAX_DB;
        g_play_analog_gain = gain_val;
    }
#endif // CONFIG_BOARD_ARCS_MINI

    /* Start recording hardware (but data collection controlled by flag) */
    if (start_recording(audio_dev) != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "Failed to start recording");
        lisa_audio_unregister_callback(audio_dev, audio_stream_callback);
        return -2;
    }

    /* Start playback */
    if (start_playback(audio_dev) != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "Failed to start playback");
        return -3;
    }
    /* 设置相位补偿 */
    lisa_audio_phase_compensation_t phase_comp = {
        .record_skip_samples = 0,
        .echo_skip_samples = 0,
    };

    ret = lisa_audio_set_phase_compensation(audio_dev, &phase_comp);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "设置相位补偿失败: %d", ret);
        return ret;
    }
    g_audio_dev = audio_dev;
    s_audio_inited = true;
    LISA_LOGI(TAG, "相位补偿已设置");

    LISA_LOGI(TAG, "System running...");
    LISA_LOGI(TAG, "Use 'start_record' and 'stop_record' commands to control");

   
    return 0;
}

static void wakeup_in_task(void *pvParameters)
{
    (void)pvParameters;

    GPDMA_Initialize();
    audio_init();
    while(1){
        vTaskDelay(pdMS_TO_TICKS(5));
    }
#if 0
    lite_dac_init();
    dac_aud_t aud = {.rate=16000};
    lite_dac_ctrl(ADAC_CTRL_AUD_CFG, &aud);

    dac_gain_t gain_dac = {.a_gain=6, .d_gain=-20};
    lite_dac_ctrl(ADAC_CTRL_VOLUME, &gain_dac);

    lite_adc_init();
    struct {
        int al;
        int ar;
        int dl;
        int dr;
    } gain = {36,36, 0,0};
    lite_adc_ctrl(MAPI_AADC_CTRL_SET_GAIN, &gain);
    
    lite_dac_ctrl(ADAC_CTRL_START, NULL);
    lite_adc_ctrl(MAPI_AADC_CTRL_REC_START, NULL);

    while (1) {
#if CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC
        if (first) {
            ret = lite_dac_get_echo_buf((uint16_t **)&ref_data, portMAX_DELAY);
            first = 0;
        }
#endif
        ret = lite_adc_read(&adc_data, CONFIG_AUDIO_STEP_SAMPS * sizeof(mic_in_t), portMAX_DELAY);
        if (ret == 0) {
            LISA_LOGE(TAG, "wakeup stream lite_adc_read failed");
            continue;
        }
#if CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC
        ret = lite_dac_get_echo_buf((uint16_t **)&ref_data, portMAX_DELAY);
       if (ret != CONFIG_AUDIO_STEP_SAMPS) {
            ref_data = (ref_in_t *)echo_zero;
        }
#endif
        buffer = acomp_wakeup_stream_tx_buffer_alloc(WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX, &buf_size, &desc_idx);

        if(buffer && buf_size > 0){
            stream_data_fusion((mic_ref_in_t*)buffer, adc_data, ref_data, CONFIG_AUDIO_STEP_SAMPS);
            acomp_wakeup_stream_tx_buffer_submit(WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX, buffer, buf_size, desc_idx);
        } else {
            LOGE("acomp_wakeup_stream_tx_buffer_alloc failed");
        }
    }
#endif
}
#if WAKEUP_AUDIO_OUT_STREAM
static void wakeup_out_stream_to_cloud(uint8_t *data, int len)
{
#define UAS_REC_FRM_SAMPS         256
#define UAS_REC_CHANNELS          5
#define LS_RECORD_REC_CHANNEL_NUM (UAS_REC_CHANNELS - 1)
#define LS_RECORD_ONE_CHNNEL_SIZE (UAS_REC_FRM_SAMPS * sizeof(short))

    for (int j = 0; j < len / (LS_RECORD_ONE_CHNNEL_SIZE * UAS_REC_CHANNELS); j++) {
        short algo_out[UAS_REC_FRM_SAMPS] = {0};
        short(*algo_frame)[UAS_REC_CHANNELS] =
            (short(*)[UAS_REC_CHANNELS])((uint8_t *)data + LS_RECORD_ONE_CHNNEL_SIZE * UAS_REC_CHANNELS * j);
        for (int i = 0; i < UAS_REC_FRM_SAMPS; i++) {
            algo_out[i] = algo_frame[i][3];
        }

#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
        /* Send AP AEC output at unity gain. ADC right is not a microphone in
         * the single-mic product; raw audio0 must not bypass this path. */
        for (int i = 0; i < UAS_REC_FRM_SAMPS; i++) algo_frame[i][4] = algo_out[i];
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
        service_codex_voice_capture(algo_out, UAS_REC_FRM_SAMPS);
#endif
        lingxin_audio_capture_clean(algo_out, UAS_REC_FRM_SAMPS);
#else
        /* Apply +6dB gain (x2) to clean audio before sending to cloud ASR */
        for (int i = 0; i < UAS_REC_FRM_SAMPS; i++) {
            int32_t sample = (int32_t)algo_out[i] << 1;
            if (sample > SHRT_MAX) {
                sample = SHRT_MAX;
            } else if (sample < SHRT_MIN) {
                sample = SHRT_MIN;
            }
            if (sample > -CONFIG_LSCHAT_WAKEUP_CLOUD_NOISE_GATE_THRESHOLD &&
                sample < CONFIG_LSCHAT_WAKEUP_CLOUD_NOISE_GATE_THRESHOLD) {
                sample = 0;
            }
            algo_frame[i][4] = (short)sample; // 保存上报云端的数据，后续UAC传出
            algo_out[i] = (short)sample;
        }

        voice_cloud_chat_send_audio((uint8_t *)algo_out, LS_RECORD_ONE_CHNNEL_SIZE);
#endif
    }
}

static void wakeup_out_task(void *pvParameters)
{
    uint8_t *buffer;
    uint32_t len;
    uint16_t desc_idx;
    int ret;

    while (1) {

#ifdef WAKEUP_AUDIO_OUT_STREAM_KICK_AUTO
        /* 自动触发模式：等待来自回调的信号量 */
        xSemaphoreTake(rx_sem, pdMS_TO_TICKS(50));
#else
        /* 手动触发模式：等待超时时间 */
        vTaskDelay(pdMS_TO_TICKS(5));
#endif
        /* 从 R2M 流中获取数据 */
        while (1) {
            buffer = acomp_wakeup_stream_rx_buffer_get(WAKEUP_AUDIO_OUT_STREAM_CH_INDEX, &len, &desc_idx);
            if (buffer != NULL && len > 0) {

                LISA_LOGD(TAG, "acomp_wakeup_stream_rx_buffer_get ok");

                wakeup_out_stream_to_cloud(buffer, len);

                wakeup_stream_debug_data_output(buffer, len);

                /* 释放缓冲区 */
                ret = acomp_wakeup_stream_rx_buffer_release(WAKEUP_AUDIO_OUT_STREAM_CH_INDEX, desc_idx, len, buffer);
                if (ret != ACOMP_ERR_OK) {
                    LISA_LOGW(TAG, "Failed to release buffer: %d", ret);
                }
            } else {
                /* 没有更多数据，跳出内层循环 */
                break;
            }
        }
    }

    vTaskDelete(NULL);
}
#endif
int app_wakeup_init(struct wakeup_algo_resources *algo_res)
{
    int ret = 0;
    bool wakeup_res_valid = false;

    ret = audio_init();
    if (ret != 0) {
        LISA_LOGE(TAG, "Audio init failed: %d", ret);
        return ret;
    }

    wakeup_res_valid = (algo_res != NULL && algo_res->mlp.addr != NULL && algo_res->mlp.size > 0 &&
                        algo_res->wrap.addr != NULL && algo_res->wrap.size > 0);
    if (!wakeup_res_valid) {
        LISA_LOGW(TAG, "Wakeup resources invalid, skip wakeup engine init");
        s_wakeup_feed_enabled = false;
#if CONFIG_BOARD_ARCS_MINI
        s_wakeup_started = false;
#endif
        return 0;
    }

    acomp_stream_chn_create_desc_t desc = {
        .cname = WAKEUP_AUDIO_MIX2CH_STREAM_CH_CNAME,
        .direction = ACOMP_STREAM_DIRECTION_M2R,
        .index = WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX,
#if CONFIG_ACOMP_WAKEUP_ALGORITHM_TYPE_DUAL_MIC
        .buffer_size = 256 * 4 * 2,
#else
        .buffer_size = 256 * 2 * 2,
#endif
        .num_descs = 4,
        .kick_policy = 1,
    };
#if WAKEUP_AUDIO_OUT_STREAM
    acomp_stream_chn_create_desc_t rx_desc = {
        .cname = WAKEUP_AUDIO_OUT_STREAM_CH_CNAME,
        .direction = ACOMP_STREAM_DIRECTION_R2M,
        .index = WAKEUP_AUDIO_OUT_STREAM_CH_INDEX,
        .buffer_size = 15360,
        .num_descs = 8,
#if WAKEUP_AUDIO_OUT_STREAM_KICK_AUTO
        .kick_policy = 1,  /* 手动触发 */
#else
        .kick_policy = 0,  /* 手动触发 */
#endif
    };
#if WAKEUP_AUDIO_OUT_STREAM_KICK_AUTO
    rx_sem = xSemaphoreCreateBinary();
    if (rx_sem == NULL) {
        LISA_LOGE(TAG, "Failed to create rx semaphore");
    }
#endif
#endif
    acomp_wakeup_init();
    acomp_wakeup_add_callback(    WAKEUP_CB_EVENT_ENGINE_RLT 
                                | WAKEUP_CB_EVENT_STREAM_UPDATE 
                                | WAKEUP_CB_EVENT_ENGINE_TIMEOUT 
                                | WAKEUP_CB_EVENT_ENGINE_ANGLE
                                | WAKEUP_CB_EVENT_ENGINE_SWITCH_MODE,
                           wakeup_event_handler, NULL);

    const acomp_wakeup_resource_config_t wakeup_config = {
        .mlp = {
            .storage = ACOMP_WAKEUP_RES_STORAGE_FLASH,
            .addr = (uintptr_t)algo_res->mlp.addr,
            .size = algo_res->mlp.size,
        },
        .wrap = {
            .storage = ACOMP_WAKEUP_RES_STORAGE_FLASH,
            .addr = (uintptr_t)algo_res->wrap.addr,
            .size = algo_res->wrap.size,
        },
    };

    ret = acomp_wakeup_prepare_with_resources(&wakeup_config);
    s_diag_prepare_ret = ret;
    LISA_LOGI(TAG, "acomp_wakeup_prepare ret:%d", ret);
    if (ret != ACOMP_ERR_OK) {
        return ret;
    }
    ret = acomp_wakeup_start();
    s_diag_start_ret = ret;
    LISA_LOGI(TAG, "acomp_wakeup_start ret:%d", ret);
    if (ret != ACOMP_ERR_OK) {
        return ret;
    }

    ret = acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_WAKEUP);
    LISA_LOGI(TAG, "acomp_wakeup_set_algo_mode ret:%d", ret);
    if (ret != ACOMP_ERR_OK) {
        (void)acomp_wakeup_stop();
        return ret;
    }
    s_diag_mode = ACOMP_WAKEUP_ALGO_MODE_WAKEUP;

    // TODO: arcs-mini 2.6版本使用
    // ret = acomp_wakeup_set_timeout(app_wakeup_get_esr_timeout_ms()); 
    ret = acomp_wakeup_set_timeout(0);
    LISA_LOGI(TAG, "acomp_wakeup_set_timeout ret:%d\r\n", ret);
    if (ret != ACOMP_ERR_OK) {
        (void)acomp_wakeup_stop();
        return ret;
    }

    acomp_wakeup_stream_ch_enable(WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX,&desc);
#if WAKEUP_AUDIO_OUT_STREAM
    acomp_wakeup_stream_ch_enable(WAKEUP_AUDIO_OUT_STREAM_CH_INDEX,&rx_desc);
#endif
    mic_ref_in = (mic_ref_in_t *)psram_malloc(sizeof(mic_ref_in_t) * CONFIG_AUDIO_STEP_SAMPS);
    if (mic_ref_in == NULL) {
        LISA_LOGE(TAG, "mic_ref_in psram_malloc_align failed");
        return -1;
    }

#if WAKEUP_AUDIO_OUT_STREAM
    if (xTaskCreate(wakeup_out_task, "wakeup_out_task", 4096, NULL, 9, NULL) != pdPASS) {
        LISA_LOGE(TAG, "Failed to create wakeup output task");
        return -1;
    }
#endif

    lisa_device_t *audio_dev = NULL;

    /* Get audio device */
    audio_dev = lisa_device_get("audio0");
    if (!audio_dev) {
        LISA_LOGE(TAG, "Failed to get audio device");
        return -1;
    }
    LISA_LOGI(TAG, "Audio device acquired");
 
    /* Register unified callback */
    ret = lisa_audio_register_callback(audio_dev, audio_stream_callback, NULL);
    if (ret != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "Failed to register callback: %d", ret);
        return -1;
    }
    LISA_LOGI(TAG, "Audio callback registered");

    s_wakeup_feed_enabled = true;

#if CONFIG_BOARD_ARCS_MINI
    s_wakeup_started = true;
#endif

    return ret;
}

#ifdef CONFIG_BOARD_ARCS_MINI
int app_wakeup_stop(void)
{
    if (!s_wakeup_started) {
        return 0;
    }

    s_wakeup_feed_enabled = false;

    (void)acomp_wakeup_stream_ch_disable(WAKEUP_AUDIO_MIX2CH_STREAM_CH_INDEX);
#if WAKEUP_AUDIO_OUT_STREAM
    (void)acomp_wakeup_stream_ch_disable(WAKEUP_AUDIO_OUT_STREAM_CH_INDEX);
#endif
    (void)acomp_wakeup_stop();
    s_wakeup_started = false;

    return 0;
}
#endif

int app_wakeup_sensitivity_level_set(app_wakeup_sensitivity_level_e level){

    static const int level_remap[] = {
        [APP_WAKEUP_SENSITIVITY_LEVEL_0] = ACOMP_WAKEUP_THRESHOLD_LEVEL_1,
        [APP_WAKEUP_SENSITIVITY_LEVEL_1] = ACOMP_WAKEUP_THRESHOLD_LEVEL_2,
        [APP_WAKEUP_SENSITIVITY_LEVEL_2] = ACOMP_WAKEUP_THRESHOLD_LEVEL_3,
        [APP_WAKEUP_SENSITIVITY_LEVEL_3] = ACOMP_WAKEUP_THRESHOLD_LEVEL_4,
        [APP_WAKEUP_SENSITIVITY_LEVEL_4] = ACOMP_WAKEUP_THRESHOLD_LEVEL_5,
    };

    if ((unsigned int)level >= sizeof(level_remap) / sizeof(level_remap[0])) {
        return ACOMP_ERR_INVALID_ARG;
    }
    int ret = acomp_wakeup_set_threshold(level_remap[level]);
    s_diag_threshold_ret = ret;
    if (ret == ACOMP_ERR_OK) {
        s_sensitivity_level = level;
    }
    LISA_LOGI(TAG,"app_wakeup_sensitivity_level_set %d ret=%d", level, ret);
    return ret;
}

app_wakeup_sensitivity_level_e app_wakeup_sensitivity_level_get(void){

    return s_sensitivity_level;
}

static void wakeup_probe_task(void *arg)
{
    (void)arg;
    int timeout_ret = acomp_wakeup_set_timeout(s_probe_seconds * 1000U);
    /* Enter ESR through a real keyword. On the shipped AP, forcing ESR directly
     * emitted mode=1 then an immediate timeout, without returning the control ACK.
     * A nonzero timeout arms the documented keyword -> ESR path instead. */
    if (timeout_ret == ACOMP_ERR_OK) {
        LISA_LOGI(TAG, "[OFFLINE_PROBE] READY seconds=%u; say wakeup keyword, then command; no prompt or action",
                  (unsigned)s_probe_seconds);
        TickType_t start = xTaskGetTickCount();
        while (!s_probe_stop && xTaskGetTickCount() - start < pdMS_TO_TICKS(s_probe_seconds * 1000U)) {
            sys_network_status_t status = {0};
            if (sys_network_get_status(&status) != 0 || status.connected || status.wifi_connected) {
                LISA_LOGW(TAG, "[OFFLINE_PROBE] network changed, ending probe");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else {
        LISA_LOGW(TAG, "[OFFLINE_PROBE] start failed timeout_ret=%d", timeout_ret);
    }
    int restore_timeout = acomp_wakeup_set_timeout(0);
    int restore_mode = acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_WAKEUP);
    LISA_LOGI(TAG, "[OFFLINE_PROBE] END commands=%u keywords=%u restore_timeout=%d restore_mode=%d",
              (unsigned)s_probe_commands, (unsigned)s_probe_keywords, restore_timeout, restore_mode);
    /* On AP control failure keep suppressing callbacks, rather than leaking a
     * diagnostic command into application handlers. A reboot restores defaults. */
    if (restore_timeout == ACOMP_ERR_OK && restore_mode == ACOMP_ERR_OK) s_probe_active = false;
    else LISA_LOGW(TAG, "[OFFLINE_PROBE] restore failed; reboot required before normal voice use");
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    service_offline_action_diagnostic_release(!s_probe_active);
#endif
    vTaskDelete(NULL);
}

static int shell_wakeup_probe(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        shellPrint(shellGetCurrent(), "OFFLINE_PROBE active=%d commands=%u keywords=%u mode=%d\n",
                   s_probe_active, (unsigned)s_probe_commands, (unsigned)s_probe_keywords, s_diag_mode);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        s_probe_stop = true;
        return 0;
    }
    char *end = NULL;
    unsigned long seconds = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc != 2 || end == argv[1] || *end != '\0' || seconds < 5 || seconds > 60) {
        shellPrint(shellGetCurrent(), "Usage: wakeup_probe <5..60 seconds|status|stop>; offline recognition only\n");
        return -1;
    }
    sys_network_status_t status = {0};
    if (!s_audio_inited || !s_wakeup_feed_enabled || s_diag_mode != ACOMP_WAKEUP_ALGO_MODE_WAKEUP ||
        sys_network_get_status(&status) != 0 || status.connected || status.wifi_connected) {
        shellPrint(shellGetCurrent(), "OFFLINE_PROBE rejected: require active microphone, wakeup mode and Wi-Fi disconnected\n");
        return -1;
    }
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_diagnostic_acquire()) return -1;
#endif
    taskENTER_CRITICAL();
    bool already_active = s_probe_active;
    if (!already_active) {
        s_probe_active = true;
        s_probe_stop = false;
        s_probe_seconds = (uint32_t)seconds;
        s_probe_commands = s_probe_keywords = 0;
    }
    taskEXIT_CRITICAL();
    if (already_active) return -1;
    if (xTaskCreate(wakeup_probe_task, "wakeup_probe", 2048, NULL, 2, NULL) != pdPASS) {
        s_probe_active = false;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        service_offline_action_diagnostic_release(true);
#endif
        return -1;
    }
    shellPrint(shellGetCurrent(), "OFFLINE_PROBE scheduled; wait for READY; do not press buttons during capture\n");
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 wakeup_probe, shell_wakeup_probe, "Temporary offline recognition-only window; RAM-only");

static int shell_wakeup_diag(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "level") == 0 &&
        strlen(argv[2]) == 1U && argv[2][0] >= '0' && argv[2][0] <= '4') {
        int ret = app_wakeup_sensitivity_level_set((app_wakeup_sensitivity_level_e)(argv[2][0] - '0'));
        shellPrint(shellGetCurrent(), "wakeup diag threshold set ret=%d\n", ret);
        return ret;
    }
    if (argc != 1 && !(argc == 2 && strcmp(argv[1], "status") == 0)) {
        shellPrint(shellGetCurrent(), "Usage: wakeup_diag [status|level 0..4]; level is RAM-only\n");
        return -1;
    }
    struct app_datas *app = get_app_datas();
    shellPrint(shellGetCurrent(),
               "wakeup diag audio=%d feed=%d prepare=%d start=%d mode=%d level=%d threshold_ret=%d "
               "can_wakeup=%d work_mode=%u\n",
               s_audio_inited, s_wakeup_feed_enabled, s_diag_prepare_ret, s_diag_start_ret,
               s_diag_mode, (int)s_sensitivity_level, s_diag_threshold_ret,
               app ? app->can_wakeup : -1, app ? (unsigned)app->voice_work_mode : 0U);
    shellPrint(shellGetCurrent(),
               "wakeup diag input=%lu submitted=%lu submit_fail=%lu alloc_fail=%lu results=%lu keywords=%lu parse_fail=%lu\n",
               (unsigned long)s_diag_input_blocks, (unsigned long)s_diag_submit_ok,
               (unsigned long)s_diag_submit_failed, (unsigned long)s_diag_alloc_failed,
               (unsigned long)s_diag_results, (unsigned long)s_diag_keywords, (unsigned long)s_diag_parse_failed);
    shellPrint(shellGetCurrent(), "wakeup diag keyword_accepted=%lu keyword_throttled=%lu last_keyword_tick=%lu\n",
               (unsigned long)s_diag_keyword_accepted, (unsigned long)s_diag_keyword_throttled,
               (unsigned long)s_last_wakeup_keyword_tick);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 wakeup_diag, shell_wakeup_diag, "Read wakeup pipeline counters; optional RAM-only threshold");

static int shell_algo_restart(int argc, char **argv)
{
    if (argc < 2) {
        return -1;
    }
    Shell* shell = shellGetCurrent();
    if (strncmp(argv[1], "1", 1) == 0) {
        shellPrint(shell, "algo restart 1\r\n");

        int ret = acomp_wakeup_stop();
        shellPrint(shell, "acomp_wakeup_stop ret:%d\r\n", ret);

        ret = acomp_wakeup_start();
        shellPrint(shell, "acomp_wakeup_start ret:%d\r\n", ret);

        ret = acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_WAKEUP);
        shellPrint(shell, "acomp_wakeup_set_algo_mode ret:%d\r\n", ret);

        // TODO: arcs-mini 2.6版本使用
        // ret = acomp_wakeup_set_timeout(app_wakeup_get_esr_timeout_ms()); 
        ret = acomp_wakeup_set_timeout(0);
        shellPrint(shell, "acomp_wakeup_set_timeout ret:%d\r\n", ret);
    } else {
        shellPrint(shell, "algo restart error\r\n");
    }

    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, algo_restart,
                 shell_algo_restart, "algo_restart 1/2");

#ifdef CONFIG_AUDIO_TDOA_ENABLE
static int shell_tdoa_status(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "status") != 0)) {
        shellPrint(shellGetCurrent(), "Usage: tdoa [status]\n");
        return -1;
    }

    app_audio_localization_snapshot_t snapshot = {0};
    if (!app_audio_localization_get_snapshot(&snapshot)) {
        shellPrint(shellGetCurrent(), "TDOA snapshot busy\n");
        return -1;
    }

    uint32_t now_ms = (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t age_ms = snapshot.timestamp_ms == 0U ? 0U : now_ms - snapshot.timestamp_ms;
    shellPrint(shellGetCurrent(),
               "TDOA: v=%u e=%u side=%s angle=%ddeg delay=%ld "
               "corr=%u active=%u/%u fused=%u age=%lums\n",
               snapshot.valid ? 1U : 0U, snapshot.echo_gated ? 1U : 0U, tdoa_side_name(snapshot.side),
               snapshot.axis_angle_deg, (long)snapshot.delay_milli_samples, snapshot.correlation_milli,
               snapshot.active_blocks, snapshot.total_blocks, snapshot.level_fused ? 1U : 0U, (unsigned long)age_ms);
    shellPrint(shellGetCurrent(), "TDOA_LEVEL: mean=%u/%u peak=%u/%u balance=%d\n", snapshot.mic0_mean_abs,
               snapshot.mic1_mean_abs, snapshot.mic0_peak, snapshot.mic1_peak, snapshot.level_balance_milli);
    /* Keep this line below the shell's fixed print buffer size. */
    shellPrint(shellGetCurrent(),
               "TDOA_JSON {\"s\":%lu,\"v\":%u,\"e\":%u,\"x\":%u,"
               "\"d\":%ld,\"c\":%u,\"a\":%u,\"m0\":%u,\"m1\":%u,"
               "\"p0\":%u,\"p1\":%u,\"b\":%d,\"f\":%u}\n",
               (unsigned long)snapshot.sequence, snapshot.valid ? 1U : 0U, snapshot.echo_gated ? 1U : 0U,
               (unsigned int)snapshot.side, (long)snapshot.delay_milli_samples, snapshot.correlation_milli,
               snapshot.active_blocks, snapshot.mic0_mean_abs, snapshot.mic1_mean_abs, snapshot.mic0_peak,
               snapshot.mic1_peak, snapshot.level_balance_milli, snapshot.level_fused ? 1U : 0U);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, tdoa,
                 shell_tdoa_status, raw dual microphone TDOA status);
#endif

#ifdef CONFIG_BOARD_ARCS_MINI
static int gain_cmd_set_mic(int argc, char **argv);
static int gain_cmd_set_mic_d(int argc, char **argv);
static int gain_cmd_set_aec(int argc, char **argv);
static int gain_cmd_set_aec_d(int argc, char **argv);
static int gain_cmd_set_spk(int argc, char **argv);
static int gain_cmd_set_spk_d(int argc, char **argv);
static int gain_cmd_print(int argc, char **argv);
static int gain_cmd_help(int argc, char **argv);

static const struct listen_cmd_t g_gain_cmds[] = {
    {"set_mic", gain_cmd_set_mic, "Set MIC0 analog gain"},
    {"set_mic_d", gain_cmd_set_mic_d, "Set MIC0 digital gain"},
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    {"set_mic2", gain_cmd_set_aec, "Set MIC1 analog gain"},
    {"set_mic2_d", gain_cmd_set_aec_d, "Set MIC1 digital gain"},
#endif
    {"set_aec", gain_cmd_set_aec, "Set AEC reference analog gain"},
    {"set_aec_d", gain_cmd_set_aec_d, "Set AEC reference digital gain"},
    {"set_spk", gain_cmd_set_spk, "Set speaker analog gain"},
    {"set_spk_d", gain_cmd_set_spk_d, "Set speaker digital gain"},
    {"print", gain_cmd_print, "Print current gain settings"},
    {"help", gain_cmd_help, "Show help information"},
};

static int gain_cmd_set_mic(int argc, char **argv)
{
    if (argc != 1 || argv == NULL || argv[0] == NULL) {
        shellPrint(shellGetCurrent(), "Usage: gain set_mic [gain_db] (valid range: %d to %d dB)\n",
                   ADC_PDM_GAIN_A_MIN_DB, ADC_PDM_GAIN_A_MAX_DB);
        return -1;
    }

    int gain_db = atoi(argv[0]);
    if (gain_db < ADC_PDM_GAIN_A_MIN_DB || gain_db > ADC_PDM_GAIN_A_MAX_DB) {
        shellPrint(shellGetCurrent(), "Error: gain_db out of range (%d to %d dB)\n",
                   ADC_PDM_GAIN_A_MIN_DB, ADC_PDM_GAIN_A_MAX_DB);
        return -1;
    }
    if (g_audio_dev == NULL) {
        shellPrint(shellGetCurrent(), "Error: audio device not ready\n");
        return -1;
    }

    g_record_mic_analog_gain = gain_db;

    lisa_audio_gain_t gain[] = {
        {
            .analog_gain = g_record_mic_analog_gain,
            .digital_gain = g_record_mic_digital_gain,
        },
        {
            .analog_gain = g_record_ref_analog_gain,
            .digital_gain = g_record_ref_digital_gain,
        },
    };
    lisa_audio_record_set_gain(g_audio_dev, gain);

    shellPrint(shellGetCurrent(), "MIC0 analog gain set to %d dB\n", gain_db);

    return 0;
}

static int gain_cmd_set_mic_d(int argc, char **argv)
{
    if (argc != 1 || argv == NULL || argv[0] == NULL) {
        shellPrint(shellGetCurrent(), "Usage: gain set_mic_d [gain_db] (valid range: %d to %d dB)\n",
                   ADC_PDM_GAIN_D_MIN_DB, ADC_PDM_GAIN_D_MAX_DB);
        return -1;
    }

    int gain_db = atoi(argv[0]);
    if (gain_db < ADC_PDM_GAIN_D_MIN_DB || gain_db > ADC_PDM_GAIN_D_MAX_DB) {
        shellPrint(shellGetCurrent(), "Error: gain_db out of range (%d to %d dB)\n",
                   ADC_PDM_GAIN_D_MIN_DB, ADC_PDM_GAIN_D_MAX_DB);
        return -1;
    }
    if (g_audio_dev == NULL) {
        shellPrint(shellGetCurrent(), "Error: audio device not ready\n");
        return -1;
    }

    g_record_mic_digital_gain = gain_db;

    lisa_audio_gain_t gain[] = {
        {
            .analog_gain = g_record_mic_analog_gain,
            .digital_gain = g_record_mic_digital_gain,
        },
        {
            .analog_gain = g_record_ref_analog_gain,
            .digital_gain = g_record_ref_digital_gain,
        },
    };
    lisa_audio_record_set_gain(g_audio_dev, gain);

    shellPrint(shellGetCurrent(), "MIC0 digital gain set to %d dB\n", gain_db);

    return 0;
}

static int gain_cmd_set_aec(int argc, char **argv)
{
    if (argc != 1 || argv == NULL || argv[0] == NULL) {
        shellPrint(shellGetCurrent(), "Usage: gain set_aec [gain_db] (valid range: %d to %d dB)\n",
                   ADC_PDM_GAIN_A_MIN_DB, ADC_PDM_GAIN_A_MAX_DB);
        return -1;
    }

    int gain_db = atoi(argv[0]);
    if (gain_db < ADC_PDM_GAIN_A_MIN_DB || gain_db > ADC_PDM_GAIN_A_MAX_DB) {
        shellPrint(shellGetCurrent(), "Error: gain_db out of range (%d to %d dB)\n",
                   ADC_PDM_GAIN_A_MIN_DB, ADC_PDM_GAIN_A_MAX_DB);
        return -1;
    }
    if (g_audio_dev == NULL) {
        shellPrint(shellGetCurrent(), "Error: audio device not ready\n");
        return -1;
    }

    g_record_ref_analog_gain = gain_db;

    lisa_audio_gain_t gain[] = {
        {
            .analog_gain = g_record_mic_analog_gain,
            .digital_gain = g_record_mic_digital_gain,
        },
        {
            .analog_gain = g_record_ref_analog_gain,
            .digital_gain = g_record_ref_digital_gain,
        },
    };
    lisa_audio_record_set_gain(g_audio_dev, gain);

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    shellPrint(shellGetCurrent(), "MIC1 analog gain set to %d dB\n", gain_db);
#else
    shellPrint(shellGetCurrent(), "AEC reference analog gain set to %d dB\n", gain_db);
#endif

    return 0;
}

static int gain_cmd_set_aec_d(int argc, char **argv)
{
    if (argc != 1 || argv == NULL || argv[0] == NULL) {
        shellPrint(shellGetCurrent(), "Usage: gain set_aec_d [gain_db] (valid range: %d to %d dB)\n",
                   ADC_PDM_GAIN_D_MIN_DB, ADC_PDM_GAIN_D_MAX_DB);
        return -1;
    }

    int gain_db = atoi(argv[0]);
    if (gain_db < ADC_PDM_GAIN_D_MIN_DB || gain_db > ADC_PDM_GAIN_D_MAX_DB) {
        shellPrint(shellGetCurrent(), "Error: gain_db out of range (%d to %d dB)\n",
                   ADC_PDM_GAIN_D_MIN_DB, ADC_PDM_GAIN_D_MAX_DB);
        return -1;
    }
    if (g_audio_dev == NULL) {
        shellPrint(shellGetCurrent(), "Error: audio device not ready\n");
        return -1;
    }

    g_record_ref_digital_gain = gain_db;

    lisa_audio_gain_t gain[] = {
        {
            .analog_gain = g_record_mic_analog_gain,
            .digital_gain = g_record_mic_digital_gain,
        },
        {
            .analog_gain = g_record_ref_analog_gain,
            .digital_gain = g_record_ref_digital_gain,
        },
    };
    lisa_audio_record_set_gain(g_audio_dev, gain);

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    shellPrint(shellGetCurrent(), "MIC1 digital gain set to %d dB\n", gain_db);
#else
    shellPrint(shellGetCurrent(), "AEC reference digital gain set to %d dB\n", gain_db);
#endif

    return 0;
}

static int gain_cmd_set_spk(int argc, char **argv)
{
    if (argc != 1 || argv == NULL || argv[0] == NULL) {
        shellPrint(shellGetCurrent(), "Usage: gain set_spk [gain_db] (valid range: %d to %d dB)\n", DAC_GAIN_A_MIN_DB,
                   DAC_GAIN_A_MAX_DB);
        return -1;
    }

    int gain_db = atoi(argv[0]);
    if (gain_db < DAC_GAIN_A_MIN_DB || gain_db > DAC_GAIN_A_MAX_DB) {
        shellPrint(shellGetCurrent(), "Error: gain_db out of range (%d to %d dB)\n", DAC_GAIN_A_MIN_DB,
                   DAC_GAIN_A_MAX_DB);
        return -1;
    }
    if (g_audio_dev == NULL) {
        shellPrint(shellGetCurrent(), "Error: audio device not ready\n");
        return -1;
    }

    g_play_analog_gain = gain_db;

    lisa_audio_gain_t gain = {
        .analog_gain = g_play_analog_gain,
        .digital_gain = g_play_digital_gain,
    };
    lisa_audio_play_set_gain(g_audio_dev, &gain);

    shellPrint(shellGetCurrent(), "Speaker analog gain set to %d dB\n", gain_db);

    return 0;
}

static int gain_cmd_set_spk_d(int argc, char **argv)
{
    if (argc != 1 || argv == NULL || argv[0] == NULL) {
        shellPrint(shellGetCurrent(), "Usage: gain set_spk_d [gain_db] (valid range: %d to %d dB)\n",
                   DAC_GAIN_D_MIN_DB, DAC_GAIN_D_MAX_DB);
        return -1;
    }

    int gain_db = atoi(argv[0]);
    if (gain_db < DAC_GAIN_D_MIN_DB || gain_db > DAC_GAIN_D_MAX_DB) {
        shellPrint(shellGetCurrent(), "Error: gain_db out of range (%d to %d dB)\n",
                   DAC_GAIN_D_MIN_DB, DAC_GAIN_D_MAX_DB);
        return -1;
    }
    if (g_audio_dev == NULL) {
        shellPrint(shellGetCurrent(), "Error: audio device not ready\n");
        return -1;
    }

    g_play_digital_gain = gain_db;

    lisa_audio_gain_t gain = {
        .analog_gain = g_play_analog_gain,
        .digital_gain = g_play_digital_gain,
    };
    lisa_audio_play_set_gain(g_audio_dev, &gain);

    shellPrint(shellGetCurrent(), "Speaker digital gain set to %d dB\n", gain_db);

    return 0;
}

static int gain_cmd_print(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    shellPrint(shellGetCurrent(), "Current Gain Settings:\n");
    shellPrint(shellGetCurrent(), "     MIC0: analog=%d dB, digital=%d dB\n", g_record_mic_analog_gain,
               g_record_mic_digital_gain);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    shellPrint(shellGetCurrent(), "     MIC1: analog=%d dB, digital=%d dB\n", g_record_ref_analog_gain,
               g_record_ref_digital_gain);
#else
    shellPrint(shellGetCurrent(), "      AEC: analog=%d dB, digital=%d dB\n", g_record_ref_analog_gain,
               g_record_ref_digital_gain);
#endif
    shellPrint(shellGetCurrent(), "  Speaker: analog=%d dB, digital=%d dB\n", g_play_analog_gain,
               g_play_digital_gain);
    return 0;
}

static int gain_cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    shellPrint(shellGetCurrent(), "Gain commands:\n");
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_mic", "Set MIC0 analog gain",
               ADC_PDM_GAIN_A_MIN_DB, ADC_PDM_GAIN_A_MAX_DB);
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_mic_d", "Set MIC0 digital gain",
               ADC_PDM_GAIN_D_MIN_DB, ADC_PDM_GAIN_D_MAX_DB);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_mic2", "Set MIC1 analog gain",
               ADC_PDM_GAIN_A_MIN_DB, ADC_PDM_GAIN_A_MAX_DB);
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_mic2_d", "Set MIC1 digital gain",
               ADC_PDM_GAIN_D_MIN_DB, ADC_PDM_GAIN_D_MAX_DB);
#endif
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_aec", "Set AEC reference analog gain",
               ADC_PDM_GAIN_A_MIN_DB, ADC_PDM_GAIN_A_MAX_DB);
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_aec_d", "Set AEC reference digital gain",
               ADC_PDM_GAIN_D_MIN_DB, ADC_PDM_GAIN_D_MAX_DB);
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_spk", "Set speaker analog gain",
               DAC_GAIN_A_MIN_DB, DAC_GAIN_A_MAX_DB);
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s (%d to %d dB)\n", "set_spk_d", "Set speaker digital gain",
               DAC_GAIN_D_MIN_DB, DAC_GAIN_D_MAX_DB);
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s\n", "print", "Print current gain settings");
    shellPrint(shellGetCurrent(), "%-17s\t:\t%s\n", "help", "Show help information");
    return 0;
}

static int gain_cmd_handler(int argc, char **argv)
{
    if (argc == 1) {
        return gain_cmd_help(argc, argv);
    }

    for (size_t i = 0; i < sizeof(g_gain_cmds) / sizeof(g_gain_cmds[0]); i++) {
        if (strcmp(g_gain_cmds[i].name, argv[1]) == 0) {
            return g_gain_cmds[i].exec(argc - 2, &argv[2]);
        }
    }

    return gain_cmd_help(argc, argv);
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, gain,
                 gain_cmd_handler, audio gain settings);
#else  // !CONFIG_BOARD_ARCS_MINI
/**
* @brief Shell command to set record gain
* Usage: record_gain <analog_gain> <digital_gain>
*/
static int shell_record_gain(int argc, char **argv)
{
 if (argc != 3) {
     shellPrint(shellGetCurrent(), "Usage: record_gain <analog_gain> <digital_gain>\r\n");
     shellPrint(shellGetCurrent(), "  analog_gain: -12 to 36 dB (step 2 dB)\r\n");
     shellPrint(shellGetCurrent(), "  digital_gain: -83 to 42 dB (step 1 dB)\r\n");
     shellPrint(shellGetCurrent(), "  Example: record_gain 36 0\r\n");
     shellPrint(shellGetCurrent(), "  Current: analog=%d, digital=%d\r\n",
                g_record_analog_gain, g_record_digital_gain);
     return -1;
 }

 int32_t analog_gain = atoi(argv[1]);
 int32_t digital_gain = atoi(argv[2]);

 /* 检查模拟增益范围: -12 to 36 dB */
 if (analog_gain < -12 || analog_gain > 36) {
     shellPrint(shellGetCurrent(), "Error: analog_gain out of range (-12 to 36 dB)\r\n");
     return -1;
 }

 /* 检查数字增益范围: -83 to 42 dB */
 if (digital_gain < -83 || digital_gain > 42) {
     shellPrint(shellGetCurrent(), "Error: digital_gain out of range (-83 to 42 dB)\r\n");
     return -1;
 }

 g_record_analog_gain = analog_gain;
 g_record_digital_gain = digital_gain;
 lisa_audio_gain_t gain = {
     .analog_gain = g_record_analog_gain,
     .digital_gain = g_record_digital_gain,
 };

 lisa_audio_record_set_gain(g_audio_dev, &gain);
 shellPrint(shellGetCurrent(), "Record gain updated: analog=%d, digital=%d\r\n",
            g_record_analog_gain, g_record_digital_gain);

 return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, record_gain,
                 shell_record_gain, "set record gain <analog> <digital>");

/**
* @brief Shell command to set play gain
* Usage: play_gain <analog_gain> <digital_gain>
*/
static int shell_play_gain(int argc, char **argv)
{
 if (argc != 3) {
     shellPrint(shellGetCurrent(), "Usage: play_gain <analog_gain> <digital_gain>\r\n");
     shellPrint(shellGetCurrent(), "  analog_gain: -24 to 6 dB (step 2 dB)\r\n");
     shellPrint(shellGetCurrent(), "  digital_gain: -113 to 30 dB (step 1 dB)\r\n");
     shellPrint(shellGetCurrent(), "  Example: play_gain 0 -12\r\n");
     shellPrint(shellGetCurrent(), "  Current: analog=%d, digital=%d\r\n",
                g_play_analog_gain, g_play_digital_gain);
     return -1;
 }

 int32_t analog_gain = atoi(argv[1]);
 int32_t digital_gain = atoi(argv[2]);

 /* 检查模拟增益范围: -24 to 6 dB */
 if (analog_gain < -24 || analog_gain > 6) {
     shellPrint(shellGetCurrent(), "Error: analog_gain out of range (-24 to 6 dB)\r\n");
     return -1;
 }

 /* 检查数字增益范围: -113 to 30 dB */
 if (digital_gain < -113 || digital_gain > 30) {
     shellPrint(shellGetCurrent(), "Error: digital_gain out of range (-113 to 30 dB)\r\n");
     return -1;
 }

 g_play_analog_gain = analog_gain;
 g_play_digital_gain = digital_gain;
 lisa_audio_gain_t gain = {
     .analog_gain = g_play_analog_gain,
     .digital_gain = g_play_digital_gain,
 };
 lisa_audio_play_set_gain(g_audio_dev, &gain);
 shellPrint(shellGetCurrent(), "Play gain updated: analog=%d, digital=%d\r\n",
            g_play_analog_gain, g_play_digital_gain);

 return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, play_gain,
                 shell_play_gain, "set play gain <analog> <digital>");
#endif // CONFIG_BOARD_ARCS_MINI
