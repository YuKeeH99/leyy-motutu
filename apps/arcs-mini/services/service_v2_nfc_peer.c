#include "service_v2_nfc_peer.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_log.h"
#include "lisa_mutex.h"
#include "lisa_thread.h"
#include "service_v2_actuators.h"
#include "service_v2_sensors.h"

#define TAG "v2.nfc.peer"

#define NFC_PEER_PROTOCOL_VERSION        0x01U
#define NFC_PEER_PAYLOAD_LEN             14U
#define NFC_PEER_TARGET_WINDOW_MS        1500U
#define NFC_PEER_SCAN_WINDOW_MS          1500U
#define NFC_PEER_SCAN_ATTEMPT_TIMEOUT_MS 750U
#define NFC_PEER_SCAN_ATTEMPT_GAP_MS     10U
#define NFC_PEER_BOOT_MIN_MS             600U
#define NFC_PEER_LOOP_GAP_MS             15U
#define NFC_PEER_ERROR_BACKOFF_MS        100U
#define NFC_PEER_LOST_TIMEOUT_MS         6000U
#define NFC_PEER_STOP_WAIT_MS            6000U
#define NFC_PEER_ROLE_LED_LEVEL          50U

static const uint8_t s_peer_magic[4] = {'A', 'R', 'C', 'S'};
/* Reader: 板 A 旧板 6b3bc01b1642ccbd */
static const uint8_t s_reader_id[SERVICE_V2_NFC_PEER_ID_LEN] = {
    0x6bU, 0x3bU, 0xc0U, 0x1bU, 0x16U, 0x42U, 0xccU, 0xbdU,
};
static const uint8_t s_target_ids[][SERVICE_V2_NFC_PEER_ID_LEN] = {
    /* 板 B 新板 6b3bc01b1442ccbd */
    {0x6bU, 0x3bU, 0xc0U, 0x1bU, 0x14U, 0x42U, 0xccU, 0xbdU},
};
#define NFC_PEER_TARGET_ID_COUNT (sizeof(s_target_ids) / sizeof(s_target_ids[0]))
#define NFC_PEER_LATENCY_UNKNOWN UINT32_MAX

typedef struct {
    lisa_thread_t *thread;
    lisa_mutex_t *lock;
    service_v2_nfc_peer_status_t status;
    TickType_t last_seen_tick;
    uint8_t identity_packet[NFC_PEER_PAYLOAD_LEN];
    service_v2_nfc_peer_role_t fixed_role;
} nfc_peer_context_t;

static nfc_peer_context_t s_peer;

static bool nfc_peer_expected_miss(int error);

static void nfc_peer_lock(void)
{
    (void)lisa_mutex_lock(s_peer.lock, LISA_WAIT_FOREVER);
}

static void nfc_peer_unlock(void)
{
    lisa_mutex_unlock(s_peer.lock);
}

static uint8_t nfc_peer_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0U;

    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x80U) != 0U ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    return crc;
}

static int nfc_peer_read_chip_id(uint8_t id[SERVICE_V2_NFC_PEER_ID_LEN])
{
    volatile const uint32_t *id_word0 = (volatile const uint32_t *)0x48600208U;
    volatile const uint32_t *id_word1 = (volatile const uint32_t *)0x4860020CU;
    uint32_t words[2] = {*id_word0, *id_word1};

    if ((words[0] | words[1]) == 0U) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }
    memcpy(id, words, SERVICE_V2_NFC_PEER_ID_LEN);
    return SERVICE_V2_SENSORS_OK;
}

static void nfc_peer_format_id(const uint8_t id[SERVICE_V2_NFC_PEER_ID_LEN], char text[17])
{
    static const char hex[] = "0123456789abcdef";

    for (uint32_t i = 0U; i < SERVICE_V2_NFC_PEER_ID_LEN; ++i) {
        text[i * 2U] = hex[id[i] >> 4U];
        text[i * 2U + 1U] = hex[id[i] & 0x0FU];
    }
    text[16] = '\0';
}

static service_v2_nfc_peer_role_t nfc_peer_fixed_role_for_id(const uint8_t id[SERVICE_V2_NFC_PEER_ID_LEN])
{
    if (memcmp(id, s_reader_id, sizeof(s_reader_id)) == 0) {
        return SERVICE_V2_NFC_PEER_ROLE_READER;
    }
    for (size_t index = 0U; index < NFC_PEER_TARGET_ID_COUNT; ++index) {
        if (memcmp(id, s_target_ids[index], SERVICE_V2_NFC_PEER_ID_LEN) == 0) {
            return SERVICE_V2_NFC_PEER_ROLE_TARGET;
        }
    }
    return SERVICE_V2_NFC_PEER_ROLE_IDLE;
}

