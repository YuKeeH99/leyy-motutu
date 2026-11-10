#include "stdint.h"
#include "stddef.h"
#include "FreeRTOS.h"
#include "task.h"

#define TAG "voice.app.wakeup"
#include "voice_msg.h"
#include "app_datas.h"
#include "sys_init.h"
#include "sys_network_manager.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_v2_voice_standby.h"
#include "service_offline_action.h"
#include "service_voice_action.h"
#endif

#include "voice_cloud.h"
#ifdef CONFIG_VOICE_LINK_LINGXIN
#include "app_netcfg_portal.h"
#include "lingxin_server.h"
#include "lingxin_server_config.h"
#include "lisa_thread.h"
#include "lisa_typedef.h"
#endif
#include "service_image.h"
#include "service_button.h"

#include "app_player.h"
#include "tone.h"
#include "voice_player_comm.h"
#include "lisa_log.h"

__attribute__((weak)) bool app_voice_interaction_blocked(void)
{
    return false;
}

enum {
    VOICE_WAKEUP_QR_STATUS_NOT_CONNECTED = 0,
    VOICE_WAKEUP_QR_STATUS_BIND = 3,
    VOICE_WAKEUP_QR_STATUS_AUTH_FAILED = 4,
    VOICE_WAKEUP_QR_STATUS_LINGXIN_CONFIG_REQUIRED = 5,
};

static bool is_valid_keyword(char *keyword)
{
    if (keyword == NULL) {
        return false;
    }

    return true;
}

static bool voice_wakeup_should_open_info(uint32_t *status_out)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (!lingxin_server_config_provisioned()) {
        if (status_out) {
            *status_out = VOICE_WAKEUP_QR_STATUS_LINGXIN_CONFIG_REQUIRED;
        }
        return true;
    }
    return false;
#else
    uint32_t status = 0xFFFFFFFFU;

    switch (voice_cloud_get_state()) {
    case VOICE_CLOUD_STATE_CONNECTING:
        return false;
    case VOICE_CLOUD_STATE_NO_NETWORK:
    case VOICE_CLOUD_STATE_NO_INTERNET:
        status = VOICE_WAKEUP_QR_STATUS_NOT_CONNECTED;
        break;
    case VOICE_CLOUD_STATE_TOKEN_FAILED:
        status = VOICE_WAKEUP_QR_STATUS_AUTH_FAILED;
        break;
    case VOICE_CLOUD_STATE_CONNECT_FAILED:
        status = VOICE_WAKEUP_QR_STATUS_BIND;
        break;
    default:
        return false;
    }

    if (status_out) {
        *status_out = status;
    }

    return true;
#endif
}

static void voice_wakeup_prompt_cloud_info(uint32_t status)
{
#ifdef CONFIG_VOICE_LINK_LINGXIN
    static TickType_t s_last_lingxin_config_prompt_tick;
    TickType_t lingxin_now = xTaskGetTickCount();

    if (status == VOICE_WAKEUP_QR_STATUS_LINGXIN_CONFIG_REQUIRED) {
        if (s_last_lingxin_config_prompt_tick == 0 ||
            (lingxin_now - s_last_lingxin_config_prompt_tick) >= pdMS_TO_TICKS(2000)) {
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_111));
            s_last_lingxin_config_prompt_tick = lingxin_now;
        }
        voice_msg_pub(VOICE_MSG_CLOUD_OPEN_INFO, &status, sizeof(status));
        return;
    }
#endif
#if CONFIG_WIFI_MANAGER
    static TickType_t s_last_netcfg_prompt_tick = 0;
    TickType_t now = xTaskGetTickCount();

    switch (status) {
    case VOICE_WAKEUP_QR_STATUS_AUTH_FAILED:
        voice_player_play_tone_url(app_tone_get_url(TONE_ID_105));
        break;
    case VOICE_WAKEUP_QR_STATUS_NOT_CONNECTED:
        if (s_last_netcfg_prompt_tick == 0 ||
            (now - s_last_netcfg_prompt_tick) >= pdMS_TO_TICKS(2000)) {
            voice_player_play_tone_url(app_tone_get_url(TONE_ID_70));
            s_last_netcfg_prompt_tick = now;
        }
        break;
    default:
        break;
    }

    voice_msg_pub(VOICE_MSG_CLOUD_OPEN_INFO, &status, sizeof(status));
