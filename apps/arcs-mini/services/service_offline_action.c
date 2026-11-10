#include "service_offline_action.h"
#include "service_offline_action_policy.h"
#include "service_voice_action.h"
#include "service_v2_voice_standby.h"
#include "service_volume.h"
#include "../app_manual_netcfg.h"
#include "app_netcfg_portal.h"
#include "app_ble_common.h"
#include "app_datas.h"
#include "app_wakeup.h"
#include "acomp_wakeup.h"
#include "sys_network_manager.h"
#include "sys_network_diag.h"
#include "sys_wifi.h"
#include "lingxin_server.h"
#include "ota_manager.h"
#include "voice_msg.h"
#include "voice_player.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lisa_log.h"
#include "shell.h"
#include <string.h>
#include <errno.h>

#define TAG "offline.action"
#define OFFLINE_ENGINE_MS 600000U
extern bool app_voice_interaction_blocked(void);
extern int network_probe_start(void);
static offline_policy_t s_policy;
static SemaphoreHandle_t s_control;
static bool s_ready, s_diagnostic, s_failed, s_shutdown, s_armed, s_reset;
static bool s_wake_pending, s_stop_pending, s_ble_owned;
static bool s_motion_owned;
static char s_keyword[64];
static bool s_fallback_pending, s_wake_fallback;
static uint32_t s_fallback_generation, s_fallback_until;
static uint32_t s_accepted, s_dropped, s_started, s_finished;
/* Keep ingress evidence across queue drain, cancellation and portal exit. */
static uint32_t s_netcfg_esr_count, s_netcfg_button_count;
static const char *s_netcfg_last_source = "none";
static bool s_reported_active;
static bool s_transaction_busy, s_button_wake, s_local_return_online;
static uint32_t s_local_generation, s_connect_generation, s_connect_started;
static bool s_connect_owned;
/* Attempt state belongs only to offline.network. Ingress uses the mailbox. */
static bool s_online_pending, s_online_started, s_online_probe_started;
static bool s_online_explicit, s_connect_backoff, s_connect_reassociate;
static bool s_keep_connected, s_online_background;
static uint32_t s_network_steps, s_network_worker_max_ms;
static uint32_t s_connect_retry_at;
static bool s_connect_request, s_connect_request_explicit, s_connect_claimed;
static bool s_connect_wake_upgrade, s_online_wakeup;
static uint32_t s_connect_wake_epoch;
static uint32_t s_wake_skip_network_generation;
static bool s_wake_skip_network;
static uint32_t s_connect_request_epoch;
static uint32_t s_audio_generation;
static volatile uint32_t s_network_epoch;
static struct app_datas *s_app;
static uint32_t s_wake_received_ms, s_last_wake_ack_ms, s_max_wake_ack_ms;
static uint32_t s_worker_max_ms, s_worker_steps;

uint32_t service_offline_action_network_epoch(void)
{
    return s_network_epoch;
}

bool service_offline_action_forced_offline(void)
{
    taskENTER_CRITICAL();
    bool forced = s_policy.forced_offline || s_policy.local_request == OFFLINE_CMD_ENTER_OFFLINE;
    taskEXIT_CRITICAL();
    return forced;
}

bool service_offline_action_network_prompt_allowed(void)
{
    return !service_offline_action_forced_offline();
}

bool service_offline_action_network_current(uint32_t epoch)
{
    taskENTER_CRITICAL();
    bool valid = epoch == s_network_epoch && !s_shutdown && !s_diagnostic &&
                 !s_policy.forced_offline && s_policy.local_request != OFFLINE_CMD_ENTER_OFFLINE;
    taskEXIT_CRITICAL();
    return valid;
}

bool service_offline_action_network_busy(void)
{
    taskENTER_CRITICAL();
    bool busy = s_connect_request || s_connect_claimed || s_online_pending;
    taskEXIT_CRITICAL();
    return busy;
}

/* Capture at ingress: completion racing the prompt must not turn this wake
 * into a new network request or an extra network warning. */
bool service_offline_action_wake_network_allowed(uint32_t generation)
{
    taskENTER_CRITICAL();
    bool allowed = !(s_wake_skip_network && s_wake_skip_network_generation == generation);
    taskEXIT_CRITICAL();
    return allowed;
}

static void offline_capture_wake_network(void)
{
    /* Caller holds the ingress critical section. */
    s_wake_skip_network = s_connect_request || s_connect_claimed || s_online_pending;
    s_wake_skip_network_generation = s_policy.generation;
    if (s_wake_skip_network) {
        /* Upgrade only the existing attempt's portal handoff permission.
         * The network worker must not restart a live scan or skip backoff. */
        s_connect_wake_upgrade = true;
        s_connect_wake_epoch = s_network_epoch;
    }
}

bool service_offline_action_local_audio_current(uint32_t generation)
{
    taskENTER_CRITICAL();
    bool valid = !s_shutdown && !s_diagnostic && !s_wake_pending && !s_policy.local_request &&
                 generation == s_policy.generation &&
                 service_v2_voice_standby_get_state() == SERVICE_V2_VOICE_STANDBY_ACTIVE;
    taskEXIT_CRITICAL();
    return valid;
}

