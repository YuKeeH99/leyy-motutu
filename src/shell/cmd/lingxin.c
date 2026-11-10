#include "cmd.h"
#include "shell.h"
#include "string.h"

#ifdef CONFIG_VOICE_LINK_LINGXIN
#include "chat_api.h"
#include "lingxin_server.h"
#include "lingxin_server_config.h"
#include "lingxin_trace.h"
#include "lisa_log.h"
#include "lisa_mem.h"
#include "lisa_thread.h"
#include "lisa_typedef.h"
#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
#include "lingxin_protocol_manager.h"
#include "lingxin_adapter.h"
#include "voice_player.h"
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
#include "lingxin_clean_recorder.h"
#endif
#endif
#endif

static int s_lingxin_shell_starting;
static int s_lingxin_shell_initing;
static int s_lingxin_shell_chatting;
static int s_lingxin_shell_probing;
static int s_lingxin_shell_texting;

#ifdef CONFIG_VOICE_LINK_LINGXIN
static void lingxin_shell_init_thread(void *arg)
{
    (void)arg;

    LOGI("[Lingxin] shell async init begin");
    int ret = lingxin_server_init();
    LOGI("[Lingxin] shell async init end, ret=%d", ret);
    s_lingxin_shell_initing = 0;
}

static void lingxin_shell_start_thread(void *arg)
{
    (void)arg;

    lingxin_trace_set("cmd:start_thread_enter");
    LOGI("[Lingxin] shell async start begin");
    lisa_thread_mdelay(1000);
    lingxin_trace_set("cmd:start_thread_after_delay");
    lingxin_trace_set("cmd:start_chat_start_call");
    int ret;
#ifdef CONFIG_APP_CODEX_VOICE_INPUT
    if (arg != NULL) ret = lingxin_server_asr_probe_start();
    else
#endif
        ret = lingxin_server_chat_start();
    lingxin_trace_set(ret == 0 ? "cmd:start_chat_start_ret_ok" : "cmd:start_chat_start_ret_fail");
    LOGI("[Lingxin] shell async start end, ret=%d", ret);
    s_lingxin_shell_starting = 0;
}

static void lingxin_shell_chat_thread(void *arg)
{
    (void)arg;

    lingxin_trace_set("cmd:chat_thread_enter");
    LOGI("[Lingxin] shell async chat begin");
    lisa_thread_mdelay(1000);
    lingxin_trace_set("cmd:chat_thread_after_delay");
    lingxin_trace_set("cmd:chat_start_call");
    int ret = lingxin_server_chat_start();
    lingxin_trace_set(ret == 0 ? "cmd:chat_start_ret_ok" : "cmd:chat_start_ret_fail");
    LOGI("[Lingxin] shell async chat end, ret=%d", ret);
    s_lingxin_shell_chatting = 0;
}

static void lingxin_shell_text_thread(void *arg)
{
    char *text = (char *)arg;

    LOGI("[Lingxin] shell async text begin");
    lisa_thread_mdelay(1000);
    int ret = lingxin_server_chat_start_text(text);
    LOGI("[Lingxin] shell async text end, ret=%d", ret);
    lisa_mem_free(text);
    s_lingxin_shell_texting = 0;
}

static void lingxin_shell_probe_thread(void *arg)
{
    (void)arg;

    LOGI("[Lingxin] shell async probe begin");
    int ret = lingxin_server_ws_probe_start();
    LOGI("[Lingxin] shell async probe end, ret=%d", ret);
    s_lingxin_shell_probing = 0;
}
#endif

static int lingxin_cmd_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin link is not enabled\n");
    return -1;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    shellPrint(shellGetCurrent(), "[Lingxin] basic profile: init disabled\n");
    return -1;
