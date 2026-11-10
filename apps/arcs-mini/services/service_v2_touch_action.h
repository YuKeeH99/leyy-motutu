#ifndef SERVICE_V2_TOUCH_ACTION_H
#define SERVICE_V2_TOUCH_ACTION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the V2.1 two-channel touch-to-action bridge.
 *
 * PA09 triggers a left-front-leg handshake; PA13 keeps the existing
 * right-front-leg kick action. Both TTP223 channels are handled as
 * toggle/latched outputs, so one debounced transition in either direction
 * represents one touch. Startup first needs
 * a continuous stable arming window and never treats the initial level as an
 * action. A trigger is accepted only while the gait/actuator services are
 * idle and either all PWM outputs are disabled at cold start or all four are
 * enabled at the calibrated normal HOME; reverse HOME, partial enable and host
 * leases and a connected Type-C cable are rejected. With safe battery voltage
 * and a normal-face IMU check, the gait service runs the channel-specific
 * action and returns to the appropriate safe pose. Call this after battery_init(),
 * service_v2_sensors_init(), and service_v2_gait_init().
 *
 * @return 0 on success, or a negative errno/sensor-service error on failure.
 */
int service_v2_touch_action_init(void);

/** Pause or resume touch polling. Re-enable always establishes a fresh baseline. */
int service_v2_touch_action_set_enabled(int enabled);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_V2_TOUCH_ACTION_H */