#else
    (void)status;
#endif
}

static bool voice_wakeup_is_button_ptt_action(voice_msg_button_action_t action)
{
    return action == VOICE_MSG_BUTTON_ACTION_PRESS_DOWN ||
           action == VOICE_MSG_BUTTON_ACTION_SHORT_UP ||
           action == VOICE_MSG_BUTTON_ACTION_LONG_UP ||
           action == VOICE_MSG_BUTTON_ACTION_LONG_HOLD_UP;
}

static bool start_voice_cloud(struct app_datas *app_datas)
{
    sys_network_status_t status;

    if (app_datas == NULL) {
        LOGW("Invalid app_datas");
        return false;
    }

    if (sys_network_get_status(&status) != 0 || !status.connected) {
        LOGW("Network not connected");
        return false;
    }

    if (app_datas->voice_cloud_connected == 0) {
        LOGW("Voice cloud not connected");
        return false;
    }

    if (app_datas->can_wakeup == 0) {
        LOGI("ignore wakeup msg");
        return false;
    }

    const char *keywords[] = {"小聆小聆"};
    struct voice_cloud_chat_config chat_config = {
        .full_duplex = app_interaction_mode_is_continuous(app_datas->int_mode),
        .timeout_ms = app_datas->full_duplex_timeout_ms,
        .oneshot = app_interaction_mode_is_continuous(app_datas->int_mode) ? false : app_datas->oneshot,
        .words = (char **)keywords,
        .words_cnt = 1,
    };

    return voice_cloud_chat_start(&chat_config) == 0;
}

#ifdef CONFIG_VOICE_LINK_LINGXIN
static volatile bool s_lingxin_starting;
static volatile bool s_lingxin_wifi_recovering;
static volatile uint32_t s_lingxin_wakeup_request_seq;
static uint32_t s_lingxin_reserved_seq, s_lingxin_reserved_owner, s_lingxin_pending_owner;
#define LINGXIN_START_TRANSITION_RETRY_MAX 8
#define LINGXIN_START_TRANSITION_RETRY_DELAY_MS 150
#define LINGXIN_WIFI_RECONNECT_TIMEOUT_MS 15000
#define LINGXIN_WIFI_RECONNECT_POLL_MS 200

static bool lingxin_network_is_ready(sys_network_status_t *status_out)
{
    sys_network_status_t status = {0};

    if (sys_network_get_status(&status) != 0 || !status.connected) {
        if (status_out != NULL) {
            *status_out = status;
        }
        return false;
    }

    if ((status.active_bearer == SYS_NETWORK_BEARER_WIFI && !status.wifi_connected) ||
        (status.active_bearer == SYS_NETWORK_BEARER_MODEM && !status.modem_connected) ||
        status.active_bearer == SYS_NETWORK_BEARER_NONE) {
        if (status_out != NULL) {
            *status_out = status;
        }
        return false;
    }

    if (status_out != NULL) {
        *status_out = status;
    }
    return true;
}

static uint32_t lingxin_wakeup_request_snapshot(void)
{
    uint32_t seq;

    taskENTER_CRITICAL();
    seq = s_lingxin_wakeup_request_seq;
    taskEXIT_CRITICAL();
    return seq;
}

static uint32_t lingxin_wakeup_request_register(bool wifi_recovery_needed,
                                                bool *start_worker,
                                                bool *retriggered,
                                                uint32_t owner_generation)
{
    uint32_t seq;

    taskENTER_CRITICAL();
    s_lingxin_wakeup_request_seq++;
    if (s_lingxin_wakeup_request_seq == 0U) {
        s_lingxin_wakeup_request_seq = 1U;
    }
    seq = s_lingxin_wakeup_request_seq;
    s_lingxin_pending_owner = owner_generation;
    *start_worker = !s_lingxin_starting;
    *retriggered = s_lingxin_starting &&
                   (s_lingxin_wifi_recovering || wifi_recovery_needed);
    if (*start_worker) {
        s_lingxin_reserved_seq = seq;
        s_lingxin_reserved_owner = owner_generation;
        s_lingxin_starting = true;
        s_lingxin_wifi_recovering = wifi_recovery_needed;
    } else if (*retriggered) {
        /* The link may disappear while an online start transition is still
         * running.  Preserve that offline wakeup for the existing worker. */
        s_lingxin_wifi_recovering = true;
    }
    taskEXIT_CRITICAL();

    return seq;
}