#else
    if (s_lingxin_shell_initing) {
        shellPrint(shellGetCurrent(), "[Lingxin] init ignored: already running\n");
        return 0;
    }

    s_lingxin_shell_initing = 1;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lingxin.init",
        .stack_size = 12 * 1024,
        .priority = LISA_OS_PRIORITY_NORMAL,
    };

    shellPrint(shellGetCurrent(), "[Lingxin] shell create async init thread\n");
    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_shell_init_thread, NULL);
    if (thread == NULL) {
        s_lingxin_shell_initing = 0;
        shellPrint(shellGetCurrent(), "[Lingxin] shell create async init thread failed\n");
        return -1;
    }

    return 0;
#endif
}

static int lingxin_cmd_chat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin link is not enabled\n");
    return -1;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    shellPrint(shellGetCurrent(), "[Lingxin] basic profile: chat disabled\n");
    return -1;
#else
    lingxin_trace_clear();
    lingxin_trace_set("cmd:chat_enter");
    if (s_lingxin_shell_initing) {
        lingxin_trace_set("cmd:chat_blocked_initing");
        shellPrint(shellGetCurrent(), "[Lingxin] chat ignored: init is running\n");
        return -1;
    }
    if (s_lingxin_shell_chatting) {
        lingxin_trace_set("cmd:chat_blocked_running");
        shellPrint(shellGetCurrent(), "[Lingxin] chat ignored: already running\n");
        return 0;
    }

    s_lingxin_shell_chatting = 1;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lingxin.chat",
        .stack_size = 12 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    shellPrint(shellGetCurrent(), "[Lingxin] shell schedule async chat thread\n");
    lingxin_trace_set("cmd:chat_before_thread_create");
    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_shell_chat_thread, NULL);
    if (thread == NULL) {
        s_lingxin_shell_chatting = 0;
        lingxin_trace_set("cmd:chat_thread_create_fail");
        shellPrint(shellGetCurrent(), "[Lingxin] shell create async chat thread failed\n");
        return -1;
    }

    lingxin_trace_set("cmd:chat_thread_created");
    shellPrint(shellGetCurrent(), "[Lingxin] chat scheduled\n");
    return 0;
#endif
}

static int lingxin_cmd_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin link is not enabled\n");
    return -1;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    shellPrint(shellGetCurrent(), "[Lingxin] basic profile: chat start disabled\n");
    return -1;
#else
    lingxin_trace_clear();
    lingxin_trace_set("cmd:start_enter");
    if (s_lingxin_shell_starting) {
        lingxin_trace_set("cmd:start_blocked_running");
        shellPrint(shellGetCurrent(), "[Lingxin] start ignored: already running\n");
        return 0;
    }

    s_lingxin_shell_starting = 1;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lingxin.start",
        .stack_size = 12 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    shellPrint(shellGetCurrent(), "[Lingxin] shell schedule async start thread\n");
    lingxin_trace_set("cmd:start_before_thread_create");
    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_shell_start_thread, argc == 1 && strcmp(argv[0], "asr-probe") == 0 ? &s_lingxin_shell_starting : NULL);
    if (thread == NULL) {
        s_lingxin_shell_starting = 0;
        lingxin_trace_set("cmd:start_thread_create_fail");
        shellPrint(shellGetCurrent(), "[Lingxin] shell create async start thread failed\n");
        return -1;
    }

    lingxin_trace_set("cmd:start_thread_created");
    shellPrint(shellGetCurrent(), "[Lingxin] start scheduled\n");
    return 0;
#endif
}

static int lingxin_cmd_probe(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin link is not enabled\n");
    return -1;
#else
    if (s_lingxin_shell_probing) {
        shellPrint(shellGetCurrent(), "[Lingxin] probe ignored: already running\n");
        return 0;
    }

    s_lingxin_shell_probing = 1;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lingxin.probe",
        .stack_size = 12 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };

    shellPrint(shellGetCurrent(), "[Lingxin] shell create async probe thread\n");
    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_shell_probe_thread, NULL);
    if (thread == NULL) {
        s_lingxin_shell_probing = 0;
        shellPrint(shellGetCurrent(), "[Lingxin] shell create async probe thread failed\n");
        return -1;
    }

    return 0;
#endif
}

