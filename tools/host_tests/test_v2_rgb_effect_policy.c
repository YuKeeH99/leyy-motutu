#include <assert.h>
#include <stdint.h>

#include "service_v2_rgb_effect_policy.h"

int main(void)
{
    service_v2_rgb_effect_policy_t policy;
    service_v2_rgb_color_t color;

    service_v2_rgb_effect_policy_init(&policy, 0x12345678U);
    assert(policy.state == SERVICE_V2_RGB_EFFECT_IDLE);

    service_v2_rgb_effect_policy_battery(&policy, 80U, SERVICE_V2_RGB_POWER_DISCHARGING);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.green > color.red && color.green > color.blue);

    service_v2_rgb_effect_policy_battery(&policy, 20U, SERVICE_V2_RGB_POWER_DISCHARGING);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red > 0U && color.green == 0U && color.blue == 0U);
    service_v2_rgb_effect_policy_battery(&policy, 23U, SERVICE_V2_RGB_POWER_DISCHARGING);
    assert(policy.low_battery);
    service_v2_rgb_effect_policy_battery(&policy, 25U, SERVICE_V2_RGB_POWER_DISCHARGING);
    assert(!policy.low_battery);

    service_v2_rgb_effect_policy_battery(&policy, 50U, SERVICE_V2_RGB_POWER_CHARGING);
    service_v2_rgb_effect_policy_color(&policy, 12U, &color);
    assert(color.red == 100U && color.green == 0U && color.blue == 0U);

    service_v2_rgb_effect_policy_battery(&policy, 100U, SERVICE_V2_RGB_POWER_CHARGE_DONE);
    service_v2_rgb_effect_policy_color(&policy, 12U, &color);
    assert(color.red == 0U && color.green == 100U && color.blue == 0U);

    service_v2_rgb_effect_policy_event(&policy, SERVICE_V2_RGB_EVENT_WAKEUP);
    assert(policy.state == SERVICE_V2_RGB_EFFECT_LISTENING);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.blue > color.red && color.green > color.red);

    service_v2_rgb_effect_policy_event(&policy, SERVICE_V2_RGB_EVENT_TTS_PLAYING);
    assert(policy.state == SERVICE_V2_RGB_EFFECT_SPEAKING);
    assert(service_v2_rgb_effect_policy_emotion(&policy, "angry"));
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red > color.green && color.red > color.blue);
    assert(service_v2_rgb_effect_policy_emotion(&policy, "sad"));
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.blue > color.red && color.blue > color.green);
    assert(!service_v2_rgb_effect_policy_emotion(&policy, "not-an-emotion"));

    service_v2_rgb_effect_policy_event(&policy, SERVICE_V2_RGB_EVENT_TTS_STOPPED);
    assert(policy.state == SERVICE_V2_RGB_EFFECT_LISTENING);
    service_v2_rgb_effect_policy_event(&policy, SERVICE_V2_RGB_EVENT_SESSION_FINISHED);
    assert(policy.state == SERVICE_V2_RGB_EFFECT_IDLE);
    service_v2_rgb_effect_policy_battery(&policy, 80U, SERVICE_V2_RGB_POWER_DISCHARGING);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.green > color.red && color.green > color.blue);

    /* The provisioning portal must describe its desired indication to the
     * central RGB policy instead of writing PA08 from a second task. */
    service_v2_rgb_effect_policy_provisioning(
        &policy, SERVICE_V2_RGB_PROVISIONING_ACTIVE);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red == 80U && color.green == 30U && color.blue == 0U);
    service_v2_rgb_effect_policy_tick(&policy, 300U);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red == 0U && color.green == 0U && color.blue == 0U);
    service_v2_rgb_effect_policy_tick(&policy, 700U);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red == 80U && color.green == 30U && color.blue == 0U);

    service_v2_rgb_effect_policy_provisioning(
        &policy, SERVICE_V2_RGB_PROVISIONING_SUCCESS);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red == 0U && color.green == 40U && color.blue == 0U);
    service_v2_rgb_effect_policy_tick(&policy, 2000U);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.green > color.red && color.green > color.blue);

    service_v2_rgb_effect_policy_event(&policy, SERVICE_V2_RGB_EVENT_DIZZY);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red == 140U && color.green == 0U && color.blue == 0U);
    service_v2_rgb_effect_policy_tick(&policy, 250U);
    service_v2_rgb_effect_policy_color(&policy, 100U, &color);
    assert(color.red == 0U && color.green == 0U && color.blue == 0U);
    service_v2_rgb_effect_policy_tick(&policy, 4750U);
    assert(policy.dizzy_active);
    service_v2_rgb_effect_policy_tick(&policy, 1500U);
    assert(!policy.dizzy_active);

    assert(service_v2_rgb_effect_breath_percent(0U) == 18U);
    assert(service_v2_rgb_effect_breath_percent(32U) == 70U);
    assert(service_v2_rgb_effect_breath_percent(64U) == 18U);

    service_v2_rgb_effect_policy_color(&policy, 0U, &color);
    assert(color.red == 0U && color.green == 0U && color.blue == 0U);
    return 0;
}
