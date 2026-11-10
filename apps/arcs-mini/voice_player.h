#pragma once
void voice_player_pcm_output(char *data, int size);

#include <stdbool.h>
#include <stdint.h>

/**
 * 播放需要抢占音乐的本地提示音。
 *
 * 提示音开始时通过 INTENT_PROMPT_TONE 抢占当前意图；
 * 提示音结束或被停止后释放 intent，由下层意图自行恢复。
 */
void voice_player_play_prompt_tone_url(const char *url);

/** Submit an IMU prompt through the original tone queue; false if rejected. */
bool voice_player_try_play_prompt_tone_url(const char *url);

/** Play an IMU prompt immediately; false when another local tone is active. */
bool voice_player_try_play_prompt_tone_now_url(const char *url);

/* Fixed local music, owned by a cancellable gait ticket. No recording.
 * position: 0=playing, 1=completed, -EAGAIN=starting, other negative=stopped. */
uint32_t voice_player_beat_music_begin(void);
int voice_player_beat_music_position(uint32_t ticket, uint32_t *position_ms);
void voice_player_beat_music_end(uint32_t ticket);
void voice_player_beat_music_cancel(void);

/** True while cloud TTS is preparing, queued, or playing. */
bool voice_player_tts_is_active(void);
void voice_player_notify_upload_complete(uint32_t epoch);
void voice_player_cancel_upload_cue(uint32_t epoch);
void voice_player_wait_upload_cue(void);

/* Lifetime counters only; no audio or ASR content is retained. "started" is
 * the player's PLAYING event, not just successfully queueing a cue request. */
typedef struct {
    uint32_t valid_requests, duplicate_requests;
    uint32_t started, completed, cancelled, stopped;
    uint32_t skipped_stale, skipped_busy, skipped_callbacks, skipped_tts;
    uint32_t enqueue_failures, playback_failures, wait_timeouts;
    uint32_t last_epoch, pending_epoch, playing_epoch;
} voice_upload_cue_stats_t;
void voice_player_get_upload_cue_stats(voice_upload_cue_stats_t *stats);

/** Nonblocking speaker guard for ESR mode commands; keywords bypass it. */
bool voice_player_mode_command_blocked(void);

/** Worker-only poll: generation cancellation and its player callbacks drained. */
bool voice_player_offline_audio_cancel_ready(uint32_t generation);