static void lingxin_start_worker_reserved(uint32_t *seq, uint32_t *owner)
{
    taskENTER_CRITICAL();
    *seq = s_lingxin_reserved_seq;
    *owner = s_lingxin_reserved_owner;
    taskEXIT_CRITICAL();
}

static bool lingxin_start_worker_finish_or_take_pending(uint32_t *handled_seq, uint32_t *owner_generation)
{
    bool retry_pending = false;

    taskENTER_CRITICAL();
    if (s_lingxin_wakeup_request_seq != *handled_seq) {
        *handled_seq = s_lingxin_wakeup_request_seq;
        *owner_generation = s_lingxin_pending_owner;
        retry_pending = true;
    } else {
        s_lingxin_starting = false;
        s_lingxin_wifi_recovering = false;
    }
    taskEXIT_CRITICAL();

    return retry_pending;
}

static int start_lingxin_chat(struct app_datas *app_datas, uint32_t owner_generation)
{
    sys_network_status_t status;

    if (app_datas == NULL) {
        LOGW("[Lingxin] start chat blocked: invalid app_datas");
        return -1;
    }

    if (sys_network_get_status(&status) != 0 || !status.connected) {
        LOGW("[Lingxin] start chat blocked: network not ready");
        return -1;
    }

    LOGI("[Lingxin] start chat precheck ok: bearer=%d connected=%d can_wakeup=%d voice_mode=0x%02x",
         status.active_bearer,
         status.connected ? 1 : 0,
         app_datas->can_wakeup,
         app_datas->voice_work_mode);

    if (app_datas->can_wakeup == 0) {
        LOGI("[Lingxin] start chat blocked: can_wakeup=0");
        return -1;
    }

    int ret = lingxin_server_chat_start_owned(owner_generation);
    if (ret != 0) {
        LOGE("[Lingxin] start chat failed ret=%d", ret);
        return ret;
    }

    LOGI("[Lingxin] start chat ok");
    return 0;
}

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
static bool lingxin_wait_query_idle(uint32_t owner_generation)
{
    for (unsigned waited = 0; waited < 3000U; waited += 50U) {
        if (!service_offline_action_online_current(owner_generation)) return false;
        if (service_voice_action_query_idle()) return true;
        lisa_thread_mdelay(50U);
    }
    return service_offline_action_online_current(owner_generation) && service_voice_action_query_idle();
}
#endif