static int lingxin_cmd_text(int argc, char **argv)
{
#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin link is not enabled\n");
    return -1;
#elif defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    shellPrint(shellGetCurrent(), "[Lingxin] basic profile: text chat disabled\n");
    return -1;
#else
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        shellPrint(shellGetCurrent(), "Usage: lingxin text <text-without-spaces>\n");
        return -1;
    }
    if (s_lingxin_shell_texting) {
        shellPrint(shellGetCurrent(), "[Lingxin] text ignored: already running\n");
        return 0;
    }

    size_t text_len = strlen(argv[0]) + 1U;
    char *text = (char *)lisa_mem_alloc((uint32_t)text_len);
    if (text == NULL) {
        shellPrint(shellGetCurrent(), "[Lingxin] text allocation failed\n");
        return -1;
    }
    memcpy(text, argv[0], text_len);

    s_lingxin_shell_texting = 1;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lingxin.text",
        .stack_size = 12 * 1024,
        .priority = LISA_OS_PRIORITY_LOW,
    };
    lisa_thread_t *thread = lisa_thread_create(&attr, lingxin_shell_text_thread, text);
    if (thread == NULL) {
        s_lingxin_shell_texting = 0;
        lisa_mem_free(text);
        shellPrint(shellGetCurrent(), "[Lingxin] text thread creation failed\n");
        return -1;
    }

    shellPrint(shellGetCurrent(), "[Lingxin] text chat scheduled\n");
    return 0;
#endif
}

static int lingxin_cmd_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin link is not enabled\n");
    return -1;
#else
    int ret = lingxin_server_chat_stop();
    shellPrint(shellGetCurrent(), "[Lingxin] stop ret=%d\n", ret);
    return ret;
#endif
}

static int lingxin_cmd_end(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin link is not enabled\n");
    return -1;
#else
    int ret = lingxin_server_chat_end_record();
    shellPrint(shellGetCurrent(), "[Lingxin] end record ret=%d\n", ret);
    return ret;
#endif
}

static int lingxin_cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifdef CONFIG_VOICE_LINK_LINGXIN
    uint32_t gate_generation = 0U;
    bool gate_open = lingxin_server_audio_uplink_gate_is_open(&gate_generation);
    shellPrint(shellGetCurrent(), "Lingxin enabled, initing=%d, starting=%d, chatting=%d, texting=%d, probing=%d, sm_state=%d, uplink_gate=%s, gate_gen=%u\n",
               s_lingxin_shell_initing, s_lingxin_shell_starting, s_lingxin_shell_chatting,
               s_lingxin_shell_texting, s_lingxin_shell_probing,
               lingxin_server_chat_state(), gate_open ? "open" : "closed",
               (unsigned)gate_generation);
#if defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    VoiceChatTransportStats stats;
    voice_chat_get_transport_stats(&stats);
    shellPrint(shellGetCurrent(), "round_epoch=%u start_seq=%u ack=%d tx_frames=%u tx_bytes=%u rx_chunks=%u sending=%d terminating=%d\n",
               stats.epoch, stats.start_seq, stats.task_started, stats.upload_frames,
               stats.upload_bytes, stats.download_chunks, stats.audio_sending, stats.terminating);
    lingxin_playback_stats_t playback;
    lingxin_audio_get_playback_stats(&playback);
    shellPrint(shellGetCurrent(), "play_gen=%u chunks=%u bytes=%u pcm=%u playing=%d started=%d suppressed=%d failed=%d ended=%d\n",
               playback.generation, playback.chunks, playback.bytes, playback.pcm_bytes,
               playback.playing, playback.started, playback.suppressed, playback.failed, playback.ended);
