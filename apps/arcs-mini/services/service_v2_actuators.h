#ifndef SERVICE_V2_ACTUATORS_H
#define SERVICE_V2_ACTUATORS_H

#include <stdbool.h>
#include <stdint.h>
#include "service_v2_rgb_effect_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ARCS Mini V2 actuator connector allocation:
 *
 *   servo 0 -> PA04 / GPT PWM channel 4
 *   servo 1 -> PA05 / GPT PWM channel 5
 *   servo 2 -> PA06 / GPT PWM channel 6
 *   servo 3 -> PA07 / GPT PWM channel 7
 *   RGB     -> PA08 / 1..64 daisy-chained WS2812-compatible LEDs (GRB wire order)
 *
 * This service owns PA04..PA08 after initialization. PA08 uses a measured,
 * absolute-deadline SRAM/GPIO waveform; PA04..PA07 use GPIO func0 while their
 * servos are disabled.
 */
#define SERVICE_V2_SERVO_COUNT           4U
#define SERVICE_V2_SERVO_FREQUENCY_HZ    50U
#define SERVICE_V2_SERVO_MIN_PULSE_US    500U
#define SERVICE_V2_SERVO_CENTER_PULSE_US 1500U
#define SERVICE_V2_SERVO_MAX_PULSE_US    2500U

/* One full end-stop-to-end-stop-and-back cycle. */
#define SERVICE_V2_SERVO_SWEEP_MIN_PERIOD_MS 400U
#define SERVICE_V2_SERVO_SWEEP_MAX_PERIOD_MS 8000U
#define SERVICE_V2_SERVO_SWEEP_DEF_PERIOD_MS 1600U

/* A zero-duration frame is an immediate synchronized update. */
#define SERVICE_V2_SERVO_FRAME_MAX_DURATION_MS 60000U
#define SERVICE_V2_SERVO_FRAME_STEP_MS         20U
#define SERVICE_V2_SERVO_ALL_MASK              ((1U << SERVICE_V2_SERVO_COUNT) - 1U)
#define SERVICE_V2_SERVO_WATCHDOG_MIN_MS       200U
#define SERVICE_V2_SERVO_WATCHDOG_MAX_MS       5000U

typedef enum {
    SERVICE_V2_SERVO_EASING_LINEAR = 0,
    SERVICE_V2_SERVO_EASING_SMOOTH,
    SERVICE_V2_SERVO_EASING_EASE_IN,
    SERVICE_V2_SERVO_EASING_EASE_OUT,
} service_v2_servo_easing_t;

typedef struct {
    uint32_t sequence;
    uint32_t duration_ms;
    uint32_t elapsed_ms;
    uint32_t watchdog_ms;
    uint32_t watchdog_remaining_ms;
    uint16_t current_us[SERVICE_V2_SERVO_COUNT];
    uint16_t target_us[SERVICE_V2_SERVO_COUNT];
    uint8_t enabled_mask;
    uint8_t sweep_servo;
    service_v2_servo_easing_t easing;
    bool frame_active;
    bool sweep_active;
    bool watchdog_expired;
    bool usb_idle_released;
} service_v2_servo_status_t;

#define SERVICE_V2_RGB_STRIP_MIN_COUNT        1U
#define SERVICE_V2_RGB_STRIP_DEFAULT_COUNT    5U
#define SERVICE_V2_RGB_STRIP_MAX_COUNT        64U
#define SERVICE_V2_RGB_BRIGHTNESS_MAX_PERCENT 100U
#define SERVICE_V2_RGB_CALIBRATION_VERSION    1U
#define SERVICE_V2_RGB_GAIN_MIN_PERMILLE      0U
#define SERVICE_V2_RGB_GAIN_MAX_PERMILLE      1000U
#define SERVICE_V2_RGB_GAMMA_MIN_X100         100U
#define SERVICE_V2_RGB_GAMMA_MAX_X100         300U

typedef enum {
    SERVICE_V2_RGB_OWNER_SYSTEM = 0,
    SERVICE_V2_RGB_OWNER_MANUAL,
    SERVICE_V2_RGB_OWNER_ACTION,
} service_v2_rgb_owner_t;

typedef struct {
    uint8_t count;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness_percent;
    service_v2_rgb_owner_t owner;
} service_v2_rgb_status_t;

/** Byte order consumed by one physical WS2812-compatible pixel. */
typedef enum {
    SERVICE_V2_RGB_ORDER_RGB = 0,
    SERVICE_V2_RGB_ORDER_RBG,
    SERVICE_V2_RGB_ORDER_GRB,
    SERVICE_V2_RGB_ORDER_GBR,
    SERVICE_V2_RGB_ORDER_BRG,
    SERVICE_V2_RGB_ORDER_BGR,
} service_v2_rgb_order_t;

/**
 * Persistent output calibration. API/status colours remain logical RGB;
 * calibration is applied only when the physical 24-bit pixel word is built.
 */
typedef struct {
    uint16_t version;
    service_v2_rgb_order_t order;
    uint16_t red_gain_permille;
    uint16_t green_gain_permille;
    uint16_t blue_gain_permille;
    uint16_t gamma_x100;
} service_v2_rgb_calibration_t;

/**
 * Initialize the four servo outputs and the PA08 RGB strip sender.
 *
 * All servo pins are left as GPIO-low, so initialization produces no servo
 * pulses. Call service_v2_actuators_servo_enable() explicitly per channel.
 * PA08 is held low during initialization and no WS2812 frame is emitted.
 *
 * @return 0 on success, negative errno-style value on failure.
 */
int service_v2_actuators_init(void);

bool service_v2_actuators_is_ready(void);

/**
 * Set a servo's high-pulse width without implicitly enabling it.
 *
 * If the channel is already enabled, the new width is applied at the next GPT
 * shadow load. Valid pulse widths are 500..2500 us at 50 Hz.
 */
