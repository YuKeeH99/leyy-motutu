#include "service_v2_rgb_effect_policy.h"

#include <stddef.h>
#include <string.h>

#define RGB_LOW_BATTERY_ENTER_PERCENT 20U
#define RGB_LOW_BATTERY_EXIT_PERCENT  25U
#define RGB_DIZZY_BLINK_MS           5000U
#define RGB_DIZZY_FADE_MS            1500U
#define RGB_DIZZY_TOTAL_MS (RGB_DIZZY_BLINK_MS + RGB_DIZZY_FADE_MS)
#define RGB_PROVISIONING_ON_MS        300U
#define RGB_PROVISIONING_CYCLE_MS    1000U
#define RGB_PROVISIONING_SUCCESS_MS  2000U
#define RGB_PROVISIONING_LEVEL         40U
#define RGB_MUTE_CYCLE_MS            2000U
#define RGB_OTA_CYCLE_MS             1200U
#define RGB_OTA_SUCCESS_MS           2000U
#define RGB_OTA_FAILED_MS            3000U

bool service_v2_rgb_effect_frame_due(uint32_t elapsed_ms, uint32_t *pending_ms)
{
    if (pending_ms == NULL) return false;
    if (*pending_ms >= SERVICE_V2_RGB_FRAME_PERIOD_MS ||
        elapsed_ms >= SERVICE_V2_RGB_FRAME_PERIOD_MS - *pending_ms) {
        *pending_ms = 0U;
        return true;
    }
    *pending_ms += elapsed_ms;
    return false;
}

void service_v2_rgb_effect_policy_init(service_v2_rgb_effect_policy_t *policy, uint32_t seed)
{
    if (policy == NULL) {
        return;
    }
    policy->state = SERVICE_V2_RGB_EFFECT_IDLE;
    policy->ota_state = SERVICE_V2_RGB_OTA_NONE;
    policy->ota_elapsed_ms = 0U;
    (void)seed;
    policy->power_state = SERVICE_V2_RGB_POWER_DISCHARGING;
    policy->emotion = SERVICE_V2_RGB_EMOTION_NEUTRAL;
    policy->battery_level = 100U;
    policy->low_battery = false;
    policy->network_unavailable = false;
    policy->alert_elapsed_ms = 0U;
    policy->forced_offline = false;
    policy->offline_elapsed_ms = 0U;
    policy->muted = false;
    policy->mute_elapsed_ms = 0U;
    policy->provisioning_state = SERVICE_V2_RGB_PROVISIONING_INACTIVE;
    policy->provisioning_elapsed_ms = 0U;
    policy->dizzy_active = false;
    policy->dizzy_elapsed_ms = 0U;
}

void service_v2_rgb_effect_policy_event(service_v2_rgb_effect_policy_t *policy,
                                        service_v2_rgb_effect_event_t event)
{
    if (policy == NULL) {
        return;
    }
    switch (event) {
    case SERVICE_V2_RGB_EVENT_TTS_PLAYING:
        policy->state = SERVICE_V2_RGB_EFFECT_SPEAKING;
        break;
    case SERVICE_V2_RGB_EVENT_WAKEUP:
    case SERVICE_V2_RGB_EVENT_SESSION_STARTING:
    case SERVICE_V2_RGB_EVENT_TTS_STOPPED:
        policy->state = SERVICE_V2_RGB_EFFECT_LISTENING;
        policy->emotion = SERVICE_V2_RGB_EMOTION_NEUTRAL;
        break;
    case SERVICE_V2_RGB_EVENT_SESSION_FINISHED:
    case SERVICE_V2_RGB_EVENT_DISCONNECTED:
        policy->state = SERVICE_V2_RGB_EFFECT_IDLE;
        policy->emotion = SERVICE_V2_RGB_EMOTION_NEUTRAL;
        break;
    case SERVICE_V2_RGB_EVENT_DIZZY:
        policy->dizzy_active = true;
        policy->dizzy_elapsed_ms = 0U;
        break;
    default:
        break;
    }
}

void service_v2_rgb_effect_policy_provisioning(
    service_v2_rgb_effect_policy_t *policy,
    service_v2_rgb_provisioning_state_t state)
{
    if (policy == NULL || state > SERVICE_V2_RGB_PROVISIONING_SUCCESS) {
        return;
    }

    policy->provisioning_state = state;
    policy->provisioning_elapsed_ms = 0U;
}

