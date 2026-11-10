#ifndef SERVICE_V2_NFC_PEER_H
#define SERVICE_V2_NFC_PEER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERVICE_V2_NFC_PEER_ID_LEN  8U
#define SERVICE_V2_NFC_PEER_UID_LEN 4U

typedef enum {
    SERVICE_V2_NFC_PEER_ROLE_IDLE = 0,
    SERVICE_V2_NFC_PEER_ROLE_READER,
    SERVICE_V2_NFC_PEER_ROLE_TARGET,
} service_v2_nfc_peer_role_t;

typedef struct {
    bool enabled;
    bool present;
    service_v2_nfc_peer_role_t role;
    uint8_t local_id[SERVICE_V2_NFC_PEER_ID_LEN];
    uint8_t peer_id[SERVICE_V2_NFC_PEER_ID_LEN];
    uint8_t peer_uid[SERVICE_V2_NFC_PEER_UID_LEN];
    uint32_t last_seen_age_ms;
    uint32_t seen_count;
    uint32_t enter_count;
    uint32_t leave_count;
    uint32_t advertise_window_count;
    int last_error;
} service_v2_nfc_peer_status_t;

/*
 * Starts one low-priority worker with a board-ID-bound PN532 ISO/IEC 14443-4
 * Type-A role. The 1634 board is always the reader and the 4050/1d41 boards
 * are always the PICC target. A successful RF session exchanges both 64-bit
 * chip IDs in a versioned CRC-protected packet, so both peers recognize each
 * other during the same encounter.
 */
int service_v2_nfc_peer_init(void);
int service_v2_nfc_peer_start(void);
int service_v2_nfc_peer_stop(void);
/* Runs one synchronous reader or target exchange while background discovery
 * is stopped. Intended for deterministic link bring-up and production test. */
int service_v2_nfc_peer_run_once(service_v2_nfc_peer_role_t role);
int service_v2_nfc_peer_get_status(service_v2_nfc_peer_status_t *status);
const char *service_v2_nfc_peer_role_name(service_v2_nfc_peer_role_t role);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_V2_NFC_PEER_H */
