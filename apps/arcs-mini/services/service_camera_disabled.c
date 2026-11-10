#include "service_camera.h"

#include <errno.h>
#include <stddef.h>

#include "lisa_log.h"

#define TAG "service_camera"

/*
 * The V2.1 hardware has no camera.  Keep the public service ABI available for
 * the shared UI model, but never touch a camera, DVP, I2C, or pinmux device.
 */
int service_camera_init(void)
{
    LISA_LOGI(TAG, "camera is not populated on arcs_mini_v2_1");
    return -ENODEV;
}

int service_camera_capture(uint8_t *buffer, uint32_t buffer_len)
{
    (void)buffer;
    (void)buffer_len;
    return -ENODEV;
}

int service_camera_stop(void)
{
    return 0;
}

int service_camera_get_framesize(uint16_t *width, uint16_t *height)
{
    if (width == NULL || height == NULL) {
        return -EINVAL;
    }

    *width = 0U;
    *height = 0U;
    return -ENODEV;
}

bool service_camera_is_inited(void)
{
    return false;
}
