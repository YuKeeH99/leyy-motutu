#ifndef SERVICE_V2_RGB_SHOW_H
#define SERVICE_V2_RGB_SHOW_H

#include "service_v2_rgb_effect_policy.h"

typedef enum {
    SERVICE_V2_RGB_SHOW_NONE = 0,
    SERVICE_V2_RGB_SHOW_BOOT,
    SERVICE_V2_RGB_SHOW_DANCE,
} service_v2_rgb_show_kind_t;

/* Owned by the RGB service; callers serialize these short state operations. */
typedef struct {
    service_v2_rgb_show_kind_t kind;
    uint32_t started_ms;
    bool enabled;
    bool boot_consumed;
} service_v2_rgb_show_t;

static inline void service_v2_rgb_show_cancel(service_v2_rgb_show_t *show)
{
    show->kind = SERVICE_V2_RGB_SHOW_NONE;
    show->boot_consumed = true;
}

static inline void service_v2_rgb_show_enable(service_v2_rgb_show_t *show, bool enabled)
{
    show->enabled = enabled;
    if (!enabled) service_v2_rgb_show_cancel(show);
}

static inline void service_v2_rgb_show_start(service_v2_rgb_show_t *show,
                                            service_v2_rgb_show_kind_t kind, uint32_t now_ms)
{
    if (!show->enabled || kind == SERVICE_V2_RGB_SHOW_NONE) return;
    if (kind == SERVICE_V2_RGB_SHOW_BOOT && show->boot_consumed) return;
    if (show->kind == kind) return; /* Repeated PLAYING cannot reset the clock. */
    show->boot_consumed = true;
    show->kind = kind;
    show->started_ms = now_ms;
}

static inline void service_v2_rgb_show_stop(service_v2_rgb_show_t *show,
                                           service_v2_rgb_show_kind_t kind)
{
    if (kind == SERVICE_V2_RGB_SHOW_BOOT) show->boot_consumed = true;
    if (show->kind == kind) show->kind = SERVICE_V2_RGB_SHOW_NONE;
}

static inline service_v2_rgb_show_kind_t service_v2_rgb_show_current(
    service_v2_rgb_show_t *show, uint32_t now_ms)
{
    uint32_t timeout = show->kind == SERVICE_V2_RGB_SHOW_BOOT ? 30000U : 120000U;
    if (!show->enabled || (uint32_t)(now_ms - show->started_ms) >= timeout)
        show->kind = SERVICE_V2_RGB_SHOW_NONE;
    return show->kind;
}

static inline void service_v2_rgb_show_frame(service_v2_rgb_show_kind_t kind,
                                             uint32_t elapsed_ms, service_v2_rgb_color_t pixels[5])
{
    for (unsigned i = 0; i < 5U; ++i) {
        if (kind == SERVICE_V2_RGB_SHOW_BOOT) {
            /* Cyan head glides 1→5→1 with a soft blue halo. */
            unsigned phase = elapsed_ms % 1600U;
            unsigned position = phase <= 800U ? phase : 1600U - phase;
            unsigned coordinate = i * 200U;
            unsigned distance = position > coordinate ? position - coordinate : coordinate - position;
            unsigned level = distance >= 400U ? 0U : (400U - distance) * 96U / 400U;
            pixels[i] = (service_v2_rgb_color_t){0U, (uint8_t)(8U + level), (uint8_t)(16U + level)};
        } else {
            /* A 1 s blue/purple pulse, with paired edges and a softer center. */
            unsigned phase = elapsed_ms % 1000U;
            unsigned ramp = phase <= 500U ? phase : 1000U - phase;
            unsigned pulse = ramp * ramp * (1500U - 2U * ramp) / 2500000U;
            unsigned pair = i <= 2U ? i : 4U - i;
            pixels[i] = (service_v2_rgb_color_t){(uint8_t)(12U + pulse / 2U),
                (uint8_t)(10U + pair * 3U), (uint8_t)(36U + pulse + pair * 5U)};
        }
    }
}

#endif
