#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Called in task context, never directly from the ADC ISR. */
void lingxin_audio_capture_clean(const int16_t *samples, uint32_t count);
void lingxin_clean_capture_reset(void);
void lingxin_clean_capture_arm(void);
bool lingxin_clean_capture_current(uint32_t generation);
void lingxin_clean_recorder_prepare_stop(void *recorder, bool drain);
/* Twelve seconds from the first local speech-VAD frame, not listening start. */
#include "lingxin_speech_limit.h"
bool lingxin_clean_recorder_take_limit_stop(void *recorder);
bool lingxin_clean_recorder_limit_timed_out(void *recorder);

/* Scalar diagnostics only: no PCM samples, text, allocation or per-frame log. */
#define LINGXIN_CAPTURE_STATS_SLOTS 4U
typedef struct {
    uint32_t generation, frames, accepted_samples, gate_drops, capture_drops;
    uint32_t accepted_peak, accepted_signal_frames, gated_peak, max_queued;
    uint32_t speech_samples;
    uint32_t vad_checked, vad_positive, vad_window_peak;
} lingxin_capture_stats_t;
void lingxin_clean_capture_get_stats(lingxin_capture_stats_t *stats);
