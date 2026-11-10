#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sys/time.h>

#define TAG "sys.net.probe"

#include "lisa_log.h"
#include "lisa_sntp.h"
#include "lisa_thread.h"
#include "lisa_semaphore.h"
#include "lisa_time.h"

#include "sys_network_diag.h"
#include "sys_network_manager.h"
#include "sys_network_health.h"
#include "sys_network_probe.h"
#include "sys_wifi.h"
#include "voice_msg.h"
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
#include "service_offline_action.h"
#endif

const char *sntp_servers[] = {
    "ntp.aliyun.com",
    "ntp.tencent.com",
    "ntp.ntsc.ac.cn",
};

static lisa_semaphore_t *network_probe_sem = NULL;
static volatile uint32_t s_probe_request;
#define NETWORK_HEALTH_INTERVAL_MS 30000U

bool network_probe_event_current(const void *data, uint32_t len, bool connected)
{
    sys_network_probe_event_t event = {0};
    bool has_owner = len == sizeof(event.owner) || len == sizeof(event);
    bool has_generation = len == sizeof(event);
    if ((data == NULL && len != 0U) || (data != NULL && !has_owner)) return false;
    if (has_owner) memcpy(&event, data, len);
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    if (has_owner && event.owner != service_offline_action_network_epoch()) return false;
#endif
    sys_network_status_t status = {0};
    if (sys_network_get_status(&status) != 0 || status.switching || status.connected != connected) return false;
    if (connected && !((status.active_bearer == SYS_NETWORK_BEARER_WIFI && status.wifi_connected) ||
                       (status.active_bearer == SYS_NETWORK_BEARER_MODEM && status.modem_connected))) return false;
    return !has_generation || sys_network_generation_is_current(event.generation);
}

static bool network_probe_owner_current(uint32_t request, uint32_t owner)
{
    if (request != s_probe_request) return false;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    return owner == service_offline_action_network_epoch();
#else
    (void)owner;
    return true;
#endif
}

static bool network_probe_notify_allowed(void)
{
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
    return service_offline_action_network_prompt_allowed();
#else
    return true;
#endif
}

static bool network_probe_link_ready(uint32_t radio_epoch)
{
    sys_network_status_t status = {0};
    if (sys_network_get_status(&status) != 0) {
        return false;
    }
    if (status.active_bearer == SYS_NETWORK_BEARER_WIFI) {
        return !sys_wifi_radio_suspended() && sys_wifi_radio_epoch() == radio_epoch &&
               status.wifi_connected;
    }
    return status.active_bearer == SYS_NETWORK_BEARER_MODEM && status.modem_connected;
}