void service_offline_action_local_audio_finished(uint32_t generation, bool ok)
{
    taskENTER_CRITICAL();
    if (generation == s_policy.generation && generation == s_local_generation) {
        if (s_local_return_online) s_policy.state = OFFLINE_ONLINE;
        else offline_prompt_finished(&s_policy, generation, ok, (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        s_local_generation = 0;
    }
    taskEXIT_CRITICAL();
}

void service_offline_action_cloud_failed(uint32_t generation)
{
    taskENTER_CRITICAL();
    if (!s_wake_pending && s_policy.state == OFFLINE_ONLINE &&
        generation == s_policy.generation) {
        s_fallback_generation = generation;
        s_fallback_pending = true;
    }
    taskEXIT_CRITICAL();
}

void service_offline_action_cloud_connected(uint32_t generation)
{
    taskENTER_CRITICAL();
    if (generation == s_policy.generation) {
        s_fallback_pending = false;
        s_fallback_until = 0;
    }
    taskEXIT_CRITICAL();
}

void service_offline_action_close_online_window(uint32_t generation)
{
    taskENTER_CRITICAL();
    if (generation == s_policy.generation && !s_wake_pending && s_policy.state == OFFLINE_ONLINE) {
        s_policy.state = OFFLINE_IDLE;
        s_fallback_pending = false;
        s_fallback_until = 0;
    }
    taskEXIT_CRITICAL();
}

static uint32_t offline_now(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

uint32_t service_offline_action_generation(void)
{
    taskENTER_CRITICAL();
    uint32_t generation = s_policy.generation;
    taskEXIT_CRITICAL();
    return generation;
}

bool service_offline_action_active(void)
{
    taskENTER_CRITICAL();
    bool active = s_wake_pending || s_stop_pending || s_policy.local_request || s_policy.netcfg_requested || s_policy.state >= OFFLINE_PROMPT;
    taskEXIT_CRITICAL();
    return active;
}

bool service_offline_action_background_allowed(void)
{
    taskENTER_CRITICAL();
    bool allowed = !s_shutdown && !s_diagnostic && !s_wake_pending && !s_stop_pending &&
                   !s_policy.local_request && !s_policy.netcfg_requested &&
                   s_policy.state < OFFLINE_PROMPT;
    taskEXIT_CRITICAL();
    return allowed;
}

bool service_offline_action_online_current(uint32_t generation)
{
    taskENTER_CRITICAL();
    bool valid = !s_shutdown && !s_diagnostic && !s_wake_pending && !s_stop_pending && !s_policy.netcfg_requested &&
                 !s_policy.forced_offline && !s_policy.local_request &&
                 s_policy.generation == generation && s_policy.state < OFFLINE_PROMPT;
    taskEXIT_CRITICAL();
    return valid;
}

bool service_offline_action_prompt_current(uint32_t generation)
{
    taskENTER_CRITICAL();
    bool valid = s_policy.generation == generation && s_policy.state == OFFLINE_PROMPT;
    taskEXIT_CRITICAL();
    return valid;
}

void service_offline_action_cancel(void)
{
    taskENTER_CRITICAL();
    offline_cancel(&s_policy);
    s_wake_pending = false;
    s_stop_pending |= s_motion_owned;
    s_reset = true;
    s_fallback_pending = s_wake_fallback = false;
    s_fallback_until = 0;
    taskEXIT_CRITICAL();
}

void service_offline_action_shutdown(void)
{
    s_shutdown = true;
    ++s_network_epoch;
    service_offline_action_cancel();
}

void service_offline_action_shutdown_cancelled(void)
{
    taskENTER_CRITICAL();
    s_shutdown = false;
    s_reset = true;
    taskEXIT_CRITICAL();
}

void service_offline_action_request_netcfg(void)
{
    bool accepted = false;
    uint32_t accepted_count = 0U;
    taskENTER_CRITICAL();
    if (s_ready && !s_shutdown && !s_diagnostic) {
        offline_cancel(&s_policy);
        s_policy.netcfg_requested = true;
        s_wake_pending = false;
        s_stop_pending = true;
        s_fallback_pending = s_wake_fallback = false;
        s_fallback_until = 0;
        ++s_network_epoch;
        ++s_netcfg_button_count;
        s_netcfg_last_source = "button";
        accepted = true;
        accepted_count = s_netcfg_button_count;
    }
    taskEXIT_CRITICAL();
    if (accepted) sys_network_diag_mark("offline", "netcfg_accept", (int)accepted_count, "button");
}

bool service_offline_action_button_exit(uint32_t action)
{
    bool gesture = (action >= VOICE_MSG_BUTTON_ACTION_CLICK && action <= VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK) ||
                   action == VOICE_MSG_BUTTON_ACTION_LONG_HOLD;
    if (!gesture || !service_offline_action_active()) return false;
    service_offline_action_cancel();
    return action == VOICE_MSG_BUTTON_ACTION_CLICK;
}

static service_offline_button_result_t offline_begin_button(bool check_generation,
    uint32_t expected_generation, uint32_t *accepted_generation)
{
    if (accepted_generation) *accepted_generation = 0U;
    sys_network_status_t net = {0};
    bool offline = service_offline_action_forced_offline() || sys_network_get_status(&net) != 0 || !net.connected;
    taskENTER_CRITICAL();
    bool allowed = (!check_generation || expected_generation == s_policy.generation) &&
                   !s_diagnostic && !s_shutdown && !s_wake_pending && !s_stop_pending &&
                   !s_policy.netcfg_requested && s_policy.state < OFFLINE_PROMPT;
    if (allowed) {
        offline_begin(&s_policy, false, offline_now());
        if (accepted_generation) *accepted_generation = s_policy.generation;
        offline_capture_wake_network();
        s_button_wake = true;
        if (offline) {
            s_wake_pending = true;
            s_wake_received_ms = offline_now();
            s_keyword[0] = '\0';
        }
        s_fallback_pending = false;
        s_fallback_until = 0;
        s_reset = true;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return SERVICE_OFFLINE_BUTTON_REJECTED;
    return offline ? SERVICE_OFFLINE_BUTTON_OFFLINE : SERVICE_OFFLINE_BUTTON_ONLINE;
}

bool service_offline_action_begin_button(void)
{
    return offline_begin_button(false, 0U, NULL) == SERVICE_OFFLINE_BUTTON_ONLINE;
}

service_offline_button_result_t service_offline_action_begin_button_for_generation(
    uint32_t expected_generation, uint32_t *accepted_generation)
{
    return offline_begin_button(true, expected_generation, accepted_generation);
}

void service_offline_action_stop(void)
{
    taskENTER_CRITICAL();
    if (s_policy.state == OFFLINE_ACTIVE) {
        offline_cancel(&s_policy);
        s_policy.state = OFFLINE_ACTIVE;
        s_policy.last_activity_ms = offline_now();
        s_stop_pending |= s_motion_owned;
    }
    taskEXIT_CRITICAL();
}

void service_offline_action_engine_idle(void)
{
    /* Explicit WAKEUP reset must not erase the provisioning handoff. */
    taskENTER_CRITICAL();
    bool netcfg = s_policy.netcfg_requested;
    taskEXIT_CRITICAL();
    if (!netcfg && !s_wake_pending && service_offline_action_active()) service_offline_action_cancel();
}

void service_offline_action_prompt_finished(uint32_t generation, bool ok)
{
    taskENTER_CRITICAL();
    offline_prompt_finished(&s_policy, generation, ok, offline_now());
    if (!ok && s_policy.state == OFFLINE_IDLE) s_reset = true;
    taskEXIT_CRITICAL();
}

bool service_offline_action_handle_result(const char *intent, bool keyword)
{
    bool netcfg_accepted = false;
    uint32_t netcfg_accepted_count = 0U;
    if (!s_ready) return !keyword; /* Preserve normal online wake on init failure. */
    /* This app owns *all* AP commands; none may enter the legacy router. */
    if (!intent) return true;
    size_t length = 0;
    while (length < sizeof(s_keyword) && intent[length]) ++length;
    if (length == 0 || length == sizeof(s_keyword)) return true;
    taskENTER_CRITICAL();
    if (!s_shutdown && !s_diagnostic) {
        if (keyword) {
            /* Refresh the sleep deadline at ingress, before the wake worker's
             * stop/resume barriers. This does not wake peripherals early. */
            service_v2_voice_standby_note_keyword();
            s_button_wake = false;
            s_wake_fallback = s_fallback_until && s_fallback_generation == s_policy.generation &&
                              (int32_t)(s_fallback_until - offline_now()) > 0;
            s_fallback_until = 0;
            s_fallback_pending = false;
            offline_cancel(&s_policy);
            offline_capture_wake_network();
            memcpy(s_keyword, intent, length + 1);
            s_wake_pending = true;
            s_wake_received_ms = offline_now();
            s_stop_pending |= s_motion_owned;
        } else if (!s_failed) {
            int command = offline_map(intent);
            /* Online mode never accepts voice provisioning, even if its Wi-Fi
             * link is down. Consume it here without cancelling the dialog. */
            if (command == OFFLINE_CMD_NETCFG && !s_policy.forced_offline) {
                ++s_dropped;
                taskEXIT_CRITICAL();
                return true;
            }
            /* ESR hears the speaker too. Only keyword barge-in is accepted
             * during playback for mode/config controls; the following user
             * command is accepted after the wake response has finished. */
            bool mode_control = command == OFFLINE_CMD_NETCFG ||
                                command >= OFFLINE_CMD_ENTER_OFFLINE;
            bool sleeping = service_v2_voice_standby_get_state() != SERVICE_V2_VOICE_STANDBY_ACTIVE;
            bool echo = mode_control && (voice_player_mode_command_blocked() ||
                                         lingxin_server_dialog_phase() == LINGXIN_DIALOG_PHASE_SPEAKING);
            if (sleeping || echo) {
                ++s_dropped;
                taskEXIT_CRITICAL();
                LISA_LOGI(TAG, "ignore ESR control=%d sleeping=%d playback=%d", command, sleeping, echo);
                return true;
            }
            uint32_t previous_generation = s_policy.generation;
            bool priority = command == SERVICE_V2_GAIT_ACTION_STOP ||
                            command == OFFLINE_CMD_GOODBYE || command == OFFLINE_CMD_NETCFG ||
                            command >= OFFLINE_CMD_ENTER_OFFLINE;
            if ((!s_transaction_busy || priority) &&
                offline_enqueue(&s_policy, s_policy.generation, command, offline_now())) {
                ++s_accepted;
                if (command == OFFLINE_CMD_NETCFG) {
                    ++s_netcfg_esr_count;
                    s_netcfg_last_source = "esr";
                    netcfg_accepted = true;
                    netcfg_accepted_count = s_netcfg_esr_count;
                }
                if (s_policy.generation != previous_generation &&
                    (command == OFFLINE_CMD_ENTER_OFFLINE || command == OFFLINE_CMD_ENTER_ONLINE ||
                     command == OFFLINE_CMD_NETCFG)) ++s_network_epoch;
                if (s_policy.local_request) s_stop_pending = true;
                if (command == SERVICE_V2_GAIT_ACTION_STOP || command == OFFLINE_CMD_GOODBYE) {
                    /* Run the physical STOP in the worker, never this AP callback.
                     * It also stops a non-offline gait (e.g. a touch action).
                     * Failed submissions keep the existing retry barrier. */
                    s_stop_pending = true;
                    if (command == OFFLINE_CMD_GOODBYE) s_reset = true;
                }
                if (s_policy.netcfg_requested) {
                    s_stop_pending = true; /* Also stop motion owned by the previous online session. */
                    s_reset = true;
                }
            } else ++s_dropped;
        }
    }
    taskEXIT_CRITICAL();
    if (netcfg_accepted) sys_network_diag_mark("offline", "netcfg_accept", (int)netcfg_accepted_count, "esr");
    return true;
}

static bool offline_interaction_blocked(void)
{
    struct app_datas *app = s_app;
    ota_state_e ota = ota_manager_get_state();
    return s_shutdown || !app || !app->can_wakeup ||
           !(app->voice_work_mode & VOICE_WORK_MODE_VOICE_WAKEUP) ||
           app_voice_interaction_blocked() || ota == OTA_STATE_UPDATING;
}

static bool offline_blocked(void)
{
    return offline_interaction_blocked() || app_netcfg_portal_offline_blocked() ||
           app_ble_netcfg_offline_blocked();
}

static void offline_control_failed(int ret)
{
    service_offline_action_cancel();
    s_failed = true;
    s_armed = false;
    LISA_LOGE(TAG, "AP control failed=%d; offline input disabled until reboot", ret);
}

/* Caller holds s_control. Never called on RPC or voice.ebus. */
static bool offline_configure(bool arm, bool reset)
{
    int ret = acomp_wakeup_set_timeout(arm ? OFFLINE_ENGINE_MS : 0U);
    if (ret == ACOMP_ERR_OK && reset)
        ret = acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_WAKEUP);
    if (ret != ACOMP_ERR_OK) { offline_control_failed(ret); return false; }
    s_armed = arm;
    return true;
}

static void offline_release_ble(void)
{
    if (s_ble_owned) {
        app_ble_netcfg_offline_release();
        s_ble_owned = false;
    }
}

static bool offline_cloud_idle(void)
{
    lingxin_dialog_phase_t phase = lingxin_server_dialog_phase();
    return service_voice_action_query_idle() && !lingxin_server_chat_is_active() &&
           (phase == LINGXIN_DIALOG_PHASE_IDLE || phase == LINGXIN_DIALOG_PHASE_UNAVAILABLE);
}

/* Worker-only. A rejected STOP remains a barrier for every later generation.
 * No product dispatch can run while it is pending, so accepting the STOP (or
 * observing physical completion) cannot leave a retry aimed at newer motion. */
static bool offline_resolve_stop(void)
{
    taskENTER_CRITICAL();
    bool pending = s_stop_pending;
    taskEXIT_CRITICAL();
    if (!pending) return true;

    service_v2_gait_status_t gait = {0};
    bool completed = service_v2_gait_get_status(&gait) == 0 && gait.ready && !gait.busy;
    if (!completed && service_v2_gait_submit(SERVICE_V2_GAIT_ACTION_STOP, 0) != 0)
        return false;
    taskENTER_CRITICAL();
    s_stop_pending = false;
    s_motion_owned = false;
    taskEXIT_CRITICAL();
    return true;
}

/* No WiFi/portal calls in the recognition worker. Repeated wakes coalesce. */
static void offline_network_request(uint32_t epoch, bool explicit_request)
{
    taskENTER_CRITICAL();
    if (epoch == s_network_epoch &&
        (explicit_request || (!s_connect_request && !s_online_pending))) {
        s_connect_request = true;
        s_connect_request_epoch = epoch;
        s_connect_request_explicit = explicit_request;
    }
    taskEXIT_CRITICAL();
}

static void offline_network_finish(void)
{
    if (s_connect_owned) sys_wifi_reconnect_saved_once_stop();
    s_connect_owned = s_online_pending = s_online_started = s_online_probe_started = false;
    s_connect_backoff = s_connect_reassociate = false;
    s_online_background = false;
    s_online_wakeup = false;
}

static void offline_network_backoff(void)
{
    if (s_connect_owned) sys_wifi_reconnect_saved_once_stop();
    s_connect_reassociate = s_online_probe_started;
    s_connect_owned = s_online_started = s_online_probe_started = false;
    s_online_explicit = false;
    s_connect_backoff = true;
    s_connect_retry_at = offline_now() + 10000U;
    LISA_LOGI(TAG, "saved AP retry in 10s epoch=%u", (unsigned)s_connect_generation);
}

static void offline_network_step(void)
{
    taskENTER_CRITICAL();
    bool request = s_connect_request;
    uint32_t epoch = s_connect_request_epoch;
    bool explicit_request = s_connect_request_explicit;
    bool wake_upgrade = s_connect_wake_upgrade;
    uint32_t wake_epoch = s_connect_wake_epoch;
    s_connect_wake_upgrade = false;
    s_connect_claimed = request;
    s_connect_request = false;
    bool netcfg = s_policy.netcfg_requested;
    taskEXIT_CRITICAL();
    if (request && epoch == service_offline_action_network_epoch() &&
        (explicit_request || !s_online_pending || s_connect_generation != epoch)) {
        offline_network_finish();
        s_connect_generation = epoch;
        s_online_explicit = explicit_request;
        s_online_wakeup = !explicit_request;
        s_online_pending = true;
    }
    if (wake_upgrade && s_online_pending && !s_online_started &&
        wake_epoch == s_connect_generation && wake_epoch == service_offline_action_network_epoch())
        s_online_wakeup = true;
    taskENTER_CRITICAL();
    s_connect_claimed = false;
    taskEXIT_CRITICAL();
    if (!s_online_pending) {
        sys_network_status_t idle_net = {0};
        if (sys_network_get_status(&idle_net) != 0) return;
        if (idle_net.connected) {
            s_keep_connected = true;
            return;
        }
        /* A healthy association must recover during awake gaps, not wait for
         * the next keyword. Preserve manual setup and the initial boot scan. */
        if ((!s_keep_connected && !sys_wifi_ip_recovery_needed()) || netcfg || s_shutdown || s_diagnostic ||
            service_offline_action_forced_offline() || offline_interaction_blocked() ||
            service_v2_voice_standby_get_state() != SERVICE_V2_VOICE_STANDBY_ACTIVE ||
            sys_wifi_radio_suspended() || idle_net.switching ||
            app_ble_netcfg_offline_blocked() ||
            !sys_wifi_has_ap()) return;
        /* Automatic portals with no clients may yield to idle recovery.
         * Manual/unknown-client portals wait for a user request instead. */
        if (app_netcfg_portal_is_active() && app_netcfg_portal_retry_saved_background() == -EBUSY) return;
        s_connect_generation = service_offline_action_network_epoch();
        s_online_explicit = false;
        s_online_background = s_online_pending = true;
    }
    if (s_connect_generation != service_offline_action_network_epoch() ||
        service_offline_action_forced_offline() || netcfg || s_shutdown) {
        offline_network_finish();
        return;
    }
    /* Resume after sleep/OTA/credential work without consuming retry budget. */
    if (s_diagnostic || offline_interaction_blocked() ||
        service_v2_voice_standby_get_state() != SERVICE_V2_VOICE_STANDBY_ACTIVE ||
        sys_wifi_radio_suspended()) return;
    sys_network_status_t net = {0};
    if (sys_network_get_status(&net) != 0 || net.switching ||
        app_netcfg_portal_offline_blocked() || app_ble_netcfg_offline_blocked()) return;
    if (net.connected && sys_wifi_is_connected() && !app_netcfg_portal_is_active()) {
        /* Association succeeded. Do not enqueue a disconnect/stop. */
        s_keep_connected = true;
        s_connect_owned = false;
        offline_network_finish();
        return;
    }
    if (s_connect_backoff && (int32_t)(offline_now() - s_connect_retry_at) < 0) return;
    s_connect_backoff = false;
    if (!sys_wifi_has_ap()) {
        offline_network_finish();
        if (!app_netcfg_portal_is_active()) (void)app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_CONNECT_FAILED);
        app_ble_netcfg_adv_start_delayed();
        return;
    }
    if (app_netcfg_portal_is_active()) {
        int ret = s_online_explicit ? app_netcfg_portal_retry_saved() :
                  s_online_wakeup ? app_netcfg_portal_retry_saved_on_wake() :
                                    app_netcfg_portal_retry_saved_background();
        if (ret != 0) return;
        if (s_connect_generation != service_offline_action_network_epoch()) return;
    }
    s_online_explicit = false;
    s_online_wakeup = false;
    if (!s_online_started) {
        s_connect_started = offline_now();
        s_online_started = true;
        if (!s_connect_reassociate && sys_wifi_is_connected()) {
            (void)network_probe_start();
            s_online_probe_started = true;
        } else {
            int ret = sys_wifi_reconnect_saved_once_start();
            s_connect_owned = ret == 0;
            if (ret < 0) { offline_network_backoff(); return; }
            if (ret == 1) {
                (void)network_probe_start();
                s_online_probe_started = true;
            }
        }
        if (s_connect_generation != service_offline_action_network_epoch()) {
            offline_network_finish();
            return;
        }
    }
    if (sys_wifi_is_connected() && !s_online_probe_started) {
        (void)network_probe_start();
        s_online_probe_started = true;
    }
    if (sys_wifi_reconnect_saved_no_signal()) {
        if (s_online_background || s_keep_connected) {
            /* Once connected, even a wake-triggered retry must preserve the
             * saved network through temporary RF loss instead of opening AP. */
            offline_network_backoff();
            return;
        }
        offline_network_finish();
        if (s_connect_generation != service_offline_action_network_epoch()) return;
        (void)app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_CONNECT_FAILED);
        app_ble_netcfg_adv_start_delayed();
        LISA_LOGI(TAG, "saved AP has no signal, enter provisioning");
        return;
    }
    if (sys_wifi_reconnect_saved_exhausted() || offline_now() - s_connect_started >= 30000U)
        offline_network_backoff();
}

static void offline_network_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_ready) {
            uint32_t started = offline_now();
            offline_network_step();
            uint32_t elapsed = offline_now() - started;
            if (elapsed > s_network_worker_max_ms) s_network_worker_max_ms = elapsed;
            ++s_network_steps;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void offline_step(void)
{
    uint32_t audio_generation = service_offline_action_generation();
    if (!s_shutdown && audio_generation != s_audio_generation) {
        if (s_policy.local_request || s_policy.forced_offline)
            (void)lingxin_server_audio_uplink_gate_close("local_owner_changed");
        if (voice_msg_pub(VOICE_MSG_OFFLINE_AUDIO_CANCEL, &audio_generation, sizeof(audio_generation)) != 0) return;
        s_audio_generation = audio_generation;
    }
    if (!offline_resolve_stop()) return;
    if (!app_wakeup_product_ready()) return;
    sys_network_status_t net = {0};
    bool network_known = sys_network_get_status(&net) == 0;
    bool blocked = offline_interaction_blocked() || !network_known;
    s_transaction_busy = app_netcfg_portal_offline_blocked() || app_ble_netcfg_offline_blocked() || net.switching;
    bool sleeping = service_v2_voice_standby_get_state() != SERVICE_V2_VOICE_STANDBY_ACTIVE;
    taskENTER_CRITICAL();
    int local = s_policy.local_request;
    uint32_t local_generation = s_policy.generation;
    uint32_t local_network_epoch = s_network_epoch;
    bool netcfg = s_policy.netcfg_requested;
    uint32_t netcfg_generation = s_policy.generation;
    taskEXIT_CRITICAL();
    if (local) {
        if (blocked || sleeping) { service_offline_action_cancel(); return; }
        service_voice_action_cancel_pending();
        (void)lingxin_server_audio_uplink_gate_close("local_control");
        int ret = lingxin_server_chat_stop();
        if (ret != 0 || !offline_cloud_idle()) return; /* Retry the stop barrier in this worker. */
        taskENTER_CRITICAL();
        bool current = local_generation == s_policy.generation && s_policy.local_request == local &&
                       service_v2_voice_standby_get_state() == SERVICE_V2_VOICE_STANDBY_ACTIVE;
        if (current) {
            if (local == OFFLINE_CMD_ENTER_OFFLINE) s_policy.forced_offline = true;
            if (local == OFFLINE_CMD_ENTER_ONLINE) s_policy.forced_offline = false;
            if (local != OFFLINE_CMD_VERSION) s_policy.last_mode_command = local;
            s_local_return_online = local == OFFLINE_CMD_VERSION && !s_policy.forced_offline && net.connected;
            s_policy.local_request = 0;
            s_policy.state = local == OFFLINE_CMD_ENTER_ONLINE ? OFFLINE_ONLINE : OFFLINE_PROMPT;
            s_policy.last_activity_ms = offline_now();
            s_local_generation = local_generation;
            s_reset = false;
        }
        taskEXIT_CRITICAL();
        if (!current) return;
        voice_msg_offline_local_voice_t request = {local_generation, local};
        if (voice_msg_pub(VOICE_MSG_OFFLINE_LOCAL_VOICE, &request, sizeof(request)) != 0)
            service_offline_action_local_audio_finished(local_generation, false);
        LISA_LOGI(TAG, "local control=%d generation=%u forced_offline=%d wifi=%d probe=%d portal=%d",
                  local, (unsigned)local_generation, s_policy.forced_offline, net.wifi_connected,
                  net.connected, app_netcfg_portal_is_active());
        if (local == OFFLINE_CMD_ENTER_ONLINE)
            offline_network_request(local_network_epoch, true);
        return;
    }
    if (netcfg) {
        if (blocked || sleeping) { service_offline_action_cancel(); return; }
        if (s_transaction_busy) return;
        /* Poll without delaying the wake worker: the old bus cancel and its
         * STOPPED callbacks must finish before main starts the new prompt. */
        if (!voice_player_offline_audio_cancel_ready(netcfg_generation)) return;
        service_v2_gait_status_t status = {0};
        if (service_v2_gait_get_status(&status) != 0 || !status.ready || status.busy) return;
        /* Keep recognition armed while portal startup/teardown runs. */
        taskENTER_CRITICAL();
        bool current = s_policy.netcfg_requested && s_policy.generation == netcfg_generation;
        taskEXIT_CRITICAL();
        if (!current || offline_interaction_blocked() ||
            service_v2_voice_standby_get_state() != SERVICE_V2_VOICE_STANDBY_ACTIVE) return;
        offline_release_ble();
        int ret = app_enter_manual_netcfg();
        if (ret == -EBUSY || ret == -EINPROGRESS || ret == -EAGAIN) return;
        LISA_LOGI(TAG, "manual provisioning generation=%u result=%d", (unsigned)netcfg_generation, ret);
        taskENTER_CRITICAL();
        if (netcfg_generation == s_policy.generation && s_policy.netcfg_requested) {
            /* Ingress already retired the old command queue. Finish this
             * handoff without another audio cancel stopping the new prompt
             * (including the failure prompt), or clobbering a newer wake. */
            s_policy.netcfg_requested = false;
            s_policy.state = OFFLINE_IDLE;
            s_reset = true;
            s_fallback_pending = s_wake_fallback = false;
            s_fallback_until = 0;
        }
        taskEXIT_CRITICAL();
        return;
    }
    taskENTER_CRITICAL();
    if (s_stop_pending) {
        taskEXIT_CRITICAL();
        return; /* Cancellation arrived during eligibility reads; preserve all ingress. */
    }
    bool wake = s_wake_pending;
    bool reset = s_reset;
    bool fallback = s_wake_fallback;
    bool failure = s_fallback_pending && s_fallback_generation == s_policy.generation;
    s_fallback_pending = false;
    s_reset = false;
    uint32_t generation = s_policy.generation;
    char keyword[64];
    memcpy(keyword, s_keyword, sizeof(keyword));
    taskEXIT_CRITICAL();

    if (s_failed && !wake) { offline_release_ble(); return; }
    if (wake) {
        if (generation != service_offline_action_generation() || !offline_resolve_stop()) return;
        if (blocked || !network_known) {
            service_offline_action_cancel(); return;
        }
        (void)service_v2_voice_standby_wake_pose(false);
        if (service_v2_voice_standby_resume_wait(20000U) != 0) {
            service_offline_action_cancel(); return;
        }
        network_known = sys_network_get_status(&net) == 0;
        blocked = offline_interaction_blocked() || !network_known;
        if (blocked || generation != service_offline_action_generation()) {
            if (generation == service_offline_action_generation()) service_offline_action_cancel();
            return;
        }
        service_voice_action_cancel_pending();
        bool offline = s_policy.forced_offline || !net.connected || fallback;
        if (!s_policy.forced_offline && !net.connected &&
            service_offline_action_wake_network_allowed(generation))
            offline_network_request(service_offline_action_network_epoch(), false);
        if (offline) {
            s_reported_active = true;
            /* ESR must already have been armed before this natural keyword. */
            if (s_failed || !s_armed) {
                service_offline_action_cancel(); return;
            }
            if (s_button_wake) {
                int ret = acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_ESR);
                if (ret != ACOMP_ERR_OK) { offline_control_failed(ret); return; }
            }
            s_button_wake = false;
            (void)lingxin_server_audio_uplink_gate_close("offline_owner");
            int stop_ret = lingxin_server_chat_stop();
            /* Old callbacks are generation-gated. Acknowledge immediately;
             * physical actions still wait for cloud cleanup below. */
            if (stop_ret != 0) {
                service_offline_action_cancel(); return;
            }
        } else {
            offline_release_ble();
        }
        taskENTER_CRITICAL();
        bool current = generation == s_policy.generation && s_wake_pending;
        if (current) {
            s_wake_pending = false;
            s_last_wake_ack_ms = offline_now() - s_wake_received_ms;
            if (s_last_wake_ack_ms > s_max_wake_ack_ms) s_max_wake_ack_ms = s_last_wake_ack_ms;
            s_policy.state = offline ? OFFLINE_PROMPT : OFFLINE_ONLINE;
            s_policy.last_activity_ms = offline_now();
        }
        taskEXIT_CRITICAL();
        if (!current) return;
        LISA_LOGI(TAG, "wake generation=%u owner=%s", (unsigned)generation, offline ? "offline" : "online");
        if (offline) {
            if (voice_msg_pub(VOICE_MSG_OFFLINE_ACTION_PROMPT, &generation, sizeof(generation)) != 0)
                service_offline_action_cancel();
        } else {
            if (s_reported_active) {
                s_reported_active = false;
                (void)voice_msg_pub(VOICE_MSG_OFFLINE_ACTION_FINISHED, NULL, 0);
            }
            (void)voice_msg_pub(VOICE_MSG_WAKEUP_KEYWORD, keyword, strlen(keyword) + 1);
        }
        return;
    }
    /* ONLINE is deliberately excluded from active() (offline ownership), but
     * its ESR control window must also end on standby. Keep forced_offline. */
    if ((service_offline_action_active() || s_policy.state == OFFLINE_ONLINE) && (blocked || sleeping)) {
        service_offline_action_cancel(); return;
    }
    if (failure && !blocked && !sleeping) {
        /* Only a definitive current-generation failure reaches here. Stop the
         * old task before arming a single, 60-second next-wake exception. */
        (void)lingxin_server_audio_uplink_gate_close("offline_fallback_cleanup");
        int ret = lingxin_server_chat_stop();
        if (ret == 0 && !offline_cloud_idle()) {
            service_offline_action_cloud_failed(generation);
        }
        if (ret == 0 && offline_cloud_idle() &&
            generation == service_offline_action_generation()) {
            service_voice_action_cancel_pending();
            taskENTER_CRITICAL();
            s_fallback_generation = generation;
            s_fallback_until = offline_now() + 60000U;
            taskEXIT_CRITICAL();
            reset = true;
        }
    }
    service_v2_gait_status_t gait = {0};
    bool busy = service_v2_gait_get_status(&gait) != 0 || !gait.ready || gait.busy;
    taskENTER_CRITICAL();
    offline_state_t before = s_policy.state;
    bool finished = s_policy.was_busy && !busy;
    offline_poll(&s_policy, offline_now(), busy);
    bool expired = before >= OFFLINE_PROMPT && s_policy.state == OFFLINE_IDLE;
    if (s_transaction_busy) s_policy.head = s_policy.count = 0;
    int action = !busy && !blocked && !s_transaction_busy && !sleeping && s_policy.state == OFFLINE_ACTIVE &&
                 s_policy.generation == generation && s_policy.count ? s_policy.queue[s_policy.head] : -1;
    taskEXIT_CRITICAL();
    if (finished) {
        s_motion_owned = false;
        ++s_finished;
        LISA_LOGI(TAG, "action finished generation=%u count=%u", (unsigned)generation, (unsigned)s_finished);
    }
    if (expired) reset = true;
    /* Volume controls do not wait for, or interrupt, gait playback. */
    taskENTER_CRITICAL();
    int control = !blocked && !s_transaction_busy && !sleeping ? offline_take_control(&s_policy, generation) : -1;
    taskEXIT_CRITICAL();
    if (control >= 0 && generation == service_offline_action_generation() && !offline_blocked()) {
        service_volume_adjust(control == OFFLINE_CMD_VOLUME_UP ? 10 : -10);
        service_v2_voice_standby_note_activity();
        LISA_LOGI(TAG, "volume generation=%u delta=%d", (unsigned)generation,
                  control == OFFLINE_CMD_VOLUME_UP ? 10 : -10);
    }
    if (action >= 0 && generation == service_offline_action_generation() && !offline_blocked() && offline_cloud_idle()) {
        service_v2_gait_action_t gait_action = action == OFFLINE_CMD_FLIP_CONTINUOUS ?
            SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP : (service_v2_gait_action_t)action;
        uint32_t cycles = action == OFFLINE_CMD_FLIP_CONTINUOUS ? 2U :
            service_v2_gait_default_cycles(gait_action);
        int ret = service_v2_gait_submit_if_idle(gait_action, cycles);
        taskENTER_CRITICAL();
        bool current = generation == s_policy.generation;
        if (current && ret != -EBUSY) (void)offline_take(&s_policy, generation);
        if (ret == 0) {
            s_motion_owned = true;
            if (current) s_policy.was_busy = true;
            else s_stop_pending = true;
        }
        taskEXIT_CRITICAL();
        if (ret == 0 && !current) {
            (void)offline_resolve_stop();
            return;
        }
        else if (ret == 0) {
            ++s_started;
            LISA_LOGI(TAG, "action start generation=%u command=%d action=%d cycles=%u accepted=%u dropped=%u",
                      (unsigned)generation, action, (int)gait_action, (unsigned)cycles,
                      (unsigned)s_accepted, (unsigned)s_dropped);
            service_v2_voice_standby_note_activity();
        } else if (ret != -EBUSY) {
            LISA_LOGE(TAG, "action dropped generation=%u action=%d ret=%d", (unsigned)generation, action, ret);
        }
    }
    if (!service_offline_action_active()) {
        if (s_reported_active) {
            s_reported_active = false;
            (void)voice_msg_pub(VOICE_MSG_OFFLINE_ACTION_FINISHED, NULL, 0);
        }
        offline_release_ble();
        bool arm = !blocked; /* ESR remains armed for the online mode-control whitelist. */
        if (reset || arm != s_armed) {
            if (s_button_wake && arm) {
                if (offline_configure(true, false)) {
                    int ret = acomp_wakeup_set_algo_mode(ACOMP_WAKEUP_ALGO_MODE_ESR);
                    if (ret != ACOMP_ERR_OK) offline_control_failed(ret);
                }
                s_button_wake = false;
            } else (void)offline_configure(arm, true);
        }
    }
}