static void lingxin_start_thread(void *arg)
{
    uint32_t owner_generation, handled_seq;
    lingxin_start_worker_reserved(&handled_seq, &owner_generation);
    struct app_datas *app_datas = get_app_datas();
    sys_network_status_t status;

    (void)arg;
    LOGI("[Lingxin] async start thread enter");
    lingxin_server_diag_timing_mark("async_start_thread_enter", 0);

    do {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        if (!service_offline_action_online_current(owner_generation)) continue;
#endif
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        /* This is a worker, not the event bus: let the standby owner finish
         * folding/power restoration before any scan or cloud request. */
        int resume_ret = service_v2_voice_standby_resume_wait(20000U);
        if (resume_ret != 0) {
            LOGW("[Lingxin] standby resume failed: %d; retry on next wakeup", resume_ret);
            continue;
        }
#endif
        bool network_was_ready_at_entry = lingxin_network_is_ready(&status);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        /* An online start never converts old audio/tasks to offline ownership. */
        if (!service_offline_action_online_current(owner_generation) || !network_was_ready_at_entry) continue;
#endif
        bool network_ready = network_was_ready_at_entry;
        bool portal_handed_off = false;

        if (!network_was_ready_at_entry) {
            bool reconnect_attempted;
            int reconnect_ret;
            int waited_ms = 0;

            taskENTER_CRITICAL();
            s_lingxin_wifi_recovering = true;
            taskEXIT_CRITICAL();

            if (app_netcfg_portal_is_active()) {
                int portal_stop_ret;

                LOGI("[Lingxin] offline wakeup hands provisioning AP to station recovery");
                lingxin_server_diag_timing_mark("wifi_portal_handoff_begin", 0);
                portal_stop_ret = app_netcfg_portal_stop();
                if (portal_stop_ret == 0) {
                    portal_handed_off = true;
                    LOGI("[Lingxin] provisioning AP stopped; station recovery may start");
                    lingxin_server_diag_timing_mark("wifi_portal_handoff_done", 0);
                } else {
                    LOGW("[Lingxin] provisioning AP handoff failed: %d", portal_stop_ret);
                    lingxin_server_diag_timing_mark("wifi_portal_handoff_failed",
                                                    portal_stop_ret);
                }
            }

            reconnect_ret = app_netcfg_portal_is_active()
                                ? -1
                                : sys_wifi_reconnect_saved_once_start();
            reconnect_attempted = reconnect_ret >= 0;
            LOGI("[Lingxin] offline wakeup seq=%u starts WiFi recovery: ret=%d timeout=%dms",
                 (unsigned)handled_seq, reconnect_ret, LINGXIN_WIFI_RECONNECT_TIMEOUT_MS);
            lingxin_server_diag_timing_mark("wifi_reconnect_requested", reconnect_ret);

            while (reconnect_attempted) {
                uint32_t requested_seq = lingxin_wakeup_request_snapshot();

                if (requested_seq != handled_seq) {
                    handled_seq = requested_seq;
                    sys_wifi_reconnect_saved_once_stop();
                    reconnect_ret = sys_wifi_reconnect_saved_once_start();
                    reconnect_attempted = reconnect_ret >= 0;
                    waited_ms = 0;
                    LOGI("[Lingxin] offline wakeup seq=%u retriggers saved-AP scan: ret=%d",
                         (unsigned)handled_seq, reconnect_ret);
                    lingxin_server_diag_timing_mark("wifi_reconnect_retriggered", reconnect_ret);
                    if (!reconnect_attempted) {
                        break;
                    }
                }

                if (lingxin_network_is_ready(&status)) {
                    network_ready = true;
                    break;
                }
                if (sys_wifi_reconnect_saved_exhausted() || sys_wifi_radio_suspended() ||
                    app_netcfg_portal_is_active()) {
                    break;
                }
                if (waited_ms >= LINGXIN_WIFI_RECONNECT_TIMEOUT_MS) {
                    break;
                }

                lisa_thread_mdelay(LINGXIN_WIFI_RECONNECT_POLL_MS);
                waited_ms += LINGXIN_WIFI_RECONNECT_POLL_MS;
            }

            if (reconnect_attempted && network_ready) {
                sys_wifi_reconnect_saved_once_stop();
            }

            if (network_ready) {
                lingxin_server_diag_timing_mark("wifi_reconnect_success", status.active_bearer);
                LOGI("[Lingxin] WiFi recovery succeeded at seq=%u; acknowledge and start chat",
                     (unsigned)handled_seq);
                voice_player_play_network_recovery_tone();
            } else {
                lingxin_server_diag_timing_mark("wifi_reconnect_timeout", reconnect_ret);
                LOGW("[Lingxin] WiFi foreground wait ended at seq=%u; background cycle owns remaining APs",
                     (unsigned)handled_seq);
                /* A 15-second foreground budget is NOT proof that all saved
                 * APs failed. Leave the SDK cycle running; main's exhausted
                 * guard owns automatic provisioning after a completed cycle. */
                if (portal_handed_off && !reconnect_attempted &&
                    !sys_wifi_has_ap() && !sys_wifi_radio_suspended() &&
                    !app_netcfg_portal_is_active()) {
                    int portal_ret = app_netcfg_portal_start(
                        APP_NETCFG_PORTAL_REASON_CONNECT_FAILED);
                    LOGI("[Lingxin] restore provisioning AP after failed station recovery: %d",
                         portal_ret);
                    lingxin_server_diag_timing_mark("wifi_portal_restored",
                                                    portal_ret);
                }
            }
        }
        if (network_ready) {
            int start_ret = -1;

            /* The offline prompt may have finished seconds ago. Never reuse
             * its already-open microphone gate for the new acknowledgement. */
            if (!network_was_ready_at_entry &&
                voice_player_request_reconnected_wakeup_tone() != 0) {
                LOGW("[Lingxin] reconnect acknowledgement could not be queued");
                continue;
            }

            taskENTER_CRITICAL();
            s_lingxin_wifi_recovering = false;
            taskEXIT_CRITICAL();
            lingxin_server_diag_timing_mark("network_ready", status.active_bearer);
            for (int attempt = 0; attempt < LINGXIN_START_TRANSITION_RETRY_MAX; attempt++) {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                if (!lingxin_wait_query_idle(owner_generation)) break;
#endif
                LOGI("[Lingxin] async start thread call start_lingxin_chat, attempt=%d/%d",
                     attempt + 1,
                     LINGXIN_START_TRANSITION_RETRY_MAX);
                start_ret = start_lingxin_chat(app_datas, owner_generation);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                if (!service_offline_action_online_current(owner_generation)) {
                    /* A newer wake owns cleanup. An old worker must not stop it. */
                    start_ret = -2;
                    break;
                }
#endif
                if (start_ret == 0) {
                    break;
                }
                if (start_ret != -2) {
                    break;
                }
                LOGW("[Lingxin] chat start deferred by transitional state, retry after %d ms",
                     LINGXIN_START_TRANSITION_RETRY_DELAY_MS);
                lisa_thread_mdelay(LINGXIN_START_TRANSITION_RETRY_DELAY_MS);
            }
            if (start_ret != 0) {
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
                if (start_ret != -2) service_offline_action_cloud_failed(owner_generation);
#endif
                LOGE("[Lingxin] async start thread failed after retry, ret=%d", start_ret);
            }
        }
    } while (lingxin_start_worker_finish_or_take_pending(&handled_seq, &owner_generation));

    LOGI("[Lingxin] async start thread exit");
}

