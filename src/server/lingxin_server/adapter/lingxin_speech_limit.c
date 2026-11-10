#include "lingxin_speech_limit.h"
#include "vad/vad_core.h"
#include <string.h>

/* Static core avoids allocation on the real-time capture path. Only this
 * task mutates it; control tasks reset by advancing the capture generation. */
static VadInstT s_vad;
static uint32_t s_generation, s_frame_used, s_speech_samples;
static uint32_t s_speech_window, s_checked_frames, s_positive_frames, s_window_peak;
static int16_t s_frame[160]; /* WebRTC accepts 10/20/30 ms, AP supplies 16 ms. */

lingxin_speech_limit_result_t lingxin_speech_limit_process(
    uint32_t generation, const int16_t *samples, uint32_t count)
{
    if (generation != s_generation) {
        WebRtcVad_InitCore(&s_vad);
        WebRtcVad_set_mode_core(&s_vad, 2);
        s_generation = generation;
        s_frame_used = s_speech_samples = 0U;
        s_speech_window = s_checked_frames = s_positive_frames = s_window_peak = 0U;
    }
    uint32_t consumed = 0U;
    while (consumed < count && s_speech_samples < LINGXIN_RECORD_MAX_SAMPLES) {
        uint32_t n = count - consumed;
        if (s_speech_samples) {
            uint32_t left = LINGXIN_RECORD_MAX_SAMPLES - s_speech_samples;
            if (n > left) n = left;
            s_speech_samples += n;
        } else {
            uint32_t left = 160U - s_frame_used;
            if (n > left) n = left;
            memcpy(s_frame + s_frame_used, samples + consumed, n * sizeof(*samples));
            s_frame_used += n;
            if (s_frame_used == 160U) {
                /* A single transient plus WebRTC hangover is not speech
                 * onset. Confirm 200ms of fresh positives inside 300ms.
                 * This qualifies only the timer; all PCM keeps streaming. */
                int vad = WebRtcVad_CalcVad16khz(&s_vad, s_frame, 160U);
                ++s_checked_frames;
                if (vad == 1) ++s_positive_frames;
                s_speech_window = ((s_speech_window << 1U) | (vad == 1 ? 1U : 0U)) &
                                   ((1U << 30U) - 1U);
                uint32_t positives = (uint32_t)__builtin_popcount(s_speech_window);
                if (positives > s_window_peak) s_window_peak = positives;
                if (positives >= 20U) {
                    /* Backdate to the first positive in the qualified window,
                     * so confirmation doesn't add time to the twelve seconds. */
                    s_speech_samples = (32U - (uint32_t)__builtin_clz(s_speech_window)) * 160U;
                }
                s_frame_used = 0U;
            }
        }
        consumed += n;
    }
    return (lingxin_speech_limit_result_t){
        .accepted = consumed, .speech_samples = s_speech_samples,
        .reached = s_speech_samples == LINGXIN_RECORD_MAX_SAMPLES,
        .checked_frames = s_checked_frames, .positive_frames = s_positive_frames,
        .window_peak = s_window_peak,
    };
}
