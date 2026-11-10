#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "service_v2_shake_detector.h"

static uint32_t peak;
static bool sample(service_v2_shake_detector_t *d, uint32_t t, int16_t x, int16_t y, int16_t z)
{
    peak = 123U;
    bool fired = service_v2_shake_detector_update(d, t, x, y, z, &peak);
    if (!fired) assert(peak == 0U);
    return fired;
}
static void reset(service_v2_shake_detector_t *d, uint32_t now)
{
    service_v2_shake_detector_reset(d, true);
    assert(!sample(d, now, 0, 0, 0));
}
static void five_seconds_required_on_every_axis(void)
{
    for (unsigned axis = 0; axis < 3; ++axis) {
        service_v2_shake_detector_t d;
        reset(&d, 0U);
        for (uint32_t t = 20U; t <= 5020U; t += 20U) {
            int16_t gyro[3] = {0};
            gyro[axis] = (t / 200U) % 2 ? -32750 : 32750;
            assert(sample(&d, t, gyro[0], gyro[1], gyro[2]) == (t == 5020U));
        }
        assert(peak == 32750U && !d.window_active);
        for (uint32_t t = 5040U; t < 10040U; t += 20U)
            assert(!sample(&d, t, 32750, 0, 0));
        assert(sample(&d, 10040U, 32750, 0, 0));
    }
}
static void thresholds_and_diagonal_counting(void)
{
    service_v2_shake_detector_t d;
    reset(&d, 0U);
    for (uint32_t t = 20U; t <= 10000U; t += 20U)
        assert(!sample(&d, t, 3930, -3930, 3930)); /* 30 dps threshold */
    reset(&d, 0U);
    /* 60 dps * 5 seconds = 300 degrees: old threshold passed, new one not. */
    for (uint32_t t = 20U; t < 6020U; t += 20U)
        assert(!sample(&d, t, 7860, -7860, 7860));
    assert(sample(&d, 6020U, 7860, -7860, 7860));
}
static void continuity_and_short_reversals(void)
{
    service_v2_shake_detector_t d;
    reset(&d, 0U);
    for (uint32_t t = 20U; t <= 3020U; t += 20U)
        assert(!sample(&d, t, 32750, 0, 0));
    for (uint32_t t = 3040U; t <= 3260U; t += 20U)
        assert(!sample(&d, t, 0, 0, 0));
    for (uint32_t t = 3280U; t < 8280U; t += 20U)
        assert(!sample(&d, t, -32750, 0, 0));
    assert(sample(&d, 8280U, -32750, 0, 0));
    reset(&d, 0U);
    for (uint32_t t = 20U; t <= 5020U; t += 20U) {
        int16_t rate = (t >= 3040U && t <= 3200U) ? 0 : 32750;
        assert(sample(&d, t, rate, 0, 0) == (t == 5020U));
    }
}
static void lost_samples_and_reset(void)
{
    service_v2_shake_detector_t d;
    reset(&d, 0U);
    for (uint32_t t = 20U; t <= 3020U; t += 20U)
        assert(!sample(&d, t, 32750, 0, 0));
    assert(!sample(&d, 4000U, 32750, 0, 0));
    for (uint32_t t = 4020U; t < 9020U; t += 20U)
        assert(!sample(&d, t, 32750, 0, 0));
    assert(sample(&d, 9020U, 32750, 0, 0));
    service_v2_shake_detector_reset(&d, false);
    assert(!sample(&d, 9040U, 32750, 0, 0) && !d.window_active);
}
static void timestamp_wrap_duplicates_and_int16_min(void)
{
    service_v2_shake_detector_t d;
    uint32_t base = UINT32_MAX - 2000U;
    reset(&d, base);
    for (uint32_t t = 20U; t <= 5020U; t += 20U) {
        assert(sample(&d, base + t, INT16_MIN, 0, 0) == (t == 5020U));
        if (t < 5020U) assert(!sample(&d, base + t, INT16_MIN, 0, 0));
    }
    assert(peak == 32768U);
}
int main(void)
{
    five_seconds_required_on_every_axis();
    thresholds_and_diagonal_counting();
    continuity_and_short_reversals();
    lost_samples_and_reset();
    timestamp_wrap_duplicates_and_int16_min();
    return 0;
}
