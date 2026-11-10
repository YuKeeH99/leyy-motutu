#include "lisa_kv.h"

#include "kv.h"
#include "service_brightness.h"

#define DEFAULT_BRIGHTNESS 70

static int service_brightness_clamp(int brightness)
{
    if (brightness < 0) {
        return 0;
    }
    if (brightness > 100) {
        return 100;
    }
    return brightness;
}

void service_brightness_init(void)
{
}

void service_brightness_set(int brightness)
{
    lisa_kv_set_int(KV_KEY_USER_BRIGHTNESS, service_brightness_clamp(brightness));
}

void service_brightness_set_temp(int brightness)
{
    (void)brightness;
}

int service_brightness_get(void)
{
    int brightness = DEFAULT_BRIGHTNESS;

    if (lisa_kv_get_int(KV_KEY_USER_BRIGHTNESS, &brightness) != 0) {
        return DEFAULT_BRIGHTNESS;
    }

    return service_brightness_clamp(brightness);
}
