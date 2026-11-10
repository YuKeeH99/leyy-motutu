#ifndef SERVICE_OFFLINE_ACTION_H
#define SERVICE_OFFLINE_ACTION_H
#include <stdbool.h>
#include <stdint.h>
int service_offline_action_init(void);
bool service_offline_action_active(void);
bool service_offline_action_background_allowed(void);
bool service_offline_action_forced_offline(void);
bool service_offline_action_network_prompt_allowed(void);
/* False for a wake received while a saved-network attempt was already busy. */
bool service_offline_action_wake_network_allowed(uint32_t generation);
/* Prevent the generic fallback poll from racing the wake-owned scan decision. */
bool service_offline_action_network_busy(void);
/* Network work follows explicit mode changes, independently of ordinary wakes. */
uint32_t service_offline_action_network_epoch(void);
/* Transport ownership survives ordinary wakes; mode/provision changes invalidate it. */
bool service_offline_action_network_current(uint32_t epoch);
bool service_offline_action_local_audio_current(uint32_t generation);
void service_offline_action_local_audio_finished(uint32_t generation, bool ok);
/* RPC callback: bounded copy/queue only. True means consumed, including drops. */
bool service_offline_action_handle_result(const char *intent, bool keyword);
void service_offline_action_cancel(void);
/* Button/manual control uses the same deferred worker as voice commands. */
void service_offline_action_request_netcfg(void);
/* Completed button gestures exit offline; return true only when click was consumed. */
bool service_offline_action_button_exit(uint32_t action);
bool service_offline_action_begin_button(void);
typedef enum {
    SERVICE_OFFLINE_BUTTON_REJECTED = 0,
    SERVICE_OFFLINE_BUTTON_ONLINE,
    SERVICE_OFFLINE_BUTTON_OFFLINE,
} service_offline_button_result_t;
/* Compare and claim in one critical section. Offline acceptance is already
 * handed to the offline worker; callers must not publish an online wake. */
service_offline_button_result_t service_offline_action_begin_button_for_generation(
    uint32_t expected_generation, uint32_t *accepted_generation);
/* Explicit stop API; no stop/exit phrase is claimed in the shipped ESR resource. */
void service_offline_action_stop(void);
void service_offline_action_shutdown(void);
void service_offline_action_shutdown_cancelled(void);
void service_offline_action_engine_idle(void);
void service_offline_action_prompt_finished(uint32_t generation, bool ok);
bool service_offline_action_prompt_current(uint32_t generation);
uint32_t service_offline_action_generation(void);
bool service_offline_action_online_current(uint32_t generation);
void service_offline_action_cloud_failed(uint32_t generation);
void service_offline_action_cloud_connected(uint32_t generation);
/* Goodbye retires only its own command window, preserving audio/network/mode. */
void service_offline_action_close_online_window(uint32_t generation);
/* Shell worker only. Nonblocking acquire excludes every product AP control.
 * release is called after the diagnostic has restored WAKEUP. */
bool service_offline_action_diagnostic_acquire(void);
void service_offline_action_diagnostic_release(bool restored);
#endif