#ifdef CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX
    lingxin_capture_stats_t capture[LINGXIN_CAPTURE_STATS_SLOTS];
    lingxin_clean_capture_get_stats(capture);
    for (unsigned i = 0; i < LINGXIN_CAPTURE_STATS_SLOTS; ++i) {
        if (!capture[i].frames) continue;
        shellPrint(shellGetCurrent(), "cap_gen=%u frames=%u accepted_ms=%u peak=%u signal_frames=%u speech_ms=%u vad=%u/%u win=%u\n",
                   capture[i].generation, capture[i].frames, capture[i].accepted_samples / 16U,
                   capture[i].accepted_peak, capture[i].accepted_signal_frames, capture[i].speech_samples / 16U,
                   capture[i].vad_positive, capture[i].vad_checked, capture[i].vad_window_peak);
        shellPrint(shellGetCurrent(), "cap_gen=%u gate_drop=%u gated_peak=%u capture_drop=%u max_queue_ms=%u\n",
                   capture[i].generation,
                   capture[i].gate_drops, capture[i].gated_peak, capture[i].capture_drops,
                   capture[i].max_queued / 32U);
    }
#endif
#endif
#else
    shellPrint(shellGetCurrent(), "Lingxin disabled\n");
#endif
    return 0;
}

static int lingxin_cmd_playback(int argc, char **argv)
{
    (void)argc; (void)argv;
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    lingxin_playback_stats_t p = {0};
    lingxin_audio_get_playback_stats(&p);
    shellPrint(shellGetCurrent(), "playback gen=%u chunks=%u rx_bytes=%u decoded_pcm=%u written_pcm=%u queued=%u peak=%u failures=%u\n",
               p.generation, p.chunks, p.bytes, p.pcm_bytes, p.pcm_written,
               p.queued_bytes, p.queue_peak_bytes, p.failures);
    shellPrint(shellGetCurrent(), "playing=%u started=%u failed=%u cloud_end=%u finish_submitted=%u writer_exited=%u cleanup_pending=%u input_ready=%u\n",
               p.playing, p.started, p.failed, p.ended, p.finish_submitted, p.writer_exited, p.cleanup_pending, p.input_ready);
#endif
    return 0;
}

static int lingxin_cmd_cue(int argc, char **argv)
{
    (void)argc; (void)argv;
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    voice_upload_cue_stats_t c = {0};
    voice_player_get_upload_cue_stats(&c);
    shellPrint(shellGetCurrent(), "cue requested=%u duplicate=%u started=%u complete=%u cancel=%u stopped=%u last=%u pending=%u playing=%u\n",
               c.valid_requests, c.duplicate_requests, c.started, c.completed, c.cancelled, c.stopped,
               c.last_epoch, c.pending_epoch, c.playing_epoch);
    shellPrint(shellGetCurrent(), "skip stale=%u busy=%u callbacks=%u tts=%u enqueue_fail=%u play_fail=%u wait_timeout=%u\n",
               c.skipped_stale, c.skipped_busy, c.skipped_callbacks, c.skipped_tts,
               c.enqueue_failures, c.playback_failures, c.wait_timeouts);
#endif
    return 0;
}

static int lingxin_cmd_goodbye(int argc, char **argv)
{
    (void)argc; (void)argv;
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    lingxin_goodbye_stats_t s = {0};
    lingxin_server_get_goodbye_stats(&s);
    shellPrint(shellGetCurrent(), "goodbye received=%u last_gen=%u last_epoch=%u pending=%u pending_gen=%u\n",
               s.received, s.last_received_generation, s.last_received_epoch,
               s.pending, s.pending_generation);
    shellPrint(shellGetCurrent(), "exit_requests=%u last_gen=%u publish_fail=%u timeouts=%u mcp_handled=%u stop_ret=%d\n",
               s.dispatched, s.last_dispatched_generation, s.publish_failures,
               s.timeouts, s.handled, s.last_stop_result);
    shellPrint(shellGetCurrent(), "session_exits=%u last_gen=%u exit_code=%d state=%d\n",
               s.session_exits, s.last_exit_generation, s.last_exit_code,
               lingxin_server_chat_state());
#endif
    return 0;
}

