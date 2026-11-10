#ifndef SERVICE_V2_VOICE_STANDBY_H
#define SERVICE_V2_VOICE_STANDBY_H

#include "service_v2_voice_standby_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the five-minute voice-standby coordinator. */
int service_v2_voice_standby_init(void);

/** Record local/user activity and resume asynchronously when asleep. */
void service_v2_voice_standby_note_activity(void);
/** ESR ingress: refresh automatic shutdown deadline without waking peripherals. */
void service_v2_voice_standby_note_keyword(void);
/** Power worker returned; allow another full standby interval before retry. */
void service_v2_voice_standby_shutdown_cancelled(void);
/** Accepted keyword/single click: false; explicit double-click reset: true. */
int service_v2_voice_standby_wake_pose(bool assembly);
/* Worker context only: never wait on the voice bus that delivers wake events.
 * Returns 0 once local peripherals and the Wi-Fi radio are ready (IP/Internet
 * may still be connecting), negative errno on timeout. */
int service_v2_voice_standby_resume_wait(uint32_t timeout_ms);

/** Request standby as soon as all dialogue, playback, OTA and motion are idle. */
int service_v2_voice_standby_request(void);

service_v2_voice_standby_state_t service_v2_voice_standby_get_state(void);
const char *service_v2_voice_standby_state_name(service_v2_voice_standby_state_t state);

/** True while asynchronous network/IMU prompts must remain silent. */
bool service_v2_voice_standby_background_audio_suppressed(void);

#ifdef __cplusplus
}
#endif

#endif