static int nfc_peer_show_fixed_role(void)
{
    /* WS2812 latches this one startup frame, so no refresh task competes with
     * the PN532 worker. RGB arguments are conventional even though the LED's
     * on-wire byte order is GRB. */
    if (s_peer.fixed_role == SERVICE_V2_NFC_PEER_ROLE_READER) {
        return service_v2_actuators_rgb_set(0U, NFC_PEER_ROLE_LED_LEVEL, 0U);
    }
    if (s_peer.fixed_role == SERVICE_V2_NFC_PEER_ROLE_TARGET) {
        return service_v2_actuators_rgb_set(NFC_PEER_ROLE_LED_LEVEL, 0U, NFC_PEER_ROLE_LED_LEVEL);
    }
    return SERVICE_V2_SENSORS_ERR_PROTOCOL;
}

static bool nfc_peer_expected_peer_id_matches(const uint8_t id[SERVICE_V2_NFC_PEER_ID_LEN])
{
    if (s_peer.fixed_role == SERVICE_V2_NFC_PEER_ROLE_TARGET) {
        return memcmp(id, s_reader_id, SERVICE_V2_NFC_PEER_ID_LEN) == 0;
    }
    if (s_peer.fixed_role != SERVICE_V2_NFC_PEER_ROLE_READER) {
        return false;
    }
    for (size_t index = 0U; index < NFC_PEER_TARGET_ID_COUNT; ++index) {
        if (memcmp(id, s_target_ids[index], SERVICE_V2_NFC_PEER_ID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

static void nfc_peer_build_identity_packet(void)
{
    memcpy(s_peer.identity_packet, s_peer_magic, sizeof(s_peer_magic));
    s_peer.identity_packet[4] = NFC_PEER_PROTOCOL_VERSION;
    memcpy(&s_peer.identity_packet[5], s_peer.status.local_id, SERVICE_V2_NFC_PEER_ID_LEN);
    s_peer.identity_packet[NFC_PEER_PAYLOAD_LEN - 1U] =
        nfc_peer_crc8(s_peer.identity_packet, NFC_PEER_PAYLOAD_LEN - 1U);
}

static bool nfc_peer_parse_identity_packet(const uint8_t *packet, size_t packet_length,
                                           uint8_t peer_id[SERVICE_V2_NFC_PEER_ID_LEN])
{
    if (!packet || !peer_id || packet_length != NFC_PEER_PAYLOAD_LEN) {
        return false;
    }
    if (memcmp(packet, s_peer_magic, sizeof(s_peer_magic)) != 0 || packet[4] != NFC_PEER_PROTOCOL_VERSION ||
        nfc_peer_crc8(packet, NFC_PEER_PAYLOAD_LEN - 1U) != packet[NFC_PEER_PAYLOAD_LEN - 1U]) {
        return false;
    }
    memcpy(peer_id, &packet[5], SERVICE_V2_NFC_PEER_ID_LEN);
    return true;
}

static bool nfc_peer_is_enabled(void)
{
    bool enabled;

    nfc_peer_lock();
    enabled = s_peer.status.enabled;
    nfc_peer_unlock();
    return enabled;
}

static void nfc_peer_set_role(service_v2_nfc_peer_role_t role)
{
    nfc_peer_lock();
    s_peer.status.role = role;
    nfc_peer_unlock();
}

/* Claiming a PN532 role and checking the enable flag must be one atomic
 * operation. Otherwise stop() can observe IDLE and return while the worker,
 * which previously saw enabled=true, starts a new reader/target transaction. */
static bool nfc_peer_claim_role(service_v2_nfc_peer_role_t role)
{
    bool claimed = false;

    nfc_peer_lock();
    if (s_peer.status.enabled && s_peer.status.role == SERVICE_V2_NFC_PEER_ROLE_IDLE) {
        s_peer.status.role = role;
        claimed = true;
    }
    nfc_peer_unlock();
    return claimed;
}

static void nfc_peer_set_error(int error)
{
    nfc_peer_lock();
    s_peer.status.last_error = error;
    nfc_peer_unlock();
}

static void nfc_peer_record_advertise_window(void)
{
    nfc_peer_lock();
    ++s_peer.status.advertise_window_count;
    s_peer.status.last_error = SERVICE_V2_SENSORS_OK;
    nfc_peer_unlock();
}

static void nfc_peer_record_seen(const uint8_t peer_id[SERVICE_V2_NFC_PEER_ID_LEN],
                                 const service_v2_pn532_target_t *target, uint32_t recognition_latency_ms)
{
    bool entered = false;
    uint32_t seen_count = 0U;
    char local_text[17];
    char peer_text[17];

    nfc_peer_lock();
    if (!s_peer.status.present) {
        ++s_peer.status.enter_count;
        entered = true;
    }
    s_peer.status.present = true;
    memcpy(s_peer.status.peer_id, peer_id, SERVICE_V2_NFC_PEER_ID_LEN);
    memset(s_peer.status.peer_uid, 0, sizeof(s_peer.status.peer_uid));
    if (target) {
        size_t uid_length = target->uid_len;
        if (uid_length > sizeof(s_peer.status.peer_uid)) {
            uid_length = sizeof(s_peer.status.peer_uid);
        }
        memcpy(s_peer.status.peer_uid, target->uid, uid_length);
    }
    ++s_peer.status.seen_count;
    seen_count = s_peer.status.seen_count;
    s_peer.status.last_error = SERVICE_V2_SENSORS_OK;
    s_peer.last_seen_tick = xTaskGetTickCount();
    nfc_peer_unlock();

    if (entered || s_peer.fixed_role == SERVICE_V2_NFC_PEER_ROLE_READER) {
        nfc_peer_format_id(s_peer.status.local_id, local_text);
        nfc_peer_format_id(peer_id, peer_text);
    }
    if (entered) {
        LISA_LOGI(TAG, "ENTER local=%s peer=%s transport=ISO14443-4A", local_text, peer_text);
    }
    /* Keep every successful Reader exchange observable.  The latency is
     * measured inside the PN532 transaction; repeating it while the antennas
     * remain coupled gives the host enough samples for useful statistics. */
    if (s_peer.fixed_role == SERVICE_V2_NFC_PEER_ROLE_READER && recognition_latency_ms != NFC_PEER_LATENCY_UNKNOWN) {
        LISA_LOGI(TAG,
                  entered ? "NFC_PEER_RECOGNIZED local=%s peer=%s count=%u "
                            "latency_ms=%u"
                          : "NFC_PEER_EXCHANGE local=%s peer=%s count=%u "
                            "latency_ms=%u",
                  local_text, peer_text, seen_count, recognition_latency_ms);
    }
}

static void nfc_peer_check_lost(void)
{
    uint8_t peer_id[SERVICE_V2_NFC_PEER_ID_LEN] = {0};
    bool left = false;
    TickType_t now = xTaskGetTickCount();

    nfc_peer_lock();
    if (s_peer.status.present && (now - s_peer.last_seen_tick) >= pdMS_TO_TICKS(NFC_PEER_LOST_TIMEOUT_MS)) {
        memcpy(peer_id, s_peer.status.peer_id, sizeof(peer_id));
        s_peer.status.present = false;
        memset(s_peer.status.peer_id, 0, sizeof(s_peer.status.peer_id));
        memset(s_peer.status.peer_uid, 0, sizeof(s_peer.status.peer_uid));
        ++s_peer.status.leave_count;
        left = true;
    }
    nfc_peer_unlock();

    if (left) {
        char peer_text[17];
        nfc_peer_format_id(peer_id, peer_text);
        LISA_LOGI(TAG, "LEAVE peer=%s reason=timeout", peer_text);
    }
}

static int nfc_peer_run_reader(bool *peer_found)
{
    uint8_t peer_packet[NFC_PEER_PAYLOAD_LEN];
    uint8_t peer_packet_length;
    service_v2_pn532_target_t target;
    uint32_t exchange_elapsed_ms = 0U;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t window_ticks = pdMS_TO_TICKS(NFC_PEER_SCAN_WINDOW_MS);

    *peer_found = false;
    do {
        int ret = service_v2_pn532_init();
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
        peer_packet_length = 0U;
        ret = service_v2_pn532_type_a_initiator_exchange_timed(
            s_peer.identity_packet, sizeof(s_peer.identity_packet), peer_packet, sizeof(peer_packet),
            &peer_packet_length, &target, NFC_PEER_SCAN_ATTEMPT_TIMEOUT_MS, &exchange_elapsed_ms);
        if (ret == SERVICE_V2_SENSORS_OK) {
            uint8_t peer_id[SERVICE_V2_NFC_PEER_ID_LEN];

            if (!nfc_peer_parse_identity_packet(peer_packet, peer_packet_length, peer_id) ||
                !nfc_peer_expected_peer_id_matches(peer_id)) {
                return SERVICE_V2_SENSORS_ERR_PROTOCOL;
            }
            nfc_peer_record_seen(peer_id, &target, exchange_elapsed_ms);
            *peer_found = true;
            return SERVICE_V2_SENSORS_OK;
        }
        if (ret == SERVICE_V2_SENSORS_ERR_PROTOCOL) {
            /* The PN532 response frame was already drained.  The sensor
             * layer keeps that session alive for this specific error, so a
             * short in-window retry avoids paying the multi-second SAM
             * re-synchronization cost for one bad RF/checksum frame. */
            lisa_thread_mdelay(NFC_PEER_SCAN_ATTEMPT_GAP_MS);
            continue;
        }
        if (!nfc_peer_expected_miss(ret)) {
            return ret;
        }
        lisa_thread_mdelay(NFC_PEER_SCAN_ATTEMPT_GAP_MS);
    } while ((xTaskGetTickCount() - start_tick) < window_ticks && nfc_peer_is_enabled());

    return SERVICE_V2_SENSORS_ERR_NO_TARGET;
}

static int nfc_peer_run_target(bool *peer_found)
{
    uint8_t peer_packet[NFC_PEER_PAYLOAD_LEN];
    uint8_t peer_packet_length = 0U;
    uint8_t nfcid1t[3];
    int ret = service_v2_pn532_init();

    *peer_found = false;
    if (ret != SERVICE_V2_SENSORS_OK) {
        return ret;
    }
    /* PN532 prepends the fixed NFCID1 byte 0x08 in PICC mode. Use the final
     * three chip-ID bytes for its configurable suffix; the full 64-bit ID is
     * validated separately by the versioned CRC-protected data packet. */
    memcpy(nfcid1t, &s_peer.status.local_id[SERVICE_V2_NFC_PEER_ID_LEN - sizeof(nfcid1t)], sizeof(nfcid1t));
    ret = service_v2_pn532_type_a_target_exchange(nfcid1t, NULL, 0U, s_peer.identity_packet,
                                                  sizeof(s_peer.identity_packet), peer_packet, sizeof(peer_packet),
                                                  &peer_packet_length, NFC_PEER_TARGET_WINDOW_MS);
    if (ret == SERVICE_V2_SENSORS_OK || ret == SERVICE_V2_SENSORS_ERR_NO_TARGET) {
        nfc_peer_record_advertise_window();
    }
    if (ret == SERVICE_V2_SENSORS_OK) {
        uint8_t peer_id[SERVICE_V2_NFC_PEER_ID_LEN];

        if (!nfc_peer_parse_identity_packet(peer_packet, peer_packet_length, peer_id) ||
            !nfc_peer_expected_peer_id_matches(peer_id)) {
            return SERVICE_V2_SENSORS_ERR_PROTOCOL;
        }
        nfc_peer_record_seen(peer_id, NULL, NFC_PEER_LATENCY_UNKNOWN);
        *peer_found = true;
    }
    return ret;
}

static bool nfc_peer_expected_miss(int error)
{
    return error == SERVICE_V2_SENSORS_ERR_NO_TARGET;
}

static void nfc_peer_task(void *argument)
{
    (void)argument;
    const service_v2_nfc_peer_role_t role = s_peer.fixed_role;

    lisa_thread_mdelay(NFC_PEER_BOOT_MIN_MS);

    for (;;) {
        if (!nfc_peer_is_enabled()) {
            nfc_peer_set_role(SERVICE_V2_NFC_PEER_ROLE_IDLE);
            lisa_thread_mdelay(100U);
            continue;
        }

        nfc_peer_check_lost();
        if (!nfc_peer_claim_role(role)) {
            lisa_thread_mdelay(NFC_PEER_LOOP_GAP_MS);
            continue;
        }

        bool success = false;
        int ret =
            role == SERVICE_V2_NFC_PEER_ROLE_READER ? nfc_peer_run_reader(&success) : nfc_peer_run_target(&success);
        nfc_peer_set_role(SERVICE_V2_NFC_PEER_ROLE_IDLE);

        if (ret == SERVICE_V2_SENSORS_OK || ret == SERVICE_V2_SENSORS_ERR_NO_TARGET) {
            nfc_peer_set_error(SERVICE_V2_SENSORS_OK);
        } else {
            nfc_peer_set_error(ret);
            LISA_LOGW(TAG, "role=%s failed: %d (%s)", service_v2_nfc_peer_role_name(role), ret,
                      service_v2_sensors_strerror(ret));
        }

        nfc_peer_check_lost();
        /* Fixed complementary roles let both sides reopen immediately. The
         * target remains armed for a long AutoColl window and conditional RDY
         * polling drains activation as soon as it arrives. This avoids the
         * former 300 ms abort/rearm churn and its RF discovery gaps. */
        lisa_thread_mdelay(ret == SERVICE_V2_SENSORS_OK || ret == SERVICE_V2_SENSORS_ERR_NO_TARGET
                               ? NFC_PEER_LOOP_GAP_MS
                               : NFC_PEER_ERROR_BACKOFF_MS);
    }
}

int service_v2_nfc_peer_init(void)
{
    if (s_peer.thread) {
        return SERVICE_V2_SENSORS_OK;
    }

    if (!s_peer.lock) {
        s_peer.lock = lisa_mutex_create();
        if (!s_peer.lock) {
            return SERVICE_V2_SENSORS_ERR_NO_MEMORY;
        }
    }

    int ret = nfc_peer_read_chip_id(s_peer.status.local_id);
    if (ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGE(TAG, "chip ID is unavailable: %d", ret);
        return ret;
    }

    s_peer.fixed_role = nfc_peer_fixed_role_for_id(s_peer.status.local_id);
    if (s_peer.fixed_role == SERVICE_V2_NFC_PEER_ROLE_IDLE) {
        char local_text[17];
        nfc_peer_format_id(s_peer.status.local_id, local_text);
        LISA_LOGE(TAG, "unsupported fixed-role chip ID: %s", local_text);
        return SERVICE_V2_SENSORS_ERR_PROTOCOL;
    }

    int led_ret = nfc_peer_show_fixed_role();
    if (led_ret != 0) {
        LISA_LOGW(TAG, "fixed-role LED update failed: %d", led_ret);
    }

    nfc_peer_build_identity_packet();
    /* The host timing tool owns startup order. Keeping discovery stopped at
     * boot prevents a Reader RF field from racing a Target that has not yet
     * entered TgInitAsTarget. */
    s_peer.status.enabled = false;
    s_peer.status.role = SERVICE_V2_NFC_PEER_ROLE_IDLE;
    s_peer.status.last_seen_age_ms = UINT32_MAX;

    lisa_thread_attr_t attributes = {
        .name = (uint8_t *)"v2_nfc_peer",
        .stack_size = 4096U,
        .priority = LISA_OS_PRIORITY_LOW,
    };
    s_peer.thread = lisa_thread_create(&attributes, nfc_peer_task, NULL);
    if (!s_peer.thread) {
        s_peer.status.enabled = false;
        return SERVICE_V2_SENSORS_ERR_NO_MEMORY;
    }

    char local_text[17];
    nfc_peer_format_id(s_peer.status.local_id, local_text);
    LISA_LOGI(TAG,
              "initialized local=%s fixed_role=%s transport=ISO14443-4A "
              "default=stopped lost=%ums",
              local_text, service_v2_nfc_peer_role_name(s_peer.fixed_role), NFC_PEER_LOST_TIMEOUT_MS);
    return SERVICE_V2_SENSORS_OK;
}

int service_v2_nfc_peer_start(void)
{
    if (!s_peer.thread || !s_peer.lock) {
        int ret = service_v2_nfc_peer_init();
        if (ret != SERVICE_V2_SENSORS_OK) {
            return ret;
        }
    }

    nfc_peer_lock();
    s_peer.status.enabled = true;
    s_peer.status.last_error = SERVICE_V2_SENSORS_OK;
    nfc_peer_unlock();
    return SERVICE_V2_SENSORS_OK;
}

int service_v2_nfc_peer_stop(void)
{
    if (!s_peer.thread || !s_peer.lock) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    nfc_peer_lock();
    s_peer.status.enabled = false;
    nfc_peer_unlock();

    bool idle = false;
    for (uint32_t elapsed_ms = 0U; elapsed_ms < NFC_PEER_STOP_WAIT_MS; elapsed_ms += 50U) {
        service_v2_nfc_peer_role_t role;
        nfc_peer_lock();
        role = s_peer.status.role;
        nfc_peer_unlock();
        if (role == SERVICE_V2_NFC_PEER_ROLE_IDLE) {
            idle = true;
            break;
        }
        lisa_thread_mdelay(50U);
    }
    if (!idle) {
        return SERVICE_V2_SENSORS_ERR_BUSY;
    }

    /* With the worker now quiescent, make sure the controller is synchronized
     * in normal SAM mode before allowing diagnostics to take ownership. */
    int quiesce_ret = service_v2_pn532_init();
    if (quiesce_ret != SERVICE_V2_SENSORS_OK) {
        LISA_LOGW(TAG, "PN532 quiesce failed after stop: %d (%s)", quiesce_ret,
                  service_v2_sensors_strerror(quiesce_ret));
    }

    uint8_t peer_id[SERVICE_V2_NFC_PEER_ID_LEN] = {0};
    bool left = false;
    nfc_peer_lock();
    if (s_peer.status.present) {
        memcpy(peer_id, s_peer.status.peer_id, sizeof(peer_id));
        s_peer.status.present = false;
        memset(s_peer.status.peer_id, 0, sizeof(s_peer.status.peer_id));
        memset(s_peer.status.peer_uid, 0, sizeof(s_peer.status.peer_uid));
        s_peer.last_seen_tick = 0U;
        ++s_peer.status.leave_count;
        left = true;
    }
    nfc_peer_unlock();

    if (left) {
        char peer_text[17];
        nfc_peer_format_id(peer_id, peer_text);
        LISA_LOGI(TAG, "LEAVE peer=%s reason=stopped", peer_text);
    }
    return quiesce_ret;
}

int service_v2_nfc_peer_run_once(service_v2_nfc_peer_role_t role)
{
    if (role != SERVICE_V2_NFC_PEER_ROLE_READER && role != SERVICE_V2_NFC_PEER_ROLE_TARGET) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }

    /* Build the local identity packet on first use. init() deliberately gives
     * the worker a boot delay, so stop() can quiesce it before any role is
     * claimed. */
    if (!s_peer.thread || !s_peer.lock) {
        int init_ret = service_v2_nfc_peer_init();
        if (init_ret != SERVICE_V2_SENSORS_OK) {
            return init_ret;
        }
        int stop_ret = service_v2_nfc_peer_stop();
        if (stop_ret != SERVICE_V2_SENSORS_OK) {
            return stop_ret;
        }
    }

    nfc_peer_lock();
    if (s_peer.status.enabled || s_peer.status.role != SERVICE_V2_NFC_PEER_ROLE_IDLE) {
        nfc_peer_unlock();
        return SERVICE_V2_SENSORS_ERR_BUSY;
    }
    s_peer.status.role = role;
    nfc_peer_unlock();

    bool success = false;
    int ret = role == SERVICE_V2_NFC_PEER_ROLE_READER ? nfc_peer_run_reader(&success) : nfc_peer_run_target(&success);
    nfc_peer_set_role(SERVICE_V2_NFC_PEER_ROLE_IDLE);
    nfc_peer_set_error(ret == SERVICE_V2_SENSORS_ERR_NO_TARGET ? SERVICE_V2_SENSORS_OK : ret);
    return ret;
}

int service_v2_nfc_peer_get_status(service_v2_nfc_peer_status_t *status)
{
    if (!status) {
        return SERVICE_V2_SENSORS_ERR_INVALID;
    }
    if (!s_peer.thread || !s_peer.lock) {
        return SERVICE_V2_SENSORS_ERR_NOT_READY;
    }

    nfc_peer_lock();
    *status = s_peer.status;
    if (s_peer.status.present) {
        status->last_seen_age_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_peer.last_seen_tick);
    } else {
        status->last_seen_age_ms = UINT32_MAX;
    }
    nfc_peer_unlock();
    return SERVICE_V2_SENSORS_OK;
}

const char *service_v2_nfc_peer_role_name(service_v2_nfc_peer_role_t role)
{
    switch (role) {
    case SERVICE_V2_NFC_PEER_ROLE_READER:
        return "reader";
    case SERVICE_V2_NFC_PEER_ROLE_TARGET:
        return "target";
    case SERVICE_V2_NFC_PEER_ROLE_IDLE:
    default:
        return "idle";
    }
}