static int lingxin_cmd_transport(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_LINGXIN_ENGINE_ENABLE) && !defined(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
    WebsocketTransportStats stats;
    voice_chat_get_websocket_stats(&stats);
    shellPrint(shellGetCurrent(), "ws connected=%d local_enqueued=%u/%u local_written_bytes=%u completed_frames=%u\n",
               stats.connected, stats.enqueued_frames, stats.enqueued_bytes,
               stats.written_bytes, stats.completed_frames);
    shellPrint(shellGetCurrent(), "ws queue=%u peak=%u full=%u pending=%u send_max_ms=%u receive_max_ms=%u\n",
               stats.queue_current, stats.queue_peak, stats.queue_full, stats.pending_bytes,
               stats.send_max_ms, stats.receive_max_ms);
    shellPrint(shellGetCurrent(), "ws wait_count=%u total_ms=%u max_ms=%u timeout=%u cancel=%u\n",
               stats.wait_count, stats.wait_total_ms, stats.wait_max_ms, stats.wait_timeout, stats.wait_cancel);
    shellPrint(shellGetCurrent(), "Local socket/TLS acceptance only, not cloud receipt; waits are lifetime totals.\n");
#else
    shellPrint(shellGetCurrent(), "Lingxin transport diagnostics unavailable\n");
#endif
    return 0;
}

static int lingxin_cmd_config(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifdef CONFIG_VOICE_LINK_LINGXIN
    Shell *shell = shellGetCurrent();
    int provisioned = lingxin_server_config_provisioned();
    shellPrint(shell, "provisioned=%d\n", provisioned);
    shellPrint(shell, "app_id=%s\n", lingxin_server_config_app_id());
    shellPrint(shell, "license=%s\n", lingxin_server_config_license());
    shellPrint(shell, "sn=%s\n", lingxin_server_config_sn());
    shellPrint(shell, "app_code=%s\n", lingxin_server_config_app_code());
    shellPrint(shell, "device_code=%s\n", lingxin_server_config_device_code());
    return lingxin_server_config_ready() ? 0 : -1;
#else
    shellPrint(shellGetCurrent(), "Lingxin disabled\n");
    return -1;
#endif
}

static int lingxin_cmd_config_set(int argc, char **argv)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (argc != 5) {
        shellPrint(shellGetCurrent(),
                   "Usage: lingxin config-set <app_id> <license> <sn> <app_code> <device_code>\n");
        return -1;
    }

    int ret = lingxin_server_config_set(argv[0], argv[1], argv[2], argv[3], argv[4]);
    shellPrint(shellGetCurrent(), "Lingxin config persist %s\n", ret == 0 ? "success" : "failed");
    return ret;
#else
    (void)argc;
    (void)argv;
    shellPrint(shellGetCurrent(), "Lingxin disabled\n");
    return -1;
#endif
}

static int lingxin_cmd_config_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifdef CONFIG_VOICE_LINK_LINGXIN
    /* Close any active cloud session before invalidating the credentials it
     * owns. The stop result is deliberately non-fatal for factory recovery. */
    (void)lingxin_server_chat_stop();
    int ret = lingxin_server_config_clear();
    shellPrint(shellGetCurrent(), "Lingxin config clear %s\n", ret == 0 ? "success" : "failed");
    return ret;
#else
    shellPrint(shellGetCurrent(), "Lingxin disabled\n");
    return -1;
#endif
}

static int lingxin_cmd_config_reload(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#ifdef CONFIG_VOICE_LINK_LINGXIN
    int ret = lingxin_server_config_reload();
    shellPrint(shellGetCurrent(), "Lingxin config reload %s\n", ret == 0 ? "success" : "failed");
    return ret;
#else
    shellPrint(shellGetCurrent(), "Lingxin disabled\n");
    return -1;
#endif
}

static int lingxin_cmd_trace(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_VOICE_LINK_LINGXIN
    shellPrint(shellGetCurrent(), "Lingxin disabled\n");
    return -1;
#else
    char *trace = NULL;
    int ret = lingxin_trace_get(&trace);
    if (ret != 0 || trace == NULL) {
        shellPrint(shellGetCurrent(), "Lingxin trace empty\n");
        return -1;
    }
    shellPrint(shellGetCurrent(), "Lingxin trace: %s\n", trace);
    lisa_mem_free(trace);
    return 0;
#endif
}