static void start_lingxin_chat_async_for_owner(const char *reason, uint32_t owner_generation)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(owner_generation)) return;
#endif
    sys_network_status_t status = {0};
    bool start_worker = false;
    bool retriggered = false;
    bool wifi_recovery_needed = !lingxin_network_is_ready(&status);
    uint32_t request_seq;
    lisa_thread_attr_t attr = {
        .name = (uint8_t *)"lingxin.start",
        .stack_size = 12 * 1024,
        .priority = LISA_OS_PRIORITY_NORMAL,
    };

    request_seq = lingxin_wakeup_request_register(wifi_recovery_needed,
                                                  &start_worker,
                                                  &retriggered, owner_generation);
    LOGI("[Lingxin] wakeup request seq=%u reason=%s internet=%d wifi_link=%d bearer=%d",
         (unsigned)request_seq, reason ? reason : "unknown",
         status.connected ? 1 : 0, status.wifi_connected ? 1 : 0,
         (int)status.active_bearer);

    if (!start_worker) {
        if (retriggered) {
            LOGI("[Lingxin] WiFi recovery retrigger queued: seq=%u reason=%s",
                 (unsigned)request_seq, reason ? reason : "unknown");
        } else {
            LOGI("[Lingxin] start transition already active: seq=%u reason=%s",
                 (unsigned)request_seq, reason ? reason : "unknown");
        }
        return;
    }

    LOGI("[Lingxin] schedule async start thread, reason=%s", reason ? reason : "unknown");
    lingxin_server_diag_timing_mark("async_start_scheduled", 0);
    if (lisa_thread_create(&attr, lingxin_start_thread, NULL) == NULL) {
        taskENTER_CRITICAL();
        s_lingxin_starting = false;
        s_lingxin_wifi_recovering = false;
        taskEXIT_CRITICAL();
        LOGE("[Lingxin] create async start thread failed");
    } else {
        LOGI("[Lingxin] async start thread created, reason=%s", reason ? reason : "unknown");
    }
}

static void start_lingxin_chat_async(const char *reason)
{
    uint32_t owner_generation = 0U;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    owner_generation = service_offline_action_generation();
#endif
    start_lingxin_chat_async_for_owner(reason, owner_generation);
}
#endif

static bool voice_wakeup_tts_interrupt_needed(void)
{
    app_player_state_t state;

    if (tts_player == NULL) {
        return false;
    }

    state = app_player_get_state(tts_player);
    return state == APP_PLAYER_STATE_PREPARING ||
           state == APP_PLAYER_STATE_PREPARED ||
           state == APP_PLAYER_STATE_PLAYING ||
           state == APP_PLAYER_STATE_PAUSED;
}

