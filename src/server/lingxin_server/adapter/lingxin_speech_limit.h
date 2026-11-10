#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LINGXIN_RECORD_MAX_SAMPLES (16000U * 12U)
typedef struct {
    uint32_t accepted, speech_samples;
    bool reached;
    uint32_t checked_frames, positive_frames, window_peak;
} lingxin_speech_limit_result_t;

/* Single AP-output task only. Generation changes reset the VAD and budget.
 * This is a maximum-duration detector, never an upload/silence gate. */
lingxin_speech_limit_result_t lingxin_speech_limit_process(
    uint32_t generation, const int16_t *samples, uint32_t count);
