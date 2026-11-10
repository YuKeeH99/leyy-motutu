#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int lingxin_server_available(void);
int lingxin_server_init(void);
int lingxin_server_connect(void);
int lingxin_server_chat_start(void);
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
typedef struct {
    bool active;
    bool agent_text;
    bool actions;
    bool final_received;
    uint32_t vad_to_final_ms;
    char text[256];
} lingxin_asr_probe_t;
int lingxin_server_asr_probe_start(void);
void lingxin_server_asr_probe_read(lingxin_asr_probe_t *result);
bool lingxin_server_asr_probe_active(void);
#endif
/* A wake worker must retain its accepted owner across asynchronous waits. */
int lingxin_server_chat_start_owned(uint32_t owner_generation);
int lingxin_next_task_id(char *task_id, size_t task_id_size);
int lingxin_server_chat_start_text(const char *text);
int lingxin_server_chat_start_owned_text(const char *text, uint32_t owner_generation);
/* Speak one local device-query result, then resume normal voice input without
 * another wake or replaying the result. Generic start_text stays single round. */
int lingxin_server_chat_start_query_text(const char *text);
int lingxin_server_chat_end_record(void);
int lingxin_server_chat_stop(void);
bool lingxin_server_chat_is_active(void);

/* Stable application-facing dialog phases.  Keep the SDK's private state
 * numbers inside lingxin_server.c so button/UI code cannot depend on a vendor
 * enum that may change between engine drops. */
typedef enum {
    LINGXIN_DIALOG_PHASE_UNAVAILABLE = -1,
    LINGXIN_DIALOG_PHASE_IDLE = 0,
    LINGXIN_DIALOG_PHASE_LISTENING,
    LINGXIN_DIALOG_PHASE_SPEAKING,
    LINGXIN_DIALOG_PHASE_TRANSITION,
} lingxin_dialog_phase_t;

lingxin_dialog_phase_t lingxin_server_dialog_phase(void);
/* Close the microphone uplink while a local wake acknowledgement is audible.
 * The returned generation prevents an older delayed release from opening a
 * newer wakeup gate. */
uint32_t lingxin_server_audio_uplink_gate_close(const char *reason);
uint32_t lingxin_server_audio_uplink_gate_hold(const char *reason);
void lingxin_server_audio_uplink_gate_open(uint32_t generation, const char *reason);
bool lingxin_server_audio_uplink_gate_is_open(uint32_t *generation);
/* Stop and discard only the current cloud TTS response. The next server
 * audioInit starts with a clean playback policy. */
void lingxin_server_suppress_current_tts(const char *reason);
/* Notify session controls from the real cloud app_player completion path.
 * Some engine versions do not emit CHAT_LIFE_CYCLE_EVENT_PLAY_END even though
 * Lingxin_Download_Audio_PlayEnd was delivered by the audio adapter. */
void lingxin_server_tts_playback_complete(void);
/* RAM-only control counters. No ASR, TTS text or audio is retained. */
typedef struct {
    uint32_t received;
    uint32_t last_received_generation;
    uint32_t last_received_epoch;
    uint32_t dispatched;
    uint32_t last_dispatched_generation;
    uint32_t publish_failures;
    uint32_t timeouts;
    uint32_t handled; /* All MCP chat-exit events; receipt alone is not an EXIT. */
    int last_stop_result;
    uint32_t session_exits;
    uint32_t last_exit_generation;
    int last_exit_code;
    uint32_t pending_generation;
    bool pending;
} lingxin_goodbye_stats_t;
void lingxin_server_get_goodbye_stats(lingxin_goodbye_stats_t *stats);
void lingxin_server_note_chat_exit_handled(int result);
/* Bind application ownership only after the matching protocol task starts. */
void lingxin_server_task_started(const char *task_id);
int lingxin_server_chat_state(void);
int lingxin_server_ws_probe_start(void);
void lingxin_server_diag_timing_begin(const char *reason);
void lingxin_server_diag_timing_mark(const char *stage, int value);
void lingxin_server_diag_timing_transition(int from_state, int to_state, int event);
void lingxin_server_diag_timing_end(const char *stage, int value);

#ifdef __cplusplus
}
#endif