static void interrupt_photo_flow(const char *source)
{
    LOGI("interrupt photo flow by %s", source ? source : "unknown");
    if (voice_wakeup_tts_interrupt_needed()) {
        if (app_player_stop(tts_player) == APP_PLAYER_OK) {
            LOGI("interrupt photo flow: stopped active tts");
        } else {
            LOGW("interrupt photo flow: stop tts failed");
        }
    }
    voice_msg_pub(VOICE_MSG_APP_CAMERA_PREVIEW_EXIT, NULL, 0);
}

static void voice_wakeup_keyword(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (!service_offline_action_online_current(service_offline_action_generation())) return;
#endif
    LOGI("voice_wakeup_keyword, keyword: %s", (char *)data);
    struct app_datas *app_datas = get_app_datas();

    if (!is_valid_keyword((char *)data)) {
        LOGW("Invalid keyword: %s", (char *)data);
        return;
    }

    if ((app_datas->voice_work_mode & VOICE_WORK_MODE_VOICE_WAKEUP) == 0) {
        LOGW("ignore wakeup evt, voice work mode: %d", app_datas->voice_work_mode);
        return;
    }

    if (app_voice_interaction_blocked()) {
        LOGI("ignore wakeup keyword: voice interaction blocked");
        return;
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    (void)service_v2_voice_standby_wake_pose(false);
#endif

    uint32_t info_status = 0;
    if (voice_wakeup_should_open_info(&info_status)) {
        LOGI("voice wakeup ignored: cloud unavailable status=%u", (unsigned)info_status);
        voice_wakeup_prompt_cloud_info(info_status);
        return;
    }

    service_image_waiting_cancel();
    interrupt_photo_flow("voice wakeup");
#ifdef CONFIG_VOICE_LINK_LINGXIN
    LOGI("[Lingxin] keyword wakeup accepted, start async chat");
    lingxin_server_diag_timing_begin("keyword_wakeup");
    lingxin_server_diag_timing_mark("keyword_wakeup_accepted", 0);
    /* Close capture before the asynchronous chat can open its recorder.  The
     * player subscriber closes it again with the generation it will release
     * after the local acknowledgement finishes plus the 200 ms tail guard. */
    (void)lingxin_server_audio_uplink_gate_close("keyword_wakeup_accepted");
    start_lingxin_chat_async("keyword");
#else
    start_voice_cloud(app_datas);
#endif
}

static void voice_wakeup_button_event(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    (void)unused;
    (void)msg_id;
    (void)len;
    (void)user_data;

    if (data == NULL || len < sizeof(voice_msg_button_evt_t)) {
        LOGW("Invalid button evt payload");
        return;
    }

    voice_msg_button_evt_t *evt = (voice_msg_button_evt_t *)data;
    LOGI("button evt id=%d action=%d", evt->button_id, evt->action);

#ifdef CONFIG_VOICE_LINK_LINGXIN
    /*
     * FlexibleButton reports SHORT_UP for every release before it knows
     * whether the sequence is a multi-click.  In Lingxin mode the application
     * layer owns CLICK and TRIPLE_CLICK (PTT and provisioning respectively),
     * so consuming SHORT_UP here would start a cloud chat during a triple
     * click.  Keep this legacy PTT subscriber disabled for Lingxin.
     */
    return;
#endif

    /* only k1 (button0) is configured for PTT */
    if (evt->button_id != 0) {
        LOGI("ignore button_id=%d", evt->button_id);
        return;
    }

    if (!voice_wakeup_is_button_ptt_action(evt->action)) {
        return;
    }

    if (app_voice_interaction_blocked()) {
        LOGI("ignore button wakeup: voice interaction blocked");
        return;
    }

    uint32_t info_status = 0;
    if (voice_wakeup_should_open_info(&info_status)) {
        if (evt->action == VOICE_MSG_BUTTON_ACTION_SHORT_UP) {
            LOGI("button wakeup ignored: cloud unavailable status=%u", (unsigned)info_status);
            voice_wakeup_prompt_cloud_info(info_status);
        }
        return;
    }

#ifdef CONFIG_VOICE_LINK_LINGXIN
    if (evt->action == VOICE_MSG_BUTTON_ACTION_SHORT_UP) {
        voice_msg_pub(VOICE_MSG_WAKEUP_BUTTON_START, NULL, 0);
    }
    return;
#endif

    if (evt->action == VOICE_MSG_BUTTON_ACTION_PRESS_DOWN) {
        voice_msg_pub(VOICE_MSG_WAKEUP_BUTTON_START, NULL, 0);
    } else if (evt->action == VOICE_MSG_BUTTON_ACTION_SHORT_UP
        || evt->action == VOICE_MSG_BUTTON_ACTION_LONG_UP
        || evt->action == VOICE_MSG_BUTTON_ACTION_LONG_HOLD_UP) {
        voice_msg_pub(VOICE_MSG_WAKEUP_BUTTON_STOP, NULL, 0);
    }
}

static void voice_msg_btn_wakeup_start(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    if (!service_button_wakeup_event_current(data, len)) return;
    struct app_datas *app_datas = get_app_datas();
    if (app_datas == NULL) {
        return;
    }

    if ((app_datas->voice_work_mode & VOICE_WORK_MODE_BUTTON_WAKEUP) == 0) {
        LOGW("ignore button evt, voice work mode: %d", app_datas->voice_work_mode);
        return;
    }

    if (!app_datas->can_wakeup) {
        return;
    }

    if (app_voice_interaction_blocked()) {
        LOGI("ignore button wakeup start: voice interaction blocked");
        return;
    }

#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    /* Owned events were already claimed by the confirmed CLICK publisher.
     * NULL retains the legacy path. Never claim a second time for subscribers. */
    if (data == NULL && !service_offline_action_begin_button()) return;
    uint32_t owner_generation = data ?
        ((const service_button_wakeup_t *)data)->owner_generation : service_offline_action_generation();
    if (!service_offline_action_online_current(owner_generation)) return;
    (void)service_v2_voice_standby_wake_pose(false);
#else
    uint32_t owner_generation = 0U;
#endif

    uint32_t info_status = 0;
    if (voice_wakeup_should_open_info(&info_status)) {
        LOGI("button wakeup start ignored: cloud unavailable status=%u", (unsigned)info_status);
        voice_wakeup_prompt_cloud_info(info_status);
        return;
    }

    service_image_waiting_cancel();
    interrupt_photo_flow("button wakeup");
#ifdef CONFIG_VOICE_LINK_LINGXIN
    LOGI("[Lingxin] button wakeup accepted, start async chat");
    lingxin_server_diag_timing_begin("button_wakeup");
    lingxin_server_diag_timing_mark("button_wakeup_accepted", 0);
    (void)lingxin_server_audio_uplink_gate_close("button_wakeup_accepted");
    start_lingxin_chat_async_for_owner("button", owner_generation);
#else
    if (!app_datas->voice_cloud_connected) {
        return;
    }
    voice_cloud_audio_recognition_start();
#endif
}

static void voice_msg_btn_wakeup_stop(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    struct app_datas *app_datas = get_app_datas();
    if (app_datas == NULL) {
        return;
    }

    if ((app_datas->voice_work_mode & VOICE_WORK_MODE_BUTTON_WAKEUP) == 0) {
        LOGW("ignore button evt, voice work mode: %d", app_datas->voice_work_mode);
        return;
    }

    if (!app_datas->can_wakeup) {
        return;
    }

    if (app_voice_interaction_blocked()) {
        LOGI("ignore button wakeup stop: voice interaction blocked");
        return;
    }

#ifdef CONFIG_VOICE_LINK_LINGXIN
    LOGI("[Lingxin] button stop accepted: end record");
    lingxin_server_chat_end_record();
#else
    if (!app_datas->voice_cloud_connected) {
        return;
    }
    voice_cloud_audio_recognition_stop();
#endif
}

int voice_wakeup_evt_init(void)
{
    voice_msg_sub(VOICE_MSG_WAKEUP_KEYWORD, voice_wakeup_keyword, NULL);
    voice_msg_sub(VOICE_MSG_BUTTON_CHANGE, voice_wakeup_button_event, NULL);
    voice_msg_sub(VOICE_MSG_WAKEUP_BUTTON_START, voice_msg_btn_wakeup_start, NULL);
    voice_msg_sub(VOICE_MSG_WAKEUP_BUTTON_STOP, voice_msg_btn_wakeup_stop, NULL);

    return 0;
}

SYS_INIT(voice_wakeup_evt_init, SYS_INIT_LEVEL_PRE_APPLICATION, 50);
