#include "service_v2_idle_action.h"
#include "service_v2_voice_standby.h"
#include "service_offline_action.h"
#include "service_voice_action.h"
#include "lingxin_server.h"
#include "voice_player.h"
#include "voice_player_comm.h"
#include "alarm_ring.h"
#include "app_netcfg_portal.h"
#include "ota_manager.h"
#include "battery/battery.h"
#include "power/power_manager.h"
#include "service_v2_actuators.h"
#include "service_v2_gait.h"
#include "service_v2_imu_interaction.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lisa_time.h"

static bool business_idle(bool audio_idle)
{
    lingxin_dialog_phase_t phase = lingxin_server_dialog_phase();
    ota_state_e ota = ota_manager_get_state();
    service_voice_action_status_t voice = {0};
    return service_v2_voice_standby_get_state() == SERVICE_V2_VOICE_STANDBY_ACTIVE &&
           service_offline_action_background_allowed() &&
           (phase == LINGXIN_DIALOG_PHASE_IDLE || phase == LINGXIN_DIALOG_PHASE_UNAVAILABLE) &&
           !lingxin_server_chat_is_active() && service_voice_action_query_idle() &&
           service_voice_action_get_status(&voice) == 0 && voice.ready &&
           !voice.action_active && voice.queued_count == 0U &&
           !voice_player_tts_is_active() && audio_idle &&
           !alarm_ring_is_active() && !app_netcfg_portal_is_active() &&
           ota != OTA_STATE_CHECKING && ota != OTA_STATE_PACKAGE_INFO && ota != OTA_STATE_UPDATING;
}

bool service_v2_background_motion_allowed(void)
{
    /* USB presence includes a full battery; the charger current is irrelevant. */
    return !power_is_usb_plugged() && business_idle(!voice_player_is_audio_active());
}

#define IDLE_INTERVAL_MS 60000U
static bool s_idle_valid;
static uint32_t s_idle_since;
static uint32_t s_idle_generation;

static service_v2_gait_action_t idle_random_action(void)
{
    /* Seed once, then advance independently of the fixed one-minute cadence. */
    static uint32_t state;
    if (state == 0U) state = lisa_rand32() | 1U;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    switch (state % 3U) {
    case 0U: return SERVICE_V2_GAIT_ACTION_IDLE_STRETCH;
    case 1U: return SERVICE_V2_GAIT_ACTION_IDLE_SCRATCH;
    default: return SERVICE_V2_GAIT_ACTION_PUSHUP;
    }
}

static bool idle_due(uint32_t now, bool idle, uint32_t generation)
{
    if (!idle) { s_idle_valid = false; return false; }
    if (!s_idle_valid || generation != s_idle_generation) {
        s_idle_valid = true;
        s_idle_since = now;
        s_idle_generation = generation;
        return false;
    }
    if ((uint32_t)(now - s_idle_since) < IDLE_INTERVAL_MS) return false;
    s_idle_since = now; /* Consume a busy/unsafe opportunity; never replay it. */
    return true;
}

void service_v2_idle_action_poll(bool enabled)
{
    uint32_t generation = service_offline_action_generation();
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (power_is_usb_plugged()) {
        (void)idle_due(now, false, generation);
        /* No HOME/STOP motion just to save power. The two ownership locks below
         * recheck pending gait work and raw servo activity before releasing PWM.
         * Capture generation before the business snapshot, so a completed wake
         * cannot turn this old poll into a release of its newly restored pose. */
        if (business_idle(!voice_player_is_audio_active())) (void)service_v2_gait_release_idle_pwm(generation);
        return;
    }
    service_v2_gait_status_t gait = {0};
    bool idle = enabled && service_v2_background_motion_allowed() &&
                service_v2_gait_get_status(&gait) == 0 && gait.ready;
    /* Our own gesture does not restart the minute; any foreground motion does. */
    if (gait.busy && !gait.background) idle = false;
    if (!idle_due(now, idle, generation) || gait.busy) return;

    service_v2_imu_interaction_status_t imu = {0};
    service_v2_servo_status_t servo = {0};
    if (service_v2_imu_interaction_get_status(&imu) != 0 || !imu.ready ||
        imu.pose != SERVICE_V2_IMU_POSE_NORMAL || battery_get_voltage_mv() < 3300U ||
        service_v2_actuators_servo_get_status(&servo) != 0 || servo.frame_active ||
        servo.sweep_active || servo.watchdog_ms != 0U || servo.watchdog_expired ||
        servo.enabled_mask != SERVICE_V2_SERVO_ALL_MASK) return;
    const uint16_t home[4] = {2100U, 900U, 2050U, 870U};
    for (uint32_t ch = 0U; ch < 4U; ++ch) {
        int current = (int)servo.current_us[ch] - home[ch];
        int target = (int)servo.target_us[ch] - home[ch];
        if (current < -40 || current > 40 || target < -40 || target > 40) return;
    }
    (void)service_v2_gait_submit_background(idle_random_action(), 0U);
}