void service_v2_rgb_effect_policy_battery(service_v2_rgb_effect_policy_t *policy,
                                          uint8_t level,
                                          service_v2_rgb_power_state_t power_state)
{
    if (policy == NULL) {
        return;
    }
    bool was_low = policy->low_battery;
    policy->battery_level = level > 100U ? 100U : level;
    policy->power_state = power_state;
    if (power_state != SERVICE_V2_RGB_POWER_DISCHARGING) {
        policy->low_battery = false;
    } else if (policy->low_battery) {
        policy->low_battery = policy->battery_level < RGB_LOW_BATTERY_EXIT_PERCENT;
    } else {
        policy->low_battery = policy->battery_level <= RGB_LOW_BATTERY_ENTER_PERCENT;
    }
    if (was_low != policy->low_battery && !policy->network_unavailable)
        policy->alert_elapsed_ms = 0U;
}

void service_v2_rgb_effect_policy_network(service_v2_rgb_effect_policy_t *policy,
                                          bool connected)
{
    if (policy == NULL) return;
    if (policy->network_unavailable != !connected) policy->alert_elapsed_ms = 0U;
    policy->network_unavailable = !connected;
}

void service_v2_rgb_effect_policy_ota(service_v2_rgb_effect_policy_t *policy,
                                     service_v2_rgb_ota_state_t state)
{
    if (policy == NULL || state == policy->ota_state) return;
    policy->ota_state = state;
    policy->ota_elapsed_ms = 0U;
}

void service_v2_rgb_effect_policy_offline(service_v2_rgb_effect_policy_t *policy,
                                          bool forced_offline)
{
    if (policy == NULL) return;
    if (policy->forced_offline != forced_offline) policy->offline_elapsed_ms = 0U;
    policy->forced_offline = forced_offline;
}

bool service_v2_rgb_effect_policy_alert_frame(const service_v2_rgb_effect_policy_t *policy,
                                               service_v2_rgb_color_t pixels[5])
{
    if (policy == NULL || pixels == NULL) return false;
    service_v2_rgb_color_t color = {0U, 0U, 0U};
    bool all = false;
    if (policy->ota_state == SERVICE_V2_RGB_OTA_UPDATING) {
        all = true;
        uint32_t phase = policy->ota_elapsed_ms;
        if (phase % 400U < 300U) {
            if (phase < 400U) color.red = 80U;
            else if (phase < 800U) color.green = 80U;
            else color.blue = 80U;
        }
    } else if (policy->ota_state == SERVICE_V2_RGB_OTA_SUCCESS &&
               policy->ota_elapsed_ms < RGB_OTA_SUCCESS_MS) {
        all = true;
        color.green = 80U;
    } else if (policy->ota_state == SERVICE_V2_RGB_OTA_FAILED &&
               policy->ota_elapsed_ms < RGB_OTA_FAILED_MS) {
        all = true;
        if (policy->ota_elapsed_ms % 600U < 300U) color.red = 80U;
    } else if (policy->provisioning_state == SERVICE_V2_RGB_PROVISIONING_ACTIVE) {
        if (policy->provisioning_elapsed_ms < RGB_PROVISIONING_ON_MS)
            color = (service_v2_rgb_color_t){80U, 30U, 0U};
    } else if (policy->provisioning_state == SERVICE_V2_RGB_PROVISIONING_SUCCESS) {
        color = (service_v2_rgb_color_t){0U, RGB_PROVISIONING_LEVEL, 0U};
    } else if (policy->network_unavailable && !policy->forced_offline) {
        all = true;
        if (policy->alert_elapsed_ms < RGB_PROVISIONING_ON_MS)
            color = (service_v2_rgb_color_t){80U, 30U, 0U};
    } else if (policy->low_battery) {
        if (policy->alert_elapsed_ms < RGB_PROVISIONING_ON_MS)
            color = (service_v2_rgb_color_t){120U, 0U, 0U};
    } else if (policy->muted) {
        if (policy->mute_elapsed_ms < 100U ||
            (policy->mute_elapsed_ms >= 200U && policy->mute_elapsed_ms < 300U)) {
            color = (service_v2_rgb_color_t){65U, 0U, 120U};
        }
    } else if (policy->forced_offline) {
        /* Intentional offline is a mode, not a network fault. Keep battery
         * and mute alerts visible, and distinguish it from orange link loss. */
        all = true;
        if (policy->offline_elapsed_ms < RGB_PROVISIONING_ON_MS)
            color = (service_v2_rgb_color_t){120U, 25U, 65U};
    } else {
        return false;
    }
    for (unsigned i = 0U; i < 5U; ++i)
        pixels[i] = all || i == 1U || i == 3U ? color : (service_v2_rgb_color_t){0U, 0U, 0U};
    return true;
}