static void network_probe_task(void *pvParameters)
{
    assert(network_probe_sem);
    while (1) {
        /* A hotspot can retain the station/IP while its cellular uplink dies.
         * Refresh reachability even without a Wi-Fi disconnect notification. */
        bool requested = lisa_semaphore_take(network_probe_sem, NETWORK_HEALTH_INTERVAL_MS) == 0;
        struct lisa_sntp_time time;
        uint32_t attempt = 0;
        uint32_t radio_epoch = sys_wifi_radio_epoch();
        uint32_t request = s_probe_request;
        uint32_t owner = 0;
#ifdef CONFIG_BOARD_ARCS_MINI_V2_1
        owner = service_offline_action_network_epoch();
#endif
        bool succeeded = false;
        bool time_valid = false;
        sys_network_status_t initial = {0};
        uint32_t generation = sys_network_generation();
        (void)sys_network_get_status(&initial);
        bool was_connected = initial.connected;
        bool failure_reported = false;

        while (network_probe_owner_current(request, owner) &&
               sys_network_generation_is_current(generation) && network_probe_link_ready(radio_epoch)) {
            uint32_t started_ms = (uint32_t)lisa_os_get_tick_ms();
            char detail[64];

            attempt++;
            snprintf(detail, sizeof(detail), "attempt=%u", (unsigned)attempt);
            sys_network_diag_mark("probe", "sntp_start", (int)attempt, detail);
            LOGI("sntp query start");
            int r = lisa_sntp_query(sntp_servers, sizeof(sntp_servers) / sizeof(sntp_servers[0]), 5 * 1000, &time);
            if (!network_probe_owner_current(request, owner) || !sys_network_generation_is_current(generation) ||
                !network_probe_link_ready(radio_epoch)) {
                break; /* Sleep/off-on invalidates a query from the old radio. */
            }
            if (r == 0) {
                snprintf(detail, sizeof(detail), "attempt=%u duration_ms=%u",
                         (unsigned)attempt,
                         (unsigned)((uint32_t)lisa_os_get_tick_ms() - started_ms));
                sys_network_diag_mark("probe", "sntp_success", 0, detail);
                succeeded = true;
                time_valid = true;
                break;
            }
            snprintf(detail, sizeof(detail), "attempt=%u duration_ms=%u",
                     (unsigned)attempt,
                     (unsigned)((uint32_t)lisa_os_get_tick_ms() - started_ms));
            sys_network_diag_mark("probe", "sntp_failed", r, detail);
            LOGE("sntp query failed, r:%d", r);
            /* A single lost UDP reply must not interrupt a healthy chat. */
            if (!was_connected || attempt >= 2U) {
                /* UDP/123 may be filtered while the business TCP endpoint
                 * remains reachable. Do not turn a healthy chat offline. */
                bool business_reachable = sys_network_business_reachable();
                if (!network_probe_owner_current(request, owner) || !sys_network_generation_is_current(generation) ||
                    !network_probe_link_ready(radio_epoch)) break;
                if (business_reachable) {
                    sys_network_diag_mark("probe", "business_tcp_success", 0, "sntp_unavailable");
                    succeeded = true;
                    break;
                }
                if (!failure_reported) {
                    if (!sys_network_report_probe_result_for_generation(false, generation)) break;
                    sys_network_probe_event_t event = {.owner = owner, .generation = generation};
                    if (network_probe_notify_allowed())
                        voice_msg_pub(VOICE_MSG_SYSTEM_NETWORK_PROBE_FAIL, &event, sizeof(event));
                    failure_reported = true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5 * 1000));
        }

        if (!succeeded || !network_probe_owner_current(request, owner) || !network_probe_link_ready(radio_epoch)) {
            continue;
        }
        if (!sys_network_report_probe_result_for_generation(true, generation)) continue;
        if (network_probe_notify_allowed() && (!was_connected || failure_reported)) {
            sys_network_probe_event_t event = {.owner = owner, .generation = generation};
            voice_msg_pub(VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS, &event, sizeof(event));
        }
        if (!time_valid || (!requested && was_connected && !failure_reported)) continue;
        struct timeval tv = {
            .tv_sec = time.sec,
            .tv_usec = time.nsec / 1000,
        };
        LOGI("time sync done, tv_sec:%ld, tv_usec:%ld", tv.tv_sec, tv.tv_usec);
        settimeofday(&tv, NULL);
    }
}

int network_probe_refresh(void)
{
    if (network_probe_sem == NULL) return -1;
    /* The one-slot semaphore coalesces errors. Do not advance s_probe_request:
     * repeated cloud errors must not starve the result of a running probe. */
    (void)lisa_semaphore_give(network_probe_sem);
    return 0;
}

int network_probe_init(void)
{
    network_probe_sem = lisa_semaphore_create(1);
    assert(network_probe_sem);

    lisa_thread_attr_t att = {
        .name = "sys.net.probe",
        .stack_size = 8192,
        .priority = LISA_OS_PRIORITY_NORMAL,
    };

    lisa_thread_t *thread = lisa_thread_create(&att, network_probe_task, NULL);
    assert(thread);

    return 0;
}

int network_probe_start(void)
{
    assert(network_probe_sem);
    ++s_probe_request;
    sys_network_diag_mark("probe", "scheduled", 0, NULL);
    lisa_semaphore_give(network_probe_sem);

    return 0;
}
