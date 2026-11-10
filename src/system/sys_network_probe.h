#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t owner;
    uint32_t generation;
} sys_network_probe_event_t;

/* Also accepts legacy empty/owner-only events, subject to current state. */
bool network_probe_event_current(const void *data, uint32_t len, bool connected);

int network_probe_init(void);
/* Radio/configuration changed: retire results from the previous request. */
int network_probe_start(void);
/* Transport/business error: coalesce a recheck without cancelling one in flight. */
int network_probe_refresh(void);