static int lingxin_cmd_help(int argc, char **argv);

#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_APP_CODEX_VOICE_INPUT)
static int lingxin_cmd_asr_result(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lingxin_asr_probe_t result;
    lingxin_server_asr_probe_read(&result);
    shellPrint(shellGetCurrent(), "ASR probe active=%d final=%d agent_text=%d actions=%d\n",
               result.active, result.final_received, result.agent_text, result.actions);
    if (result.final_received) {
        if (result.vad_to_final_ms != UINT32_MAX)
            shellPrint(shellGetCurrent(), "cloud_vad_to_final_ms=%u\n", (unsigned)result.vad_to_final_ms);
        shellPrint(shellGetCurrent(), "ASR text: %s\n", result.text);
    }
    return 0;
}
#endif

static const struct listen_cmd_t g_lingxin_cmds[] = {
#if defined(CONFIG_VOICE_LINK_LINGXIN) && defined(CONFIG_APP_CODEX_VOICE_INPUT)
    {"asr-result", lingxin_cmd_asr_result, "Read the last office-mode ASR probe result"},
#endif
    {"init", lingxin_cmd_init, "Init Lingxin only, ex: lingxin init"},
    {"chat", lingxin_cmd_chat, "Start Lingxin chat on current shell thread, ex: lingxin chat"},
    {"start", lingxin_cmd_start, "Start Lingxin chat, or office diagnostic: lingxin start asr-probe"},
    {"probe", lingxin_cmd_probe, "Start Lingxin raw websocket probe, ex: lingxin probe"},
    {"text", lingxin_cmd_text, "Start Lingxin text chat, ex: lingxin text hello"},
    {"stop", lingxin_cmd_stop, "Stop Lingxin chat, ex: lingxin stop"},
    {"end", lingxin_cmd_end, "End current Lingxin recording, ex: lingxin end"},
    {"status", lingxin_cmd_status, "Show Lingxin status, ex: lingxin status"},
    {"transport", lingxin_cmd_transport, "Show local websocket queue/write diagnostics"},
    {"playback", lingxin_cmd_playback, "Show decoder/player progress and cleanup state"},
    {"cue", lingxin_cmd_cue, "Show VAD cue counters (no audio content)"},
    {"goodbye", lingxin_cmd_goodbye, "Show goodbye receipt, exit dispatch and completion counters"},
    {"config", lingxin_cmd_config, "Show persisted Lingxin config"},
    {"config-set", lingxin_cmd_config_set, "Persist Lingxin app_id/license/sn/app_code/device_code"},
    {"config-clear", lingxin_cmd_config_clear, "Delete persisted Lingxin factory config"},
    {"config-reload", lingxin_cmd_config_reload, "Reload persisted Lingxin config without reboot"},
    {"trace", lingxin_cmd_trace, "Show Lingxin last trace, ex: lingxin trace"},
    {"help", lingxin_cmd_help, "Show help information"},
};

static int lingxin_cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    shellPrint(shellGetCurrent(), "Lingxin commands:\n");
    for (int i = 0; i < sizeof(g_lingxin_cmds) / sizeof(g_lingxin_cmds[0]); i++) {
        shellPrint(shellGetCurrent(), "%-10s\t:\t%s\n", g_lingxin_cmds[i].name, g_lingxin_cmds[i].help);
    }
    return 0;
}

static int lingxin_cmd_handler(int argc, char **argv)
{
    if (argc == 1) {
        return lingxin_cmd_help(argc, argv);
    }

    for (int i = 0; i < sizeof(g_lingxin_cmds) / sizeof(g_lingxin_cmds[0]); i++) {
        if (strcmp(g_lingxin_cmds[i].name, argv[1]) == 0) {
            return g_lingxin_cmds[i].exec(argc - 2, argv + 2);
        }
    }

    return lingxin_cmd_help(argc, argv);
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN, lingxin,
                 lingxin_cmd_handler, Lingxin test commands);
