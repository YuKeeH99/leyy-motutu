#ifndef SERVICE_BUTTON_H
#define SERVICE_BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include "voice_msg.h"

typedef enum {
    SERVICE_BUTTON_DIALOG_NONE = 0,
    SERVICE_BUTTON_DIALOG_WAKE,
    SERVICE_BUTTON_DIALOG_EXIT,
} service_button_dialog_intent_t;

/* Keep the ordinary button event as the prefix for existing subscribers. */
typedef struct {
    voice_msg_button_evt_t button;
    uint32_t gesture_id;
    uint32_t owner_generation;
    service_button_dialog_intent_t dialog_intent;
} service_button_event_t;

/* Immutable accepted owner, shared by all BUTTON_START subscribers. */
typedef struct {
    uint32_t gesture_id;
    uint32_t owner_generation;
} service_button_wakeup_t;

bool service_button_dialog_click_current(const void *data, uint32_t len);
bool service_button_wakeup_request(const service_button_event_t *event);
/* NULL/0 retains the legacy unowned wake event contract. */
bool service_button_wakeup_event_current(const void *data, uint32_t len);

int service_button_init(void);

#endif