bool service_v2_rgb_effect_policy_emotion(service_v2_rgb_effect_policy_t *policy,
                                          const char *emotion)
{
    if (policy == NULL || emotion == NULL) {
        return false;
    }
    if (strcmp(emotion, "happy") == 0) policy->emotion = SERVICE_V2_RGB_EMOTION_HAPPY;
    else if (strcmp(emotion, "sad") == 0) policy->emotion = SERVICE_V2_RGB_EMOTION_SAD;
    else if (strcmp(emotion, "angry") == 0) policy->emotion = SERVICE_V2_RGB_EMOTION_ANGRY;
    else if (strcmp(emotion, "fearful") == 0) policy->emotion = SERVICE_V2_RGB_EMOTION_FEARFUL;
    else if (strcmp(emotion, "surprised") == 0) policy->emotion = SERVICE_V2_RGB_EMOTION_SURPRISED;
    else if (strcmp(emotion, "disgusted") == 0) policy->emotion = SERVICE_V2_RGB_EMOTION_DISGUSTED;
    else if (strcmp(emotion, "neutral") == 0) policy->emotion = SERVICE_V2_RGB_EMOTION_NEUTRAL;
    else return false;
    return true;
}

void service_v2_rgb_effect_policy_mute(service_v2_rgb_effect_policy_t *policy, bool muted)
{
    if (policy == NULL) {
        return;
    }
    if (policy->muted != muted) {
        policy->mute_elapsed_ms = 0U;
    }
    policy->muted = muted;
}

void service_v2_rgb_effect_policy_tick(service_v2_rgb_effect_policy_t *policy,
                                       uint32_t elapsed_ms)
{
    if (policy == NULL) {
        return;
    }

    policy->alert_elapsed_ms = (policy->alert_elapsed_ms + elapsed_ms % RGB_PROVISIONING_CYCLE_MS) % RGB_PROVISIONING_CYCLE_MS;
    if (policy->forced_offline) {
        policy->offline_elapsed_ms = (policy->offline_elapsed_ms + elapsed_ms % RGB_PROVISIONING_CYCLE_MS) % RGB_PROVISIONING_CYCLE_MS;
    }
    if (policy->ota_state == SERVICE_V2_RGB_OTA_UPDATING) {
        policy->ota_elapsed_ms = (policy->ota_elapsed_ms + elapsed_ms % RGB_OTA_CYCLE_MS) % RGB_OTA_CYCLE_MS;
    } else if (policy->ota_state == SERVICE_V2_RGB_OTA_SUCCESS ||
               policy->ota_state == SERVICE_V2_RGB_OTA_FAILED) {
        uint32_t limit = policy->ota_state == SERVICE_V2_RGB_OTA_SUCCESS
                             ? RGB_OTA_SUCCESS_MS : RGB_OTA_FAILED_MS;
        /* Retain the terminal state after expiry so repeated polling cannot
         * restart its indication on every frame. */
        policy->ota_elapsed_ms += elapsed_ms < limit - policy->ota_elapsed_ms
                                      ? elapsed_ms : limit - policy->ota_elapsed_ms;
    }
    if (policy->muted) {
        policy->mute_elapsed_ms = (policy->mute_elapsed_ms + elapsed_ms % RGB_MUTE_CYCLE_MS) % RGB_MUTE_CYCLE_MS;
    }
    if (policy->provisioning_state == SERVICE_V2_RGB_PROVISIONING_ACTIVE) {
        policy->provisioning_elapsed_ms =
            (policy->provisioning_elapsed_ms + elapsed_ms) % RGB_PROVISIONING_CYCLE_MS;
    } else if (policy->provisioning_state == SERVICE_V2_RGB_PROVISIONING_SUCCESS) {
        if (elapsed_ms >= RGB_PROVISIONING_SUCCESS_MS - policy->provisioning_elapsed_ms) {
            policy->provisioning_state = SERVICE_V2_RGB_PROVISIONING_INACTIVE;
            policy->provisioning_elapsed_ms = 0U;
        } else {
            policy->provisioning_elapsed_ms += elapsed_ms;
        }
    }

    if (policy->dizzy_active) {
        if (elapsed_ms >= RGB_DIZZY_TOTAL_MS - policy->dizzy_elapsed_ms) {
            policy->dizzy_elapsed_ms = RGB_DIZZY_TOTAL_MS;
            policy->dizzy_active = false;
        } else {
            policy->dizzy_elapsed_ms += elapsed_ms;
        }
    }
}

