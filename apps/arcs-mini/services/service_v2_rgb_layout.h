#ifndef SERVICE_V2_RGB_LAYOUT_H
#define SERVICE_V2_RGB_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* Physical positions are 1-based: eyes at 2 and 4; all other pixels dark.
 * Apply after channel calibration, before entering the timed wire loop. */
static inline uint32_t service_v2_rgb_pixel_word(uint32_t index, uint32_t word, bool eyes)
{
    return !eyes || index == 1U || index == 3U ? word : 0U;
}

#endif
