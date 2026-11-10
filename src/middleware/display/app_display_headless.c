#include <stdint.h>

#include "lisa_kv.h"
#include "kv.h"

#include "app_display.h"

#define APP_DISPLAY_BRIGHTNESS_DEFAULT 70

int app_display_set_brightness(uint8_t val)
{
    return lisa_kv_set_int(KV_KEY_USER_BRIGHTNESS, val);
}

uint8_t app_display_get_brightness(void)
{
    int val = APP_DISPLAY_BRIGHTNESS_DEFAULT;

    if (lisa_kv_get_int(KV_KEY_USER_BRIGHTNESS, &val) != 0) {
        return APP_DISPLAY_BRIGHTNESS_DEFAULT;
    }

    if (val < 0) {
        return 0;
    }
    if (val > UINT8_MAX) {
        return UINT8_MAX;
    }
    return (uint8_t)val;
}

int app_display_brightness_init(void)
{
    return 0;
}