uint8_t service_v2_rgb_effect_breath_percent(uint32_t phase)
{
    uint32_t normalized = phase & 63U;
    uint32_t ramp = normalized <= 32U ? normalized : 64U - normalized;
    /* Smoothstep: slow near the ends, no harsh triangular brightness cusp. */
    return (uint8_t)(18U + (52U * ramp * ramp * (96U - 2U * ramp)) / 32768U);
}

void service_v2_rgb_effect_policy_color(const service_v2_rgb_effect_policy_t *policy,
                                        uint8_t brightness_percent,
                                        service_v2_rgb_color_t *color)
{
    service_v2_rgb_color_t base = {22U, 110U, 78U};
    bool solid = false;

    if (policy == NULL || color == NULL) {
        return;
    }
    service_v2_rgb_color_t alert_pixels[5];
    if (service_v2_rgb_effect_policy_alert_frame(policy, alert_pixels)) {
        *color = alert_pixels[1];
        return;
    }
    if (policy->state == SERVICE_V2_RGB_EFFECT_LISTENING) {
        base = (service_v2_rgb_color_t){24U, 100U, 160U};
    } else if (policy->state == SERVICE_V2_RGB_EFFECT_SPEAKING) {
        static const service_v2_rgb_color_t colors[] = {
            [SERVICE_V2_RGB_EMOTION_NEUTRAL] = {45U, 110U, 150U},
            [SERVICE_V2_RGB_EMOTION_HAPPY] = {35U, 140U, 110U},
            [SERVICE_V2_RGB_EMOTION_SAD] = {20U, 45U, 120U},
            [SERVICE_V2_RGB_EMOTION_ANGRY] = {140U, 15U, 10U},
            [SERVICE_V2_RGB_EMOTION_FEARFUL] = {65U, 55U, 120U},
            [SERVICE_V2_RGB_EMOTION_SURPRISED] = {65U, 130U, 160U},
            [SERVICE_V2_RGB_EMOTION_DISGUSTED] = {35U, 100U, 65U},
        };
        base = colors[(unsigned)policy->emotion <= SERVICE_V2_RGB_EMOTION_DISGUSTED ? policy->emotion : SERVICE_V2_RGB_EMOTION_NEUTRAL];
    } else if (policy->power_state == SERVICE_V2_RGB_POWER_CHARGING) {
        base = (service_v2_rgb_color_t){100U, 0U, 0U};
        solid = true;
    } else if (policy->power_state == SERVICE_V2_RGB_POWER_CHARGE_DONE) {
        base = (service_v2_rgb_color_t){0U, 100U, 0U};
        solid = true;

    }

    {
        uint8_t level = solid || brightness_percent > 100U ? 100U : brightness_percent;
        color->red = (uint8_t)(((uint16_t)base.red * level) / 100U);
        color->green = (uint8_t)(((uint16_t)base.green * level) / 100U);
        color->blue = (uint8_t)(((uint16_t)base.blue * level) / 100U);
    }

    if (policy->dizzy_active) {
        if (policy->dizzy_elapsed_ms < RGB_DIZZY_BLINK_MS) {
            bool on = ((policy->dizzy_elapsed_ms / 250U) & 1U) == 0U;
            *color = on ? (service_v2_rgb_color_t){140U, 0U, 0U}
                        : (service_v2_rgb_color_t){0U, 0U, 0U};
        } else {
            uint32_t remain = RGB_DIZZY_TOTAL_MS - policy->dizzy_elapsed_ms;
            color->red = (uint8_t)((140U * remain + color->red * (RGB_DIZZY_FADE_MS - remain)) /
                                   RGB_DIZZY_FADE_MS);
            color->green = (uint8_t)((color->green * (RGB_DIZZY_FADE_MS - remain)) /
                                     RGB_DIZZY_FADE_MS);
            color->blue = (uint8_t)((color->blue * (RGB_DIZZY_FADE_MS - remain)) /
                                    RGB_DIZZY_FADE_MS);
        }
    }
}
