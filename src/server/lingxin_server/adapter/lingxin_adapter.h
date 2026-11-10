#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lingxin_http.h"
#include "lingxin_local_player.h"
#include "lingxin_mutex.h"
#include "lingxin_recorder.h"
#include "lingxin_semaphore.h"
#include "lingxin_system_time.h"
#include "lingxin_thread.h"
#include "lingxin_timer.h"
#include "lingxin_websocket.h"

int lingxin_adapter_http_post(HttpConfig *config, RequestCallback userCallback, void *userData);
WebsocketClient *lingxin_adapter_websocket_init(WebsocketConfig *config);
bool lingxin_adapter_websocket_start(WebsocketClient *client);
bool lingxin_adapter_websocket_send_text(WebsocketClient *client, const char *message);
int lingxin_adapter_websocket_send_binary(WebsocketClient *client, const char *audioData, size_t dataSize);
void lingxin_adapter_websocket_close(WebsocketClient *client);

uint32_t lingxin_audio_uplink_gate_close(const char *reason);
uint32_t lingxin_audio_uplink_gate_hold(const char *reason);
void lingxin_audio_uplink_gate_open(uint32_t generation, const char *reason);
bool lingxin_audio_uplink_gate_is_open(uint32_t *generation);
void lingxin_audio_suppress_current_tts(const char *reason);
/* Nonblocking physical playback readiness for a new input task. False from
 * failure/cancellation reservation through writer/producer exit and confirmed
 * player cleanup, including queued termination and a failed reset between
 * attempts. Suppression requested or cleanup_running == false is insufficient.
 * Does not acquire the player's operation mutex or consume a wake request. */
bool lingxin_audio_playback_input_ready(void);

typedef struct {
    uint32_t generation, chunks, bytes, pcm_bytes;
    uint32_t pcm_written, queued_bytes, queue_peak_bytes, failures;
    bool writer_exited, cleanup_pending, finish_submitted, input_ready;
    bool playing, started, suppressed, failed, ended;
} lingxin_playback_stats_t;
void lingxin_audio_get_playback_stats(lingxin_playback_stats_t *stats);
/* Choose startup buffering for the next playback generation. */
void lingxin_audio_set_codex_prefill(bool enabled);
