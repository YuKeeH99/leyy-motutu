#ifndef SERVICE_V2_RGB_EFFECT_POLICY_H
#define SERVICE_V2_RGB_EFFECT_POLICY_H

#include <stdint.h>
#include <stdbool.h>

#define SERVICE_V2_RGB_FRAME_PERIOD_MS 50U
bool service_v2_rgb_effect_frame_due(uint32_t elapsed_ms, uint32_t *pending_ms);

typedef enum {
    SERVICE_V2_RGB_EFFECT_IDLE = 0,
    SERVICE_V2_RGB_EFFECT_LISTENING,
    SERVICE_V2_RGB_EFFECT_SPEAKING,
} service_v2_rgb_effect_state_t;

typedef enum {
    SERVICE_V2_RGB_EVENT_WAKEUP = 0,
    SERVICE_V2_RGB_EVENT_SESSION_STARTING,
    SERVICE_V2_RGB_EVENT_TTS_PLAYING,
    SERVICE_V2_RGB_EVENT_TTS_STOPPED,
    SERVICE_V2_RGB_EVENT_SESSION_FINISHED,
    SERVICE_V2_RGB_EVENT_DISCONNECTED,
    SERVICE_V2_RGB_EVENT_DIZZY,
} service_v2_rgb_effect_event_t;

typedef enum {
    SERVICE_V2_RGB_POWER_DISCHARGING = 0,
    SERVICE_V2_RGB_POWER_CHARGING,
    SERVICE_V2_RGB_POWER_CHARGE_DONE,
} service_v2_rgb_power_state_t;

typedef enum {
    SERVICE_V2_RGB_PROVISIONING_INACTIVE = 0,
    SERVICE_V2_RGB_PROVISIONING_ACTIVE,
    SERVICE_V2_RGB_PROVISIONING_SUCCESS,
} service_v2_rgb_provisioning_state_t;

typedef enum {
    SERVICE_V2_RGB_EMOTION_NEUTRAL = 0,
    SERVICE_V2_RGB_EMOTION_HAPPY,
    SERVICE_V2_RGB_EMOTION_SAD,
    SERVICE_V2_RGB_EMOTION_ANGRY,
    SERVICE_V2_RGB_EMOTION_FEARFUL,
    SERVICE_V2_RGB_EMOTION_SURPRISED,
    SERVICE_V2_RGB_EMOTION_DISGUSTED,
} service_v2_rgb_emotion_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} service_v2_rgb_color_t;

typedef enum {
    SERVICE_V2_RGB_OTA_NONE = 0,
    SERVICE_V2_RGB_OTA_UPDATING,
    SERVICE_V2_RGB_OTA_SUCCESS,
    SERVICE_V2_RGB_OTA_FAILED,
} service_v2_rgb_ota_state_t;

typedef struct {
    service_v2_rgb_effect_state_t state;
    service_v2_rgb_power_state_t power_state;
    service_v2_rgb_emotion_t emotion;
    uint8_t battery_level;
    bool low_battery;
    bool network_unavailable;
    uint32_t alert_elapsed_ms;
    bool forced_offline;
    uint32_t offline_elapsed_ms;
    bool muted;
    uint32_t mute_elapsed_ms;
    service_v2_rgb_provisioning_state_t provisioning_state;
    uint32_t provisioning_elapsed_ms;
    bool dizzy_active;
    uint32_t dizzy_elapsed_ms;
    service_v2_rgb_ota_state_t ota_state;
    uint32_t ota_elapsed_ms;
} service_v2_rgb_effect_policy_t;

void service_v2_rgb_effect_policy_ota(service_v2_rgb_effect_policy_t *policy,
                                     service_v2_rgb_ota_state_t state);

void service_v2_rgb_effect_policy_network(service_v2_rgb_effect_policy_t *policy,
                                          bool connected);
void service_v2_rgb_effect_policy_offline(service_v2_rgb_effect_policy_t *policy,
                                          bool forced_offline);
void service_v2_rgb_effect_policy_mute(service_v2_rgb_effect_policy_t *policy, bool muted);
/* Exact alert frames bypass breathing/crossfade and cover all five pixels. */
bool service_v2_rgb_effect_policy_alert_frame(const service_v2_rgb_effect_policy_t *policy,
                                               service_v2_rgb_color_t pixels[5]);

void service_v2_rgb_effect_policy_init(service_v2_rgb_effect_policy_t *policy, uint32_t seed);
void service_v2_rgb_effect_policy_event(service_v2_rgb_effect_policy_t *policy,
                                        service_v2_rgb_effect_event_t event);
void service_v2_rgb_effect_policy_battery(service_v2_rgb_effect_policy_t *policy,
                                          uint8_t level,
                                          service_v2_rgb_power_state_t power_state);
void service_v2_rgb_effect_policy_provisioning(
    service_v2_rgb_effect_policy_t *policy,
    service_v2_rgb_provisioning_state_t state);
bool service_v2_rgb_effect_policy_emotion(service_v2_rgb_effect_policy_t *policy,
                                          const char *emotion);
void service_v2_rgb_effect_policy_tick(service_v2_rgb_effect_policy_t *policy,
                                       uint32_t elapsed_ms);
uint8_t service_v2_rgb_effect_breath_percent(uint32_t phase);
void service_v2_rgb_effect_policy_color(const service_v2_rgb_effect_policy_t *policy,
                                        uint8_t brightness_percent,
                                        service_v2_rgb_color_t *color);

#endif