int service_v2_actuators_servo_set_us(uint8_t servo, uint16_t pulse_us);

int service_v2_actuators_servo_enable(uint8_t servo);
int service_v2_actuators_servo_disable(uint8_t servo);
int service_v2_actuators_servo_disable_all(void);

/** Release held PWM only with USB present and no active frame/sweep/manual
 * lease, atomically with the actuator owner check. Does not change pulse
 * targets or block a later explicit frame/enable. Caller must also exclude
 * queued or running gait work. Returns -EBUSY instead of interrupting work. */
int service_v2_actuators_servo_release_usb_idle(void);

/**
 * Move all four servos to one synchronized target frame.
 *
 * The command is non-blocking.  A duration of zero applies the four target
 * pulses immediately; otherwise a single worker interpolates every 20 ms and
 * shadow-loads all four GPT channels together.  Starting a frame while the
 * legacy sweep worker is active returns -EBUSY.  Outputs remain enabled after
 * the target is reached so the pose is held.
 */
int service_v2_actuators_servo_frame(const uint16_t pulse_us[SERVICE_V2_SERVO_COUNT], uint32_t duration_ms,
                                     service_v2_servo_easing_t easing);

/** Cancel an active frame and hold the last commanded position. */
int service_v2_actuators_servo_frame_stop(void);

/** Return the latest commanded state (standard PWM servos provide no feedback). */
int service_v2_actuators_servo_get_status(service_v2_servo_status_t *status);

/** Configure a manual-control lease. Zero disables it; valid non-zero values are 200..5000 ms. */
int service_v2_actuators_servo_watchdog_set(uint32_t timeout_ms);

/** Refresh the configured lease while one or more servo outputs are enabled. */
int service_v2_actuators_servo_watchdog_kick(void);

/**
 * Continuously sweep one servo between its end stops until stopped.
 *
 * Unlike servo_test(), this returns as soon as the worker task is running, so
 * the shell stays free and the host can keep polling the other sensors. The
 * channel is enabled on start and disabled again once the sweep stops. Only
 * one channel can sweep at a time. Periods outside the supported range are
 * rejected rather than clamped.
 */
int service_v2_actuators_servo_sweep_start(uint8_t servo, uint16_t period_ms);

/** Stop the sweep and leave the channel disabled. Safe to call when idle. */
int service_v2_actuators_servo_sweep_stop(void);

/** Report whether a sweep is running, and on which channel. */
bool service_v2_actuators_servo_sweep_active(uint8_t *servo);

/** System-indicator compatibility API; ignored while MANUAL/ACTION owns the strip. */
int service_v2_actuators_rgb_set(uint8_t red, uint8_t green, uint8_t blue);
int service_v2_actuators_rgb_off(void);
/** Five logical RGB pixels for system shows; honours ownership and calibration. */
int service_v2_actuators_rgb_system_frame(const service_v2_rgb_color_t pixels[5]);

/**
 * Fill a daisy-chained WS2812 strip with one colour.
 *
 * count is 1..64 and brightness_percent is 0..100.  Red/green/blue retain the
 * conventional API order; the sender converts each pixel to GRB wire order.
 */
int service_v2_actuators_rgb_strip_set(uint8_t count, uint8_t red, uint8_t green, uint8_t blue,
                                       uint8_t brightness_percent);

/** Change the active strip length and clear both the old and new extent. */
int service_v2_actuators_rgb_strip_set_count(uint8_t count);

int service_v2_actuators_rgb_get_status(service_v2_rgb_status_t *status);

/** Return the active calibration and whether it is backed by a valid KV record. */
int service_v2_actuators_rgb_calibration_get(service_v2_rgb_calibration_t *calibration, bool *persistent);

/** Validate, persist and immediately redraw the current logical colour. */
int service_v2_actuators_rgb_calibration_set(const service_v2_rgb_calibration_t *calibration);

/** Persist the bit-for-bit-compatible GRB/gain-1000/gamma-1.00 defaults. */
int service_v2_actuators_rgb_calibration_reset(void);

/** Stable uppercase name used by the shell protocol. */
const char *service_v2_actuators_rgb_order_name(service_v2_rgb_order_t order);

/** Release MANUAL/ACTION ownership back to system indicators and turn the strip off. */
int service_v2_actuators_rgb_release(void);

/** Atomically validate/register a four-servo target and publish its action light. */
int service_v2_actuators_motion_frame(const uint16_t pulse_us[SERVICE_V2_SERVO_COUNT], uint32_t duration_ms,
                                      service_v2_servo_easing_t easing, uint8_t count, uint8_t red, uint8_t green,
                                      uint8_t blue, uint8_t brightness_percent);

/** Hold PA08/LED1-DI at a static level for board-level diagnostics. */
int service_v2_actuators_rgb_data_level(bool high);

/**
 * Drive PA08 through its normal GPIO func1/DOUT path and read the pad level
 * back.  Unlike rgb_data_level(), this does not use the IOMUX force path.
 */
int service_v2_actuators_rgb_gpio_level(bool high, bool *readback_high);

/** Report CPU, delay-derived diagnostic, and sender MCYCLE rates. */
int service_v2_actuators_rgb_clock_measure(uint32_t *reported_hz, uint32_t *measured_hz, uint32_t *cached_hz);

/** Continuously emit identical WS2812 bits for stable oscilloscope capture. */
int service_v2_actuators_rgb_scope_start(bool one);
int service_v2_actuators_rgb_scope_stop(void);

/**
 * Synchronously show dim red, green and blue for a short hardware self-test,
 * then switch the LED off. This function must be called from task context.
 */
int service_v2_actuators_rgb_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_V2_ACTUATORS_H */