static void offline_worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_control, 0) == pdTRUE) {
            uint32_t started = offline_now();
            if (!s_diagnostic) offline_step();
            uint32_t elapsed = offline_now() - started;
            if (elapsed > s_worker_max_ms) s_worker_max_ms = elapsed;
            ++s_worker_steps;
            xSemaphoreGive(s_control);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool service_offline_action_diagnostic_acquire(void)
{
    if (!s_control || xSemaphoreTake(s_control, 0) != pdTRUE) return false;
    taskENTER_CRITICAL();
    bool allowed = !s_diagnostic && !s_shutdown && !s_failed && !s_wake_pending && !s_stop_pending && !s_policy.netcfg_requested &&
                   s_policy.state < OFFLINE_PROMPT;
    if (allowed) { s_diagnostic = true; offline_cancel(&s_policy); }
    taskEXIT_CRITICAL();
    xSemaphoreGive(s_control);
    return allowed;
}

void service_offline_action_diagnostic_release(bool restored)
{
    /* Diagnostic has finished all AP calls before handing control back. */
    taskENTER_CRITICAL();
    s_failed |= !restored;
    s_armed = false;
    s_reset = true;
    s_diagnostic = false;
    taskEXIT_CRITICAL();
}

int service_offline_action_init(void)
{
    if (s_ready) return 0;
    s_app = get_app_datas(); /* Initialized on main; avoid 20 warning logs/sec in worker. */
    offline_policy_init(&s_policy); /* Mode is RAM-only; every boot starts online. */
    s_control = xSemaphoreCreateMutex();
    if (!s_control) return -ENOMEM;
    s_reset = true;
    if (xTaskCreate(offline_worker, "offline.action", 2048, NULL, 2, NULL) != pdPASS)
        return -ENOMEM;
    if (xTaskCreate(offline_network_worker, "offline.network", 2048, NULL, 2, NULL) != pdPASS)
        return -ENOMEM;
    s_ready = true;
    return 0;
}

static int shell_offline_action(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "status") != 0) return -EINVAL;
    taskENTER_CRITICAL();
    offline_policy_t snapshot = s_policy;
    bool armed = s_armed, diagnostic = s_diagnostic, failed = s_failed, pending = s_wake_pending;
    uint32_t accepted = s_accepted, dropped = s_dropped, started = s_started, finished = s_finished;
    uint32_t network_epoch = s_network_epoch;
    uint32_t netcfg_esr = s_netcfg_esr_count, netcfg_button = s_netcfg_button_count;
    const char *netcfg_source = s_netcfg_last_source;
    taskEXIT_CRITICAL();
    shellPrint(shellGetCurrent(), "OFFLINE state=%d gen=%u armed=%d diag=%d failed=%d pending=%d queue=%u\n",
               snapshot.state, (unsigned)snapshot.generation, armed, diagnostic, failed, pending, snapshot.count);
    shellPrint(shellGetCurrent(), "OFFLINE accepted=%u dropped=%u started=%u finished=%u prompt=%d idle_age_ms=%u\n",
               (unsigned)accepted, (unsigned)dropped, (unsigned)started, (unsigned)finished,
               snapshot.state == OFFLINE_PROMPT, (unsigned)(offline_now() - snapshot.last_activity_ms));
    shellPrint(shellGetCurrent(), "OFFLINE controls=%u netcfg_pending=%d\n",
               snapshot.control_count, snapshot.netcfg_requested);
    shellPrint(shellGetCurrent(), "OFFLINE network_epoch=%u netcfg_esr=%u netcfg_button=%u netcfg_last_source=%s\n",
               (unsigned)network_epoch, (unsigned)netcfg_esr, (unsigned)netcfg_button, netcfg_source);
    shellPrint(shellGetCurrent(), "OFFLINE timing ack_last_ms=%u ack_max_ms=%u worker_max_ms=%u steps=%u\n",
               (unsigned)s_last_wake_ack_ms, (unsigned)s_max_wake_ack_ms,
               (unsigned)s_worker_max_ms, (unsigned)s_worker_steps);
    shellPrint(shellGetCurrent(), "OFFLINE mode=%s local_request=%d net_attempt=%d transaction_busy=%d\n",
               snapshot.forced_offline ? "offline" : "online", snapshot.local_request,
               s_online_pending, s_transaction_busy);
    shellPrint(shellGetCurrent(), "OFFLINE retry started=%d backoff=%d epoch=%u steps=%u worker_max_ms=%u\n",
               s_online_started, s_connect_backoff, (unsigned)s_connect_generation,
               (unsigned)s_network_steps, (unsigned)s_network_worker_max_ms);
    shellPrint(shellGetCurrent(), "OFFLINE keep_connected=%d background=%d explicit=%d saved_ap=%d\n",
               s_keep_connected, s_online_background, s_online_explicit, sys_wifi_has_ap());
    sys_network_status_t net = {0};
    bool known = sys_network_get_status(&net) == 0;
    uint32_t ip = 0;
    bool has_ip = sys_wifi_get_ip_addr(&ip);
    shellPrint(shellGetCurrent(), "OFFLINE link wifi=%d ip_valid=%d ip=0x%08x probe=%d bearer=%d switching=%d portal_active=%d\n",
               sys_wifi_is_connected(), has_ip, (unsigned)ip, net.connected, net.active_bearer,
               net.switching, app_netcfg_portal_is_active());
    shellPrint(shellGetCurrent(), "OFFLINE gates net_known=%d connected=%d portal=%d ble=%d ota=%d sleep=%d shutdown=%d\n",
               known, net.connected, app_netcfg_portal_offline_blocked(), app_ble_netcfg_offline_blocked(),
               ota_manager_get_state(), service_v2_voice_standby_get_state(), s_shutdown);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 offline_action, shell_offline_action, "Read-only offline_action status");
