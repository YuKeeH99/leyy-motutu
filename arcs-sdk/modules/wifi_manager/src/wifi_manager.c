/**
 * Copyright (c) 2025, LISTENAI
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include "wifi_manager/wifi_manager.h"
#include "wifi_manager/wifi_manager_wifi_ops.h"
#include "wifi_manager/dlist.h"
#include "wifi_manager/wifi_manager_storage.h"
#include "wifi_manager/priv/wifi_manager_autoconn_internal.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>


#define TAG "wifi_mgr"
#include "lisa_log.h"
#include "lisa_time.h"
#include "lisa_thread.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define _WAIT_FOREVER  LISA_WAIT_FOREVER
#define WIFI_SCAN_MAX_NUMBER  (32)

#ifndef LISA_WAIT_FOREVER
#define LISA_WAIT_FOREVER (0xFFFFFFFFU)
#endif

#ifndef LISA_NO_WAIT
#define LISA_NO_WAIT (0U)
#endif

#ifndef LISA_OK
#define LISA_OK (0)
#endif

#ifndef CONFIG_WIFI_MGR_CONNECT_TIMEOUT
#define CONFIG_WIFI_MGR_CONNECT_TIMEOUT (15000)
#endif

#define WIFI_MGR_MALLOC(size) (s_wifi_mgr_obj->mem_ops.malloc(size))
#define WIFI_MGR_CALLOC(nmemb, size) (s_wifi_mgr_obj->mem_ops.calloc(nmemb, size))
#define WIFI_MGR_FREE(ptr) (s_wifi_mgr_obj->mem_ops.free(ptr))

#define WIFI_MGR_CHECK(cond, ret_val) ({                                      \
    if (!(cond)) {                                                            \
        LISA_LOGE(TAG, "Check failed at %s(%d)", __FUNCTION__, __LINE__);            \
        return (ret_val);                                                     \
    }                                                                         \
})

#define WIFI_MGR_FREE_AND_NULL(ptr) ({                                        \
    WIFI_MGR_FREE(ptr);                                                     \
    (ptr) = NULL;                                                             \
})



#define WIFI_LIST_ID                            1
#define WIFI_INFO_COUNT_ID                      2
#define MAX_WIFI_AUTO_CONN_RETRY                1
#define MAX_AP_FAILURE_COUNT                    3
#define WIFI_MGR_AUTOCONN_DEFAULT_INTERVAL_MS   1000U
#define WIFI_MGR_AUTOCONN_WARMUP_CYCLES         3U
#define WIFI_MGR_AUTOCONN_CONNECT_FAIL_MAX_MUL  8U
#define WIFI_MGR_AUTOCONN_NOT_FOUND_MAX_MUL     32U

#define WIFI_MGR_QUEUE_COUNT                    10
#define WIFI_MGR_QUEUE_NAME                     "wifi_mgr_queue"

/* Short CP task critical sections close intake independently of the KV mutex. */
#define WIFI_MGR_RADIO_PAUSED        (1UL << 31)
#define WIFI_MGR_RADIO_ASYNC_SCAN    (1UL << 30)
#define WIFI_MGR_RADIO_ASYNC_CONNECT (1UL << 29)

#define WIFI_MGR_THREAD_STACK_SIZE               CONFIG_WIFI_MGR_THREAD_STACK
#define WIFI_MGR_THREAD_PRIORITY                 CONFIG_WIFI_MGR_THREAD_PRIORITY

#define WIFI_MGR_CONNECTION_EVENT               (WIFI_MGR_WIFI_EVT_STA_CONNECTED | WIFI_MGR_WIFI_EVT_STA_DISCONNECTED | WIFI_MGR_WIFI_EVT_STA_CONNECTING | WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED | WIFI_MGR_WIFI_EVT_SCAN_FAILED)

typedef struct {
    wifi_mgr_sta_config_t config;
    wifi_mgr_connection_status_t sta_status;
} wifi_mgr_device_t;

typedef struct wifi_mgr_event_cb {
    sys_dnode_t node;
    union {
        wifi_mgr_connection_cb_t connection_handler;
        wifi_mgr_scan_done_cb_t scandone_handler;
        void *handler;
    };
    uint32_t event;
    void *arg;
} wifi_mgr_event_cb_t;

typedef struct {
    sys_dnode_t node;
    char ssid[32];              // SSID
    char bssid[18];             // BSSID (XX:XX:XX:XX:XX:XX格式)
    uint32_t failure_count;     // 连续失败次数
    uint32_t last_failure_ms;
    bool explicit_block;        // Explicit disconnect stays blocked until manual connect.
} wifi_mgr_ap_failure_record_t;

#define AP_FAILURE_COOLDOWN_MS 60000U

typedef enum {
    WIFI_MGR_AUTOCONN_MANUAL_IDLE = 0,
    WIFI_MGR_AUTOCONN_MANUAL_PAUSED_SYNC,
    WIFI_MGR_AUTOCONN_MANUAL_RESUME_ON_EVENT,
} wifi_mgr_manual_autoconn_state_t;

typedef struct {
    wifi_mgr_autoconn_config_t config;
    bool enable;
    uint32_t generation;
    uint32_t evidence_epoch;
    uint32_t base_interval_ms;
    uint32_t current_interval_ms;
    uint32_t no_candidate_streak;
    sys_dlist_t failure_records_list;  // 失败记录链表
} wifi_mgr_auto_conn_obj_t;

typedef struct {
    wifi_mgr_device_t sta_device;
    wifi_mgr_wifi_event_cb_t wifi_event_cb;
    sys_dlist_t wifi_callback_list;
    wifi_mgr_auto_conn_obj_t *auto_connect_obj;
    int last_connect_fail_error;
    bool suppress_next_pmk_connect_fail_callback;
    wifi_mgr_manual_autoconn_state_t manual_autoconn_state;
    uint32_t manual_autoconn_generation;
    uint32_t manual_autoconn_epoch;
    uint32_t radio_generation;
    uint32_t radio_state;
    uint32_t auto_evidence_epoch;
    uint32_t connect_terminal_sequence;
    bool auto_exhausted;
    void* mutex;
    void* queue;
    bool thread_exit;
    void *thread;
    wifi_manager_mem_ops_t mem_ops;
    wifi_manager_os_ops_t os_ops;
    wifi_manager_wifi_ops_t wifi_ops;
    wifi_storage_ctx_t storage_ctx;
} wifi_mgr_obj_t;

static wifi_mgr_obj_t *s_wifi_mgr_obj = NULL;

static uint32_t wifi_mgr_auto_evidence_reset(bool cancel)
{
    taskENTER_CRITICAL();
    if (cancel) s_wifi_mgr_obj->auto_evidence_epoch++;
    s_wifi_mgr_obj->auto_exhausted = false;
    uint32_t epoch = s_wifi_mgr_obj->auto_evidence_epoch;
    taskEXIT_CRITICAL();
    return epoch;
}

static int wifi_mgr_auto_connect_stop_epoch(uint32_t *epoch);

bool wifi_mgr_auto_connect_exhausted(void)
{
    if (!s_wifi_mgr_obj) return false;
    taskENTER_CRITICAL();
    bool exhausted = s_wifi_mgr_obj->auto_exhausted &&
        !(s_wifi_mgr_obj->radio_state & WIFI_MGR_RADIO_PAUSED);
    taskEXIT_CRITICAL();
    return exhausted;
}

#define WIFI_MGR_MUTEX_LOCK() do {                      \
    s_wifi_mgr_obj->os_ops.mutex_lock(s_wifi_mgr_obj->mutex, _WAIT_FOREVER);    \
} while (0)

#define WIFI_MGR_MUTEX_UNLOCK() do {                    \
    s_wifi_mgr_obj->os_ops.mutex_unlock(s_wifi_mgr_obj->mutex);             \
} while (0)


static int wifi_auto_connect_stop(wifi_mgr_auto_conn_obj_t *auto_connect_obj);
static void wifi_mgr_thread(void *arg);
static void wifi_mgr_fill_connection_info(wifi_mgr_connection_info_t *connection_info,
                                          wifi_mgr_sta_config_t *sta_copy,
                                          void *event_data,
                                          uint32_t data_len,
                                          wifi_mgr_connection_status_t status);

/* The counter covers the complete caller, including HAL callbacks and any
 * remaining auto-cycle work. Async submissions additionally await an event. */
static uint32_t wifi_mgr_radio_read(const uint32_t *value)
{
    taskENTER_CRITICAL();
    uint32_t result = *value;
    taskEXIT_CRITICAL();
    return result;
}

static void wifi_mgr_radio_clear(uint32_t bits)
{
    taskENTER_CRITICAL();
    s_wifi_mgr_obj->radio_state &= ~bits;
    taskEXIT_CRITICAL();
}

static int wifi_mgr_radio_begin(bool asynchronous, bool scan, uint32_t *generation)
{
    int ret = 0;
    taskENTER_CRITICAL();
    uint32_t state = s_wifi_mgr_obj->radio_state;
    if (state != 0U) {
        ret = -EBUSY;
    } else {
        s_wifi_mgr_obj->radio_state++;
        if (asynchronous) {
            s_wifi_mgr_obj->radio_state |= scan ? WIFI_MGR_RADIO_ASYNC_SCAN : WIFI_MGR_RADIO_ASYNC_CONNECT;
        }
        if (generation) {
            *generation = s_wifi_mgr_obj->radio_generation;
        }
    }
    taskEXIT_CRITICAL();
    return ret;
}

static void wifi_mgr_radio_end(bool asynchronous, bool scan, bool failed)
{
    taskENTER_CRITICAL();
    s_wifi_mgr_obj->radio_state--;
    if (asynchronous && failed) {
        s_wifi_mgr_obj->radio_state &= ~(scan ? WIFI_MGR_RADIO_ASYNC_SCAN : WIFI_MGR_RADIO_ASYNC_CONNECT);
    }
    taskEXIT_CRITICAL();
}

static bool wifi_mgr_radio_cancelled(uint32_t generation)
{
    return wifi_mgr_radio_is_paused() ||
        generation != wifi_mgr_radio_read(&s_wifi_mgr_obj->radio_generation);
}

static int wifi_mgr_radio_connect(wifi_mgr_sta_config_t *config, uint32_t wait_ms)
{
    taskENTER_CRITICAL();
    s_wifi_mgr_obj->radio_state |= WIFI_MGR_RADIO_ASYNC_CONNECT;
    taskEXIT_CRITICAL();
    int ret = s_wifi_mgr_obj->wifi_ops.sta_connect(config, wait_ms);
    /* ARCS timed waits do not cancel association. Retain its pending marker
     * until the terminal event; a successful synchronous return is complete. */
    if (wait_ms != LISA_NO_WAIT && ret != -ETIMEDOUT) {
        wifi_mgr_radio_clear(WIFI_MGR_RADIO_ASYNC_CONNECT);
    }
    return ret;
}

int wifi_mgr_radio_pause(uint32_t timeout_ms)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    wifi_mgr_auto_evidence_reset(true);
    uint64_t start_ms = lisa_os_get_tick_ms();
    taskENTER_CRITICAL();
    if (!(s_wifi_mgr_obj->radio_state & WIFI_MGR_RADIO_PAUSED)) {
        s_wifi_mgr_obj->radio_generation++;
    }
    s_wifi_mgr_obj->radio_state |= WIFI_MGR_RADIO_PAUSED;
    taskEXIT_CRITICAL();

    for (;;) {
        uint32_t state = wifi_mgr_radio_read(&s_wifi_mgr_obj->radio_state);
        if (!(state & ~WIFI_MGR_RADIO_PAUSED)) return 0;
        uint64_t elapsed_ms = lisa_os_get_tick_ms() - start_ms;
        if (elapsed_ms >= timeout_ms) return -ETIMEDOUT;
        uint32_t remaining_ms = timeout_ms - (uint32_t)elapsed_ms;
        lisa_thread_mdelay(remaining_ms < 10U ? remaining_ms : 10U);
    }
}

int wifi_mgr_radio_resume(void)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    wifi_mgr_radio_clear(WIFI_MGR_RADIO_PAUSED);
    return 0;
}

bool wifi_mgr_radio_is_paused(void)
{
    if (s_wifi_mgr_obj == NULL) return false;
    return (wifi_mgr_radio_read(&s_wifi_mgr_obj->radio_state) & WIFI_MGR_RADIO_PAUSED) != 0U;
}

/**
 * @brief Validate BSSID format (XX:XX:XX:XX:XX:XX)
 * @param bssid BSSID string to validate
 * @return true if valid, false otherwise
 */
static bool is_valid_bssid_format(const char *bssid)
{
    if (!bssid) {
        return false;
    }
    
    // Empty BSSID is valid (means any BSSID)
    if (bssid[0] == '\0') {
        return true;
    }
    
    // Check length (17 chars for XX:XX:XX:XX:XX:XX)
    size_t len = strlen(bssid);
    if (len != 17) {
        return false;
    }
    
    // Check format: XX:XX:XX:XX:XX:XX
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            // Should be ':'
            if (bssid[i] != ':') {
                return false;
            }
        } else {
            // Should be hex digit
            char c = bssid[i];
            if (!((c >= '0' && c <= '9') || 
                  (c >= 'a' && c <= 'f') || 
                  (c >= 'A' && c <= 'F'))) {
                return false;
            }
        }
    }
    
    return true;
}

uint32_t wifi_mgr_autoconn_next_interval_ms(uint32_t base_interval_ms,
                                            uint32_t current_interval_ms,
                                            uint32_t no_candidate_streak,
                                            wifi_mgr_autoconn_cycle_result_t result,
                                            uint32_t max_interval_ms)
{
    uint32_t base = (base_interval_ms == 0U) ? WIFI_MGR_AUTOCONN_DEFAULT_INTERVAL_MS : base_interval_ms;
    uint32_t current = (current_interval_ms == 0U) ? base : current_interval_ms;

    if (result == WIFI_MGR_AUTOCONN_CYCLE_SUCCESS) {
        return base;
    }

    if (result == WIFI_MGR_AUTOCONN_CYCLE_NO_CANDIDATE &&
        no_candidate_streak <= WIFI_MGR_AUTOCONN_WARMUP_CYCLES) {
        return base;
    }

    uint32_t next = current;
    uint32_t max_mul = WIFI_MGR_AUTOCONN_CONNECT_FAIL_MAX_MUL;
    if (result == WIFI_MGR_AUTOCONN_CYCLE_NO_CANDIDATE) {
        max_mul = WIFI_MGR_AUTOCONN_NOT_FOUND_MAX_MUL;
    }

    if (next <= (UINT32_MAX / 2U)) {
        next *= 2U;
    }

    uint32_t max_interval = (base > (UINT32_MAX / max_mul)) ? UINT32_MAX : (base * max_mul);
    if (max_interval_ms != 0U) {
        max_interval = (max_interval_ms < base) ? base : max_interval_ms;
    }

    if (next > max_interval) {
        next = max_interval;
    }
    return next;
}

static bool is_ap_blacklisted(wifi_mgr_auto_conn_obj_t *auto_connect_obj, const char *ssid, const char *bssid)
{
    if (!auto_connect_obj || !ssid || !bssid) {
        return false;
    }
    
    WIFI_MGR_MUTEX_LOCK();
    wifi_mgr_ap_failure_record_t *entry;
    SYS_DLIST_FOR_EACH_CONTAINER(&auto_connect_obj->failure_records_list, entry, node) {
        if (strcmp(entry->ssid, ssid) == 0 &&
            (entry->bssid[0] == '\0' || strcmp(entry->bssid, bssid) == 0)) {
            if (entry->failure_count >= MAX_AP_FAILURE_COUNT) {
                if (entry->explicit_block ||
                    (uint32_t)((uint32_t)lisa_os_get_tick_ms() - entry->last_failure_ms) < AP_FAILURE_COOLDOWN_MS) {
                    WIFI_MGR_MUTEX_UNLOCK();
                    return true;
                }
                /* RF/auth failures are transient; never ban a saved hotspot
                 * for the rest of this boot after three failed attempts. */
                entry->failure_count = 0;
            }
        }
    }
    WIFI_MGR_MUTEX_UNLOCK();
    return false;
}

static void increment_ap_failure_count(wifi_mgr_auto_conn_obj_t *auto_connect_obj, const char *ssid, const char *bssid)
{
    if (!auto_connect_obj || !ssid || !bssid) {
        return;
    }
    
    // 查找现有记录
    WIFI_MGR_MUTEX_LOCK();
    wifi_mgr_ap_failure_record_t *entry;
    SYS_DLIST_FOR_EACH_CONTAINER(&auto_connect_obj->failure_records_list, entry, node) {
        if (strcmp(entry->ssid, ssid) == 0 && strcmp(entry->bssid, bssid) == 0) {
            entry->failure_count++;
            entry->last_failure_ms = (uint32_t)lisa_os_get_tick_ms();
            LISA_LOGW(TAG, "AP %s (%s) failure count: %u", ssid, bssid, entry->failure_count);
            if (entry->failure_count >= MAX_AP_FAILURE_COUNT) {
                LISA_LOGI(TAG, "AP %s (%s) blacklisted after %u failures", ssid, bssid, entry->failure_count);
            }
            WIFI_MGR_MUTEX_UNLOCK();
            return;
        }
    }
    
    // 添加新记录
    wifi_mgr_ap_failure_record_t *new_record = WIFI_MGR_CALLOC(1, sizeof(wifi_mgr_ap_failure_record_t));
    if (!new_record) {
        LISA_LOGE(TAG, "Failed to allocate memory for failure record");
        WIFI_MGR_MUTEX_UNLOCK();
        return;
    }
    
    strncpy(new_record->ssid, ssid, sizeof(new_record->ssid) - 1);
    new_record->ssid[sizeof(new_record->ssid) - 1] = '\0';
    strncpy(new_record->bssid, bssid, sizeof(new_record->bssid) - 1);
    new_record->bssid[sizeof(new_record->bssid) - 1] = '\0';
    new_record->failure_count = 1;
    new_record->last_failure_ms = (uint32_t)lisa_os_get_tick_ms();
    
    sys_dlist_prepend(&auto_connect_obj->failure_records_list, &new_record->node);
    
    LISA_LOGI(TAG, "Added failure record for AP %s (%s), count: 1", ssid, bssid);
    WIFI_MGR_MUTEX_UNLOCK();
}

static void set_ap_failure_count_to_max(wifi_mgr_auto_conn_obj_t *auto_connect_obj, const char *ssid, const char *bssid)
{
    if (!auto_connect_obj || !ssid) {
        return;
    }

    WIFI_MGR_MUTEX_LOCK();
    wifi_mgr_ap_failure_record_t *entry;
    SYS_DLIST_FOR_EACH_CONTAINER(&auto_connect_obj->failure_records_list, entry, node) {
        if (strcmp(entry->ssid, ssid) == 0) {
            entry->failure_count = MAX_AP_FAILURE_COUNT;
            entry->explicit_block = true;
            entry->bssid[0] = '\0';
            WIFI_MGR_MUTEX_UNLOCK();
            return;
        }
    }

    wifi_mgr_ap_failure_record_t *new_record = WIFI_MGR_CALLOC(1, sizeof(wifi_mgr_ap_failure_record_t));
    if (!new_record) {
        LISA_LOGE(TAG, "Failed to allocate memory for blacklist record");
        WIFI_MGR_MUTEX_UNLOCK();
        return;
    }
    strncpy(new_record->ssid, ssid, sizeof(new_record->ssid) - 1);
    new_record->ssid[sizeof(new_record->ssid) - 1] = '\0';
    (void)bssid;
    new_record->bssid[0] = '\0';
    new_record->failure_count = MAX_AP_FAILURE_COUNT;
    new_record->explicit_block = true;
    sys_dlist_prepend(&auto_connect_obj->failure_records_list, &new_record->node);
    WIFI_MGR_MUTEX_UNLOCK();
}

static void reset_ap_failure_count(wifi_mgr_auto_conn_obj_t *auto_connect_obj, const char *ssid, const char *bssid)
{
    if (!auto_connect_obj || !ssid || !bssid) {
        return;
    }
    
    WIFI_MGR_MUTEX_LOCK();
    wifi_mgr_ap_failure_record_t *entry;
    SYS_DLIST_FOR_EACH_CONTAINER(&auto_connect_obj->failure_records_list, entry, node) {
        if (strcmp(entry->ssid, ssid) == 0 &&
            (entry->bssid[0] == '\0' || strcmp(entry->bssid, bssid) == 0)) {
            entry->failure_count = 0;
            entry->explicit_block = false;
            LISA_LOGI(TAG, "Reset failure count for AP %s (%s)", ssid, bssid);
            WIFI_MGR_MUTEX_UNLOCK();
            return;
        }
    }
    WIFI_MGR_MUTEX_UNLOCK();
}



typedef enum{
    WIFI_MGR_QUEUE_MSG_EVENT_WIFI_AUTO_CONNECT_START,
    WIFI_MGR_QUEUE_MSG_EVENT_WIFI_AUTO_CONNECT_STOP,

}wifi_mgr_queue_message_event_e;

typedef struct{
    wifi_mgr_queue_message_event_e event;
    void* payload;
}wifi_mgr_queue_message_t;

typedef struct {
    wifi_mgr_autoconn_config_t config;
    uint32_t generation;
    uint32_t evidence_epoch;
} wifi_mgr_autoconn_request_t;

static int wifi_mgr_queue_auto_start(const wifi_mgr_autoconn_config_t *config,
                                     uint32_t generation, uint32_t epoch)
{
    if (wifi_mgr_radio_cancelled(generation) ||
        epoch != wifi_mgr_radio_read(&s_wifi_mgr_obj->auto_evidence_epoch)) return -EBUSY;
    wifi_mgr_autoconn_request_t *request = WIFI_MGR_MALLOC(sizeof(*request));
    if (!request) return -ENOMEM;
    request->config = *config;
    request->generation = generation;
    request->evidence_epoch = epoch;
    wifi_mgr_queue_message_t msg = {
        .event = WIFI_MGR_QUEUE_MSG_EVENT_WIFI_AUTO_CONNECT_START,
        .payload = request,
    };
    /* This path is also called from HAL callbacks; a full queue must not wait
     * for the worker that can itself be waiting for that callback. */
    int ret = s_wifi_mgr_obj->os_ops.queue_push(s_wifi_mgr_obj->queue, &msg,
                                              sizeof(msg), LISA_NO_WAIT);
    if (ret != 0) WIFI_MGR_FREE(request);
    return ret;
}
static void start_auto_connect_timer(wifi_mgr_auto_conn_obj_t *auto_connect_obj)
{
    if (auto_connect_obj != NULL && auto_connect_obj->enable == true) {
        //TODO
    }
}

static void wifi_mgr_dispatch_user_callbacks(wifi_mgr_wifi_event_t event, void *event_data, uint32_t data_len)
{
    wifi_mgr_event_cb_t *entry, *next;
    SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&s_wifi_mgr_obj->wifi_callback_list, entry, next, node) {
        if (!(entry->event & event)) {
            continue;
        }
        if (event & WIFI_MGR_WIFI_EVT_STA_CONNECTED) {

            wifi_mgr_connection_info_t connection_info = {0};
            wifi_mgr_sta_config_t sta_copy;
            wifi_mgr_fill_connection_info(&connection_info,
                                          &sta_copy,
                                          event_data,
                                          data_len,
                                          WIFI_MGR_STA_CONNECTED);
            entry->connection_handler(&connection_info, entry->arg);

        } else if (event & WIFI_MGR_WIFI_EVT_STA_CONNECTING) {
            wifi_mgr_connection_info_t connection_info = {0};
            wifi_mgr_sta_config_t sta_copy;
            wifi_mgr_fill_connection_info(&connection_info,
                                          &sta_copy,
                                          event_data,
                                          data_len,
                                          WIFI_MGR_STA_CONNECTING);
            entry->connection_handler(&connection_info, entry->arg);

        } else if (event & WIFI_MGR_WIFI_EVT_STA_DISCONNECTED) {
            // 只有在之前已连接的情况下才触发断开回调
            if (s_wifi_mgr_obj->sta_device.sta_status == WIFI_MGR_STA_CONNECTED) {
                s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_DISCONNECTED;
                wifi_mgr_connection_info_t connection_info = {0};
                wifi_mgr_sta_config_t sta_copy = {0};
                wifi_mgr_connect_fail_info_t fail_info = {
                    .error_code = -1,
                    .status_code = -1,
                    .reason_code = -1,
                };
                if (event_data != NULL && data_len >= sizeof(wifi_mgr_disconnect_event_info_t)) {
                    wifi_mgr_disconnect_event_info_t *disc_info = event_data;
                    wifi_mgr_fill_connection_info(&connection_info,
                                                  &sta_copy,
                                                  &disc_info->sta_config,
                                                  sizeof(disc_info->sta_config),
                                                  WIFI_MGR_STA_DISCONNECTED);
                    memcpy(&fail_info, &disc_info->fail_info, sizeof(fail_info));
                } else if (event_data != NULL && data_len >= sizeof(wifi_mgr_connect_fail_info_t)) {
                    memcpy(&fail_info, event_data, sizeof(fail_info));
                } else {
                    wifi_mgr_fill_connection_info(&connection_info,
                                                  &sta_copy,
                                                  event_data,
                                                  data_len,
                                                  WIFI_MGR_STA_DISCONNECTED);
                }

                if (connection_info.sta_info == NULL) {
                    memcpy(&sta_copy, &s_wifi_mgr_obj->sta_device.config, sizeof(sta_copy));
                    connection_info.sta_info = &sta_copy;
                }
                connection_info.error_code = fail_info.error_code;
                connection_info.status_code = fail_info.status_code;
                connection_info.reason = fail_info.reason_code;
                entry->connection_handler(&connection_info, entry->arg);
            } else {
                LISA_LOGD(TAG, "Received disconnect event but device was not connected, ignoring user callback");
            }

        } else if (event & WIFI_MGR_WIFI_EVT_SCAN_DONE) {

            if (event_data != NULL && data_len >= sizeof(wifi_mgr_scan_ap_list_t)) {
                wifi_mgr_scan_ap_list_t *aps_list = event_data;
                entry->scandone_handler(aps_list->ap_info, aps_list->count, entry->arg);
            } else {
                LISA_LOGW(TAG, "Scan done event missing scan list payload");
            }
        } else if (event & WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED) {
            wifi_mgr_connect_fail_info_t fail_info = {
                .error_code = -1,
                .status_code = -1,
                .reason_code = -1,
            };
            if (event_data != NULL) {
                if (data_len >= sizeof(fail_info)) {
                    memcpy(&fail_info, event_data, sizeof(fail_info));
                } else if (data_len >= sizeof(int)) {
                    fail_info.error_code = *((int *)event_data);
                    fail_info.reason_code = fail_info.error_code;
                }
            }
            LISA_LOGE(TAG,
                      "WIFI_DEV_EVT_STA_CONNECTION_FAILED, error:%d status:%d reason:%d",
                      fail_info.error_code, fail_info.status_code, fail_info.reason_code);
            wifi_mgr_connection_info_t connection_info = {0};
            connection_info.status = WIFI_MGR_STA_CONNECT_FAILED;
            connection_info.error_code = fail_info.error_code;
            connection_info.status_code = fail_info.status_code;
            connection_info.reason = fail_info.reason_code;
            entry->connection_handler(&connection_info, entry->arg);
        } else if (event & WIFI_MGR_WIFI_EVT_SCAN_FAILED) {
            int reason = -1;
            if (event_data != NULL && data_len >= sizeof(int)) {
                reason = *((int *)(event_data));    //parse reason code from event_data.
            }
            wifi_mgr_scan_info_t *aps_info = NULL;
            int ap_num = (reason <= 0) ? reason : -reason;
            entry->scandone_handler(aps_info, ap_num, entry->arg);
        }
    }
}

static void wifi_mgr_fill_connection_info(wifi_mgr_connection_info_t *connection_info,
                                          wifi_mgr_sta_config_t *sta_copy,
                                          void *event_data,
                                          uint32_t data_len,
                                          wifi_mgr_connection_status_t status)
{
    if (event_data != NULL && data_len >= sizeof(wifi_mgr_sta_config_t)) {
        memcpy(sta_copy, event_data, sizeof(*sta_copy));
        connection_info->sta_info = sta_copy;
    } else {
        connection_info->sta_info = event_data;
    }
    connection_info->status = status;
}

static void wifi_event_handler(wifi_mgr_wifi_event_t events, void *event_data, uint32_t data_len, void *arg)
{
    if (s_wifi_mgr_obj == NULL) {
        LISA_LOGW(TAG, "wifi_event ignored after deinit");
        return;
    }
    /* Independent terminal callbacks can outlive the HAL submission. Count
     * their whole lifetime too, including application netif/DHCP work. */
    taskENTER_CRITICAL();
    s_wifi_mgr_obj->radio_state++;
    if (events & (WIFI_MGR_WIFI_EVT_STA_CONNECTED | WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED |
                  WIFI_MGR_WIFI_EVT_STA_DISCONNECTED))
        s_wifi_mgr_obj->connect_terminal_sequence++;
    taskEXIT_CRITICAL();
    LISA_LOGI(TAG, "%s: wifi_event: 0x%02X,event_data:%p ", __FUNCTION__, events,event_data);
    if (events & WIFI_MGR_WIFI_EVT_STA_CONNECTED) {
        wifi_mgr_auto_evidence_reset(false);
        s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_CONNECTED;
        const char *ssid_for_reset = NULL;
        const char *bssid_for_reset = NULL;
        if (event_data != NULL && data_len >= sizeof(wifi_mgr_sta_config_t)) {
            wifi_mgr_sta_config_t *sta_config = event_data;
            memcpy(&s_wifi_mgr_obj->sta_device.config, sta_config, sizeof(wifi_mgr_sta_config_t));
            ssid_for_reset = sta_config->ssid;
            bssid_for_reset = sta_config->bssid;
        } else {
            LISA_LOGW(TAG, "STA connected event missing config payload");
            ssid_for_reset = s_wifi_mgr_obj->sta_device.config.ssid;
            bssid_for_reset = s_wifi_mgr_obj->sta_device.config.bssid;
        }
        if (s_wifi_mgr_obj->auto_connect_obj != NULL) {
            reset_ap_failure_count(s_wifi_mgr_obj->auto_connect_obj, ssid_for_reset, bssid_for_reset);
        }
        s_wifi_mgr_obj->last_connect_fail_error = 0;
        if (s_wifi_mgr_obj->auto_connect_obj != NULL &&
            s_wifi_mgr_obj->auto_connect_obj->enable) {  /* stop auto connection timer and delayable work */
            wifi_auto_connect_stop(s_wifi_mgr_obj->auto_connect_obj);
        }
    }
    if (events & WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED) {
        wifi_mgr_connect_fail_info_t fail_info = {
            .error_code = -1,
            .status_code = -1,
            .reason_code = -1,
        };
        if (event_data != NULL) {
            if (data_len >= sizeof(fail_info)) {
                memcpy(&fail_info, event_data, sizeof(fail_info));
            } else if (data_len >= sizeof(int)) {
                fail_info.error_code = *((int *)event_data);
                fail_info.reason_code = fail_info.error_code;
            }
        }
        s_wifi_mgr_obj->last_connect_fail_error = fail_info.error_code;
    }

    bool resume_auto = false;
    uint32_t resume_generation = 0;
    uint32_t resume_epoch = 0;
    wifi_mgr_autoconn_config_t resume_config = {0};
    WIFI_MGR_MUTEX_LOCK();
    if (events & (WIFI_MGR_WIFI_EVT_SCAN_DONE | WIFI_MGR_WIFI_EVT_SCAN_FAILED)) {
        wifi_mgr_radio_clear(WIFI_MGR_RADIO_ASYNC_SCAN);
    }
    if (events & (WIFI_MGR_WIFI_EVT_STA_CONNECTED | WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED |
                  WIFI_MGR_WIFI_EVT_STA_DISCONNECTED)) {
        wifi_mgr_radio_clear(WIFI_MGR_RADIO_ASYNC_CONNECT);
        if (s_wifi_mgr_obj->manual_autoconn_state == WIFI_MGR_AUTOCONN_MANUAL_RESUME_ON_EVENT &&
            s_wifi_mgr_obj->auto_connect_obj != NULL) {
            resume_auto = !wifi_mgr_radio_cancelled(s_wifi_mgr_obj->manual_autoconn_generation) &&
                s_wifi_mgr_obj->manual_autoconn_epoch == wifi_mgr_radio_read(&s_wifi_mgr_obj->auto_evidence_epoch);
            resume_config = s_wifi_mgr_obj->auto_connect_obj->config;
            resume_generation = s_wifi_mgr_obj->manual_autoconn_generation;
            resume_epoch = s_wifi_mgr_obj->manual_autoconn_epoch;
            s_wifi_mgr_obj->manual_autoconn_state = WIFI_MGR_AUTOCONN_MANUAL_IDLE;
        }
    }
    WIFI_MGR_MUTEX_UNLOCK();
    if (resume_auto) wifi_mgr_queue_auto_start(&resume_config, resume_generation, resume_epoch);

    if ((events & WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED) &&
        s_wifi_mgr_obj->suppress_next_pmk_connect_fail_callback) {
        s_wifi_mgr_obj->suppress_next_pmk_connect_fail_callback = false;
        LISA_LOGW(TAG, "Suppress PMK fast reconnect failure before fallback retry");
        wifi_mgr_radio_end(false, false, false);
        return;
    }

    if (!wifi_mgr_radio_is_paused()) {
        wifi_mgr_dispatch_user_callbacks(events, event_data, data_len);
    } else if (events & (WIFI_MGR_WIFI_EVT_STA_DISCONNECTED | WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED)) {
        /* Preserve manager bookkeeping, but callbacks arriving after a pause
         * barrier must never bring the network interface back up. */
        s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_DISCONNECTED;
    }
    wifi_mgr_radio_end(false, false, false);
}

static int wifi_cb_list_add_item(wifi_mgr_wifi_event_t event, void *event_handler, void* arg)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    wifi_mgr_event_cb_t *item = WIFI_MGR_CALLOC(1, sizeof(wifi_mgr_event_cb_t));
    if (item == NULL) {
        return -ENOMEM;
    }

    WIFI_MGR_MUTEX_LOCK();
    item->arg = arg;
    item->event = event;
    item->handler = event_handler;
    sys_dlist_prepend(&s_wifi_mgr_obj->wifi_callback_list, &item->node);
    WIFI_MGR_MUTEX_UNLOCK();
    return 0;
}

static int wifi_cb_list_remove_item(wifi_mgr_wifi_event_t event, void *event_handler)
{
    WIFI_MGR_MUTEX_LOCK();
    wifi_mgr_event_cb_t *removed_node = NULL;
    wifi_mgr_event_cb_t *entry, *next;
    SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&s_wifi_mgr_obj->wifi_callback_list, entry, next, node) {
        if (entry->event & event) {
            if (entry->handler == event_handler) {
                sys_dlist_remove(&entry->node);
                removed_node = entry;
                break;
            }
        }
    }
    if (removed_node == NULL) {
        WIFI_MGR_MUTEX_UNLOCK();
        return -ENOENT; /* Not found */
    }
    WIFI_MGR_FREE(removed_node);
    WIFI_MGR_MUTEX_UNLOCK();
    return 0;
}

static int ops_check(wifi_mgr_ops_t *ops)
{
    // 验证内存操作函数有效性
    WIFI_MGR_CHECK(ops->mem_ops->malloc != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->mem_ops->calloc != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->mem_ops->align_malloc != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->mem_ops->nocache_malloc != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->mem_ops->free != NULL, -EINVAL);
    
    // 验证系统操作函数有效性
    WIFI_MGR_CHECK(ops->os_ops->mutex_create != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->mutex_lock != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->mutex_unlock != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->mutex_delete != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->queue_create != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->queue_push != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->queue_pop != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->queue_delete != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->thread_create != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->os_ops->thread_delete != NULL, -EINVAL);

    /* 验证WiFi操作函数有效性 */
    WIFI_MGR_CHECK(ops->wifi_ops->init != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->wifi_ops->deinit != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->wifi_ops->add_callback != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->wifi_ops->remove_callback != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->wifi_ops->sta_connect != NULL, -EINVAL);
    WIFI_MGR_CHECK(ops->wifi_ops->sta_disconnect != NULL, -EINVAL);

    return 0;
}

int wifi_mgr_init(wifi_mgr_ops_t *ops)
{
    int ret = 0;
    uint32_t count = 0;
    bool wifi_initialized = false;
    bool callback_registered = false;
    
    WIFI_MGR_CHECK(s_wifi_mgr_obj == NULL, -EIO);
    WIFI_MGR_CHECK(ops != NULL, -EINVAL);

    ret = ops_check(ops);
    if (ret != 0) {
        LISA_LOGE(TAG, "ops_check failed");
        return ret;
    }
    
    s_wifi_mgr_obj = ops->mem_ops->calloc(1, sizeof(wifi_mgr_obj_t));
    if (s_wifi_mgr_obj == NULL) {
        LISA_LOGE(TAG, "mem_ops->calloc failed size: %ld", sizeof(wifi_mgr_obj_t));
        return -ENOMEM;
    }
    
    memcpy(&s_wifi_mgr_obj->mem_ops, ops->mem_ops, sizeof(wifi_manager_mem_ops_t));
    memcpy(&s_wifi_mgr_obj->os_ops, ops->os_ops, sizeof(wifi_manager_os_ops_t));
    memcpy(&s_wifi_mgr_obj->wifi_ops, ops->wifi_ops, sizeof(wifi_manager_wifi_ops_t));

    s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_DISCONNECTED;

    sys_dlist_init(&s_wifi_mgr_obj->wifi_callback_list);

    /* Callback registration may deliver an event immediately. */
    s_wifi_mgr_obj->mutex = s_wifi_mgr_obj->os_ops.mutex_create();
    if (s_wifi_mgr_obj->mutex == NULL) {
        ret = -ENOMEM;
        goto __cleanup;
    }

    ret = s_wifi_mgr_obj->wifi_ops.init();
    if (ret != 0) {
        LISA_LOGE(TAG, "wifi_ops.init failed");
        goto __cleanup;
    }
    wifi_initialized = true;

    s_wifi_mgr_obj->wifi_event_cb.handler = &wifi_event_handler;
    s_wifi_mgr_obj->wifi_event_cb.events = WIFI_MGR_WIFI_EVT_STA_CONNECTED |
                                            WIFI_MGR_WIFI_EVT_STA_DISCONNECTED |
                                            WIFI_MGR_WIFI_EVT_STA_CONNECTING |
                                            WIFI_MGR_WIFI_EVT_STA_CONNECTION_FAILED |
                                            WIFI_MGR_WIFI_EVT_SCAN_FAILED |
                                            WIFI_MGR_WIFI_EVT_SCAN_DONE;
    s_wifi_mgr_obj->wifi_event_cb.arg = NULL;
    ret = s_wifi_mgr_obj->wifi_ops.add_callback(&s_wifi_mgr_obj->wifi_event_cb);
    if (ret != 0) {
        LISA_LOGE(TAG, "wifi_ops.add_callback failed");
        goto __cleanup;
    }
    callback_registered = true;

    s_wifi_mgr_obj->auto_connect_obj = WIFI_MGR_MALLOC(sizeof(wifi_mgr_auto_conn_obj_t));
    if (s_wifi_mgr_obj->auto_connect_obj == NULL) {
        goto __cleanup;
    }
    memset(s_wifi_mgr_obj->auto_connect_obj, 0, sizeof(wifi_mgr_auto_conn_obj_t));
    s_wifi_mgr_obj->auto_connect_obj->config.interval_ms = WIFI_MGR_AUTOCONN_DEFAULT_INTERVAL_MS;
    s_wifi_mgr_obj->auto_connect_obj->config.max_interval_ms = 0U;
    s_wifi_mgr_obj->auto_connect_obj->base_interval_ms = WIFI_MGR_AUTOCONN_DEFAULT_INTERVAL_MS;
    s_wifi_mgr_obj->auto_connect_obj->current_interval_ms = WIFI_MGR_AUTOCONN_DEFAULT_INTERVAL_MS;
    s_wifi_mgr_obj->auto_connect_obj->no_candidate_streak = 0;
    sys_dlist_init(&s_wifi_mgr_obj->auto_connect_obj->failure_records_list);
    s_wifi_mgr_obj->manual_autoconn_state = WIFI_MGR_AUTOCONN_MANUAL_IDLE;
    s_wifi_mgr_obj->manual_autoconn_state = WIFI_MGR_AUTOCONN_MANUAL_IDLE;

    s_wifi_mgr_obj->queue = s_wifi_mgr_obj->os_ops.queue_create(WIFI_MGR_QUEUE_COUNT, WIFI_MGR_QUEUE_NAME, sizeof(wifi_mgr_queue_message_t));
    if (s_wifi_mgr_obj->queue == NULL) {
        goto __cleanup;
    }
    
    wifi_storage_ops_t storage_ops = {
        .malloc = s_wifi_mgr_obj->mem_ops.malloc,
        .calloc = s_wifi_mgr_obj->mem_ops.calloc,
        .free = s_wifi_mgr_obj->mem_ops.free,
        .mutex_lock = s_wifi_mgr_obj->os_ops.mutex_lock,
        .mutex_unlock = s_wifi_mgr_obj->os_ops.mutex_unlock
    };
    ret = wifi_storage_init(&s_wifi_mgr_obj->storage_ctx, &storage_ops, s_wifi_mgr_obj->mutex);
    if (ret != 0) {
        LISA_LOGE(TAG, "wifi_storage_init failed");
        goto __cleanup;
    }
    
    wifi_manager_os_thread_attr_t thread_attr = {
        .name = "wifi_mgr_thread",
        .priority = WIFI_MGR_THREAD_PRIORITY,
        .stack_size = WIFI_MGR_THREAD_STACK_SIZE
    };
    s_wifi_mgr_obj->thread = s_wifi_mgr_obj->os_ops.thread_create(&thread_attr, wifi_mgr_thread, NULL);
    if (s_wifi_mgr_obj->thread == NULL) {
        ret = -ENOMEM;
        goto __cleanup;
    }

    return 0;

__cleanup:
    LISA_LOGE(TAG, "free s_wifi_mgr_obj: %p", s_wifi_mgr_obj);
    if (s_wifi_mgr_obj) {
        if (callback_registered) {
            s_wifi_mgr_obj->wifi_ops.remove_callback(&s_wifi_mgr_obj->wifi_event_cb);
        }
        if (s_wifi_mgr_obj->queue) {
            s_wifi_mgr_obj->os_ops.queue_delete(s_wifi_mgr_obj->queue);
        }
        if (s_wifi_mgr_obj->mutex) {
            s_wifi_mgr_obj->os_ops.mutex_delete(s_wifi_mgr_obj->mutex);
        }
        if (s_wifi_mgr_obj->auto_connect_obj) {
            WIFI_MGR_FREE(s_wifi_mgr_obj->auto_connect_obj);
        }
        if (wifi_initialized) {
            s_wifi_mgr_obj->wifi_ops.deinit();
        }
        ops->mem_ops->free(s_wifi_mgr_obj);
        s_wifi_mgr_obj = NULL;
    }
    return ret;
}

int wifi_mgr_deinit()
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    
    // Step 1: Save local copies of resources and operations
    wifi_mgr_obj_t *obj = s_wifi_mgr_obj;
    void (*free_func)(void *) = obj->mem_ops.free;
    wifi_manager_os_ops_t os_ops = obj->os_ops;
    wifi_manager_wifi_ops_t wifi_ops = obj->wifi_ops;
    wifi_mgr_wifi_event_cb_t event_cb = obj->wifi_event_cb;
    void *thread = obj->thread;
    void *mutex = obj->mutex;
    void *queue = obj->queue;
    
    // Step 2: Unregister WiFi callback first to prevent new events
    // This is critical: no new events can arrive after this point
    wifi_ops.remove_callback(&event_cb);
    
    // Step 3: Signal thread to exit and wait for it
    os_ops.mutex_lock(mutex, _WAIT_FOREVER);
    obj->thread_exit = true;
    os_ops.mutex_unlock(mutex);
    
    os_ops.thread_delete(thread);
    
    // Step 4: Deinitialize WiFi driver
    wifi_ops.deinit();
    
    // Step 5: Clean up auto-connect resources
    if (obj->auto_connect_obj) {
        wifi_auto_connect_stop(obj->auto_connect_obj);
        // 释放失败记录链表
        wifi_mgr_ap_failure_record_t *entry, *next;
        SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&obj->auto_connect_obj->failure_records_list, entry, next, node) {
            sys_dlist_remove(&entry->node);
            free_func(entry);
        }
        free_func(obj->auto_connect_obj);
        obj->auto_connect_obj = NULL;
    }
    
    // Step 6: Clean up callback list
    wifi_mgr_event_cb_t *cb_entry, *cb_next;
    SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&obj->wifi_callback_list, cb_entry, cb_next, node) {
        sys_dlist_remove(&cb_entry->node);
        free_func(cb_entry);
    }
    
    // Step 7: Deinitialize storage
    wifi_storage_deinit(&obj->storage_ctx);

    // Step 8: Delete synchronization objects
    if (mutex) {
        os_ops.mutex_delete(mutex);
    }
    
    if (queue) {
        os_ops.queue_delete(queue);
    }
    
    // Step 9: Set global pointer to NULL (after all cleanup)
    s_wifi_mgr_obj = NULL;
    
    // Step 10: Finally free the main object
    free_func(obj);

    return 0;
}

int wifi_mgr_sta_enable()
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    return s_wifi_mgr_obj->wifi_ops.sta_enable();
}

int wifi_mgr_sta_disable()
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    return s_wifi_mgr_obj->wifi_ops.sta_disable();
}

bool wifi_mgr_sta_is_enable(void)
{
    if (s_wifi_mgr_obj == NULL) {
        return false;
    }
    return s_wifi_mgr_obj->wifi_ops.sta_is_enable();
}

int wifi_mgr_sta_connect(wifi_mgr_sta_config_t *sta_config, bool asynchronous)
{
    int ret = 0;
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    WIFI_MGR_CHECK(sta_config != NULL, -EINVAL);
    uint32_t generation;
    uint32_t epoch = 0;
    ret = wifi_mgr_radio_begin(asynchronous, false, &generation);
    if (ret != 0) return ret;

    uint32_t wait = asynchronous ? LISA_NO_WAIT : CONFIG_WIFI_MGR_CONNECT_TIMEOUT;
    LISA_LOGD(TAG, "start to connect ssid: %s, pwd: %s", sta_config->ssid, sta_config->pwd);
    LISA_LOGI(TAG, "wifi_mgr_sta_connect: pmk_valid: %d", sta_config->pmk_valid);

    if (s_wifi_mgr_obj->auto_connect_obj != NULL) {
        wifi_mgr_auto_connect_stop_epoch(&epoch);
        WIFI_MGR_MUTEX_LOCK();
        if (!wifi_mgr_radio_cancelled(generation) &&
            epoch == wifi_mgr_radio_read(&s_wifi_mgr_obj->auto_evidence_epoch)) {
            s_wifi_mgr_obj->manual_autoconn_generation = generation;
            s_wifi_mgr_obj->manual_autoconn_epoch = epoch;
            s_wifi_mgr_obj->manual_autoconn_state = asynchronous ?
                WIFI_MGR_AUTOCONN_MANUAL_RESUME_ON_EVENT : WIFI_MGR_AUTOCONN_MANUAL_PAUSED_SYNC;
        }
        WIFI_MGR_MUTEX_UNLOCK();
    }

    ret = wifi_mgr_radio_connect(sta_config, wait);
    bool resume_auto = false;
    wifi_mgr_autoconn_config_t resume_config = {0};
    WIFI_MGR_MUTEX_LOCK();
    if (ret == 0 && asynchronous) {
        if (wifi_mgr_radio_read(&s_wifi_mgr_obj->radio_state) & WIFI_MGR_RADIO_ASYNC_CONNECT) {
            s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_CONNECTING;
        }
    } else if (s_wifi_mgr_obj->manual_autoconn_state != WIFI_MGR_AUTOCONN_MANUAL_IDLE &&
               s_wifi_mgr_obj->manual_autoconn_generation == generation) {
        resume_auto = !wifi_mgr_radio_cancelled(generation) && s_wifi_mgr_obj->auto_connect_obj != NULL &&
            epoch == wifi_mgr_radio_read(&s_wifi_mgr_obj->auto_evidence_epoch);
        if (resume_auto) resume_config = s_wifi_mgr_obj->auto_connect_obj->config;
        s_wifi_mgr_obj->manual_autoconn_state = WIFI_MGR_AUTOCONN_MANUAL_IDLE;
    }
    WIFI_MGR_MUTEX_UNLOCK();
    if (resume_auto) wifi_mgr_queue_auto_start(&resume_config, generation, epoch);
    wifi_mgr_radio_end(asynchronous, false, ret != 0 && ret != -ETIMEDOUT);
    return ret;
}

int wifi_mgr_sta_add_connection_cb(wifi_mgr_connection_cb_t connection_cb, void *arg)
{
    WIFI_MGR_CHECK(connection_cb != NULL, -EINVAL);
    int ret = wifi_cb_list_add_item(WIFI_MGR_CONNECTION_EVENT,
        connection_cb, arg);
    return ret;
}

int wifi_mgr_sta_remove_connection_cb(wifi_mgr_connection_cb_t connection_cb)
{
    WIFI_MGR_CHECK(connection_cb != NULL, -EINVAL);
    int ret = wifi_cb_list_remove_item(WIFI_MGR_CONNECTION_EVENT, connection_cb);
    return ret;
}

int wifi_mgr_sta_disconnect(bool asynchronous)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    uint32_t wait = asynchronous ? LISA_NO_WAIT : LISA_WAIT_FOREVER;
    LISA_LOGD(TAG, "start to disconnect, wait: %lu", wait);
    bool was_connected = (s_wifi_mgr_obj->sta_device.sta_status == WIFI_MGR_STA_CONNECTED);
    wifi_mgr_sta_config_t last_connected_cfg = {0};
    if (was_connected) {
        memcpy(&last_connected_cfg, &s_wifi_mgr_obj->sta_device.config, sizeof(last_connected_cfg));
    }
    int ret = s_wifi_mgr_obj->wifi_ops.sta_disconnect(wait);
    if (ret < 0) {
        LISA_LOGW(TAG, "station disconnect failed, ret: %d", ret);
    }
    if (was_connected && s_wifi_mgr_obj->auto_connect_obj != NULL) {
        set_ap_failure_count_to_max(s_wifi_mgr_obj->auto_connect_obj, last_connected_cfg.ssid, "");
    }
    return ret;
}

int wifi_mgr_sta_get_connected_info(wifi_mgr_sta_config_t *sta_info)
{
    WIFI_MGR_CHECK(sta_info != NULL, -EINVAL);
    WIFI_MGR_MUTEX_LOCK();
    if (s_wifi_mgr_obj->sta_device.sta_status != WIFI_MGR_STA_CONNECTED) {
        LISA_LOGE(TAG,"WiFi station is not connected");
        WIFI_MGR_MUTEX_UNLOCK();
        return -ENOENT; //Wifi station is not connected
    }
    memcpy(sta_info, &s_wifi_mgr_obj->sta_device.config, sizeof(wifi_mgr_sta_config_t));
    WIFI_MGR_MUTEX_UNLOCK();
    return 0;
}

int wifi_mgr_scan_ap(wifi_mgr_scan_info_t *ap_info, uint32_t size, bool asynchronous)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    int ret = wifi_mgr_radio_begin(asynchronous, true, NULL);
    if (ret != 0) return ret;
    if (!s_wifi_mgr_obj->wifi_ops.sta_is_enable()) {
        LISA_LOGW(TAG, "STA is disabled, scan not allowed");
        wifi_mgr_radio_end(asynchronous, true, true);
        return -ENODEV;
    }
    
    uint32_t wait = asynchronous ? LISA_NO_WAIT : LISA_WAIT_FOREVER;
    ret = s_wifi_mgr_obj->wifi_ops.scan_ap(ap_info, size, wait);
    
    if (ret < 0) {
        LISA_LOGW(TAG, "Scan failed, ret: %d", ret);
    }
    wifi_mgr_radio_end(asynchronous, true, ret < 0);
    return ret;
}

int wifi_mgr_add_scan_done_cb(wifi_mgr_scan_done_cb_t scandone_cb, void *arg)
{
    WIFI_MGR_CHECK(scandone_cb != NULL, -EINVAL);
    int ret = wifi_cb_list_add_item(WIFI_MGR_WIFI_EVT_SCAN_DONE | WIFI_MGR_WIFI_EVT_SCAN_FAILED,
                                    scandone_cb, arg);
    return ret;
}

int wifi_mgr_remove_scan_done_cb(wifi_mgr_scan_done_cb_t scandone_cb)
{
    WIFI_MGR_CHECK(scandone_cb != NULL, -EINVAL);
    int ret = wifi_cb_list_remove_item(WIFI_MGR_WIFI_EVT_SCAN_DONE | WIFI_MGR_WIFI_EVT_SCAN_FAILED,
                                       scandone_cb);
    return ret;
}

int wifi_mgr_storage_search_ap(wifi_mgr_sta_config_t *matched_list, int max_count,
                               wifi_mgr_storage_search_mode_t search_modes, void* target)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    
    wifi_mgr_sta_config_t *temp_list = NULL;
    int ret = wifi_storage_search_ap(&s_wifi_mgr_obj->storage_ctx, &temp_list, search_modes, target);

    if (ret > 0 && temp_list != NULL) {
        if (matched_list != NULL && max_count > 0) {
            int copy_count = (ret < max_count) ? ret : max_count;
            memcpy(matched_list, temp_list, copy_count * sizeof(wifi_mgr_sta_config_t));
        }
        WIFI_MGR_FREE(temp_list);
    }

    return ret;
}

int wifi_mgr_storage_save_ap(wifi_mgr_sta_config_t *ap_info)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    if (ap_info == NULL || ap_info->ssid[0] == '\0') {
        return -EINVAL;
    }
    return wifi_storage_save_ap(&s_wifi_mgr_obj->storage_ctx, ap_info);
}

int wifi_mgr_storage_delete_ap(wifi_mgr_sta_config_t *ap_info)
{
    WIFI_MGR_CHECK(s_wifi_mgr_obj != NULL, -EIO);
    return wifi_storage_delete_ap(&s_wifi_mgr_obj->storage_ctx, ap_info);
}

/* Consume each scan entry once; always select the strongest remaining saved AP. */
static int find_next_ap_from_list(wifi_mgr_scan_info_t *aps, int count, wifi_mgr_sta_config_t *next)
{
    int best = -1;
    for (int idx = 0; idx < count; idx++) {
        if (!aps[idx].ssid[0] ||
            is_ap_blacklisted(s_wifi_mgr_obj->auto_connect_obj, aps[idx].ssid, aps[idx].bssid))
            continue;
        wifi_mgr_sta_config_t *matches = NULL;
        int hits = wifi_storage_search_ap(&s_wifi_mgr_obj->storage_ctx, &matches, SEARCH_BY_BSSID, aps[idx].bssid);
        if (hits < 0) {
            if (matches) s_wifi_mgr_obj->storage_ctx.ops.free(matches);
            return hits;
        }
        if (!hits) {
            if (matches) s_wifi_mgr_obj->storage_ctx.ops.free(matches);
            matches = NULL;
            hits = wifi_storage_search_ap(&s_wifi_mgr_obj->storage_ctx, &matches, SEARCH_BY_SSID, aps[idx].ssid);
        }
        if (hits < 0 || (hits > 0 && !matches)) {
            if (matches) s_wifi_mgr_obj->storage_ctx.ops.free(matches);
            return hits < 0 ? hits : -EIO;
        }
        for (int i = 0; i < hits; i++) {
            if (!matches[i].ssid[0] || (matches[i].bssid[0] && strcmp(matches[i].bssid, aps[idx].bssid)))
                continue;
            if (best < 0 || aps[idx].rssi > next->rssi) {
                *next = matches[i];
                memcpy(next->bssid, aps[idx].bssid, sizeof(next->bssid));
                next->rssi = aps[idx].rssi;
                best = idx;
            }
            break;
        }
        if (matches) s_wifi_mgr_obj->storage_ctx.ops.free(matches);
    }
    if (best < 0) return -ENODEV;
    for (int i = 0; i < count; i++) {
        if (!strcmp(aps[i].bssid, next->bssid)) aps[i].ssid[0] = '\0';
    }
    return 0;
}

static bool wifi_mgr_auto_cycle_cancelled(uint32_t generation, uint32_t epoch)
{
    return wifi_mgr_radio_cancelled(generation) ||
        epoch != wifi_mgr_radio_read(&s_wifi_mgr_obj->auto_evidence_epoch);
}

/* A timed wait leaves association active. Await its terminal callback before
 * advancing; cancellation retains the HAL marker for the radio pause barrier. */
static int wifi_mgr_auto_connect_ap(wifi_mgr_sta_config_t *ap, uint32_t generation, uint32_t epoch,
                                   bool *association_failed)
{
    *association_failed = false;
    uint32_t terminal = wifi_mgr_radio_read(&s_wifi_mgr_obj->connect_terminal_sequence);
    int ret = wifi_mgr_radio_connect(ap, CONFIG_WIFI_MGR_AUTO_CONNECT_TIMEOUT_MS);
    if (ret == -ETIMEDOUT) {
        while (wifi_mgr_radio_read(&s_wifi_mgr_obj->radio_state) & WIFI_MGR_RADIO_ASYNC_CONNECT) {
            if (wifi_mgr_auto_cycle_cancelled(generation, epoch)) return -ECANCELED;
            lisa_thread_mdelay(10);
        }
        ret = s_wifi_mgr_obj->sta_device.sta_status == WIFI_MGR_STA_CONNECTED ? 0 :
            (s_wifi_mgr_obj->last_connect_fail_error ? s_wifi_mgr_obj->last_connect_fail_error : -EIO);
    }
    /* A HAL submission/resource error says nothing about this AP. Count only
     * a terminal callback from this attempt, independently for the PMK retry. */
    *association_failed = ret != 0 && ret != -ENOMEM && ret != -EBUSY &&
        ret != -EAGAIN && ret != -ECANCELED &&
        terminal != wifi_mgr_radio_read(&s_wifi_mgr_obj->connect_terminal_sequence);
    return ret;
}

static wifi_mgr_autoconn_cycle_result_t autoconnect_work_handler(uint32_t generation, uint32_t epoch)
{
    if (wifi_mgr_auto_cycle_cancelled(generation, epoch)) return WIFI_MGR_AUTOCONN_CYCLE_NO_CANDIDATE;
    if (!s_wifi_mgr_obj->wifi_ops.sta_is_enable()) return WIFI_MGR_AUTOCONN_CYCLE_NO_CANDIDATE;
    if (s_wifi_mgr_obj->sta_device.sta_status == WIFI_MGR_STA_CONNECTED) return WIFI_MGR_AUTOCONN_CYCLE_SUCCESS;
    wifi_mgr_scan_info_t *aps = WIFI_MGR_MALLOC(sizeof(*aps) * WIFI_SCAN_MAX_NUMBER);
    if (!aps) return WIFI_MGR_AUTOCONN_CYCLE_NO_CANDIDATE;
    int count = s_wifi_mgr_obj->wifi_ops.scan_ap(aps, WIFI_SCAN_MAX_NUMBER, LISA_WAIT_FOREVER);
    bool exhausted = false;
    int attempted = 0;
    wifi_mgr_autoconn_cycle_result_t result = WIFI_MGR_AUTOCONN_CYCLE_NO_CANDIDATE;
    if (count < 0 || count > WIFI_SCAN_MAX_NUMBER ||
        wifi_mgr_auto_cycle_cancelled(generation, epoch)) goto done;
    for (int i = 0; i < count; i++) {
        /* Malformed scan data does not prove saved APs are unavailable. */
        if (!is_valid_bssid_format(aps[i].bssid)) goto done;
    }
    for (;;) {
        if (wifi_mgr_auto_cycle_cancelled(generation, epoch)) break;
        wifi_mgr_sta_config_t ap = {0};
        int ret = find_next_ap_from_list(aps, count, &ap);
        if (ret == -ENODEV) { exhausted = true; break; }
        if (ret != 0) break;
        s_wifi_mgr_obj->last_connect_fail_error = 0;
        s_wifi_mgr_obj->suppress_next_pmk_connect_fail_callback = ap.pmk_valid;
        bool association_failed;
        ret = wifi_mgr_auto_connect_ap(&ap, generation, epoch, &association_failed);
        s_wifi_mgr_obj->suppress_next_pmk_connect_fail_callback = false;
        if (association_failed && !wifi_mgr_auto_cycle_cancelled(generation, epoch) && ap.pmk_valid) {
            ap.pmk_valid = 0;
            memset(ap.pmk, 0, sizeof(ap.pmk));
            s_wifi_mgr_obj->last_connect_fail_error = 0;
            ret = wifi_mgr_auto_connect_ap(&ap, generation, epoch, &association_failed);
        }
        if (ret == 0) {
            reset_ap_failure_count(s_wifi_mgr_obj->auto_connect_obj, ap.ssid, ap.bssid);
            wifi_mgr_auto_evidence_reset(false);
            result = WIFI_MGR_AUTOCONN_CYCLE_SUCCESS;
            break;
        }
        if (!association_failed || wifi_mgr_auto_cycle_cancelled(generation, epoch)) break;
        attempted++;
        increment_ap_failure_count(s_wifi_mgr_obj->auto_connect_obj, ap.ssid, ap.bssid);
        result = WIFI_MGR_AUTOCONN_CYCLE_CONNECT_FAIL;
    }
done:
    WIFI_MGR_FREE(aps);
    taskENTER_CRITICAL();
    if (exhausted && epoch == s_wifi_mgr_obj->auto_evidence_epoch &&
        generation == s_wifi_mgr_obj->radio_generation &&
        s_wifi_mgr_obj->sta_device.sta_status != WIFI_MGR_STA_CONNECTED &&
        !(s_wifi_mgr_obj->radio_state & WIFI_MGR_RADIO_PAUSED))
        s_wifi_mgr_obj->auto_exhausted = true;
    taskEXIT_CRITICAL();
    if (exhausted) LISA_LOGI(TAG, "saved AP cycle exhausted after %d attempts", attempted);
    return result;
}

static int wifi_auto_connect_stop(wifi_mgr_auto_conn_obj_t *auto_connect_obj)
{

    return 0;
}

int wifi_mgr_auto_connect_start(wifi_mgr_autoconn_config_t *autoconn_config)
{
    WIFI_MGR_CHECK(autoconn_config != NULL, -EINVAL);

    if(s_wifi_mgr_obj == NULL){
        return -EAGAIN;
    }

    uint32_t epoch = wifi_mgr_auto_evidence_reset(true);
    uint32_t generation = wifi_mgr_radio_read(&s_wifi_mgr_obj->radio_generation);
    return wifi_mgr_queue_auto_start(autoconn_config, generation, epoch);
}

static int wifi_mgr_auto_connect_stop_epoch(uint32_t *epoch)
{
    wifi_mgr_queue_message_t msg;
    int ret;

    if(s_wifi_mgr_obj == NULL){
        return -EAGAIN;
    }

    uint32_t cancelled_epoch = wifi_mgr_auto_evidence_reset(true);
    if (epoch) *epoch = cancelled_epoch;
    msg.event = WIFI_MGR_QUEUE_MSG_EVENT_WIFI_AUTO_CONNECT_STOP;
    msg.payload = NULL;

    ret = s_wifi_mgr_obj->os_ops.queue_push(s_wifi_mgr_obj->queue, &msg, sizeof(wifi_mgr_queue_message_t), LISA_NO_WAIT);

    return ret;
}

int wifi_mgr_auto_connect_stop(void)
{
    return wifi_mgr_auto_connect_stop_epoch(NULL);
}

wifi_mgr_connection_status_t wifi_mgr_sta_get_status()
{
    wifi_mgr_wifi_status_t wifi_sta_status;
    if (s_wifi_mgr_obj == NULL) {
        return WIFI_MGR_STA_DISCONNECTED;
    }
    WIFI_MGR_MUTEX_LOCK();
    wifi_sta_status = s_wifi_mgr_obj->wifi_ops.sta_get_status();

    switch (wifi_sta_status) {
        case WIFI_MGR_WIFI_STATUS_STA_DISCONNECTED:
        if (s_wifi_mgr_obj->sta_device.sta_status != WIFI_MGR_STA_CONNECTING) {
            s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_DISCONNECTED;
        }
        break;

        case WIFI_MGR_WIFI_STATUS_STA_CONNECTING:
        s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_CONNECTING;
        break;

        case WIFI_MGR_WIFI_STATUS_STA_CONNECTED:
        s_wifi_mgr_obj->sta_device.sta_status = WIFI_MGR_STA_CONNECTED;
        break;

        default:
        break;
    }
    WIFI_MGR_MUTEX_UNLOCK();
    return s_wifi_mgr_obj->sta_device.sta_status;
}

static void wifi_mgr_thread(void *arg){
    int result;
    wifi_mgr_queue_message_t msg;
    wifi_mgr_autoconn_request_t *request;

    while(1){
        if(s_wifi_mgr_obj->thread_exit){
            LISA_LOGI(TAG, "wifi_mgr_thread exit\n");
            return;
        }
        
        if (!s_wifi_mgr_obj->auto_connect_obj) {
            LISA_LOGE(TAG, "auto_connect_obj is NULL, thread exiting");
            return;
        }

        uint32_t wait_ms = s_wifi_mgr_obj->auto_connect_obj->current_interval_ms;
        if (wait_ms == 0U) {
            wait_ms = s_wifi_mgr_obj->auto_connect_obj->base_interval_ms;
        }
        
        result = s_wifi_mgr_obj->os_ops.queue_pop(s_wifi_mgr_obj->queue, &msg, sizeof(wifi_mgr_queue_message_t), wait_ms);
        if(result == LISA_OK){
            WIFI_MGR_MUTEX_LOCK();
            switch(msg.event){
                case WIFI_MGR_QUEUE_MSG_EVENT_WIFI_AUTO_CONNECT_START:
                    request = (wifi_mgr_autoconn_request_t *)msg.payload;
                    if (s_wifi_mgr_obj->auto_connect_obj && request &&
                        request->evidence_epoch == wifi_mgr_radio_read(&s_wifi_mgr_obj->auto_evidence_epoch) &&
                        !wifi_mgr_radio_cancelled(request->generation)) {
                        s_wifi_mgr_obj->auto_connect_obj->enable = true;
                        s_wifi_mgr_obj->auto_connect_obj->generation = request->generation;
                        s_wifi_mgr_obj->auto_connect_obj->evidence_epoch = request->evidence_epoch;
                        s_wifi_mgr_obj->auto_connect_obj->config = request->config;
                        s_wifi_mgr_obj->auto_connect_obj->base_interval_ms =
                            (s_wifi_mgr_obj->auto_connect_obj->config.interval_ms == 0U) ?
                            WIFI_MGR_AUTOCONN_DEFAULT_INTERVAL_MS :
                            s_wifi_mgr_obj->auto_connect_obj->config.interval_ms;
                        s_wifi_mgr_obj->auto_connect_obj->current_interval_ms =
                            s_wifi_mgr_obj->auto_connect_obj->base_interval_ms;
                        s_wifi_mgr_obj->auto_connect_obj->no_candidate_streak = 0;
                    }
                    break;
                case WIFI_MGR_QUEUE_MSG_EVENT_WIFI_AUTO_CONNECT_STOP:
                    if (s_wifi_mgr_obj->auto_connect_obj) {
                        s_wifi_mgr_obj->auto_connect_obj->enable = false;
                    }
                    break;
                default:
                    break;
            }
            WIFI_MGR_MUTEX_UNLOCK();
            if(msg.payload){
                WIFI_MGR_FREE(msg.payload);
            }
        }
        
        WIFI_MGR_MUTEX_LOCK();
        bool run_auto = s_wifi_mgr_obj->auto_connect_obj &&
            s_wifi_mgr_obj->auto_connect_obj->enable &&
            s_wifi_mgr_obj->auto_connect_obj->evidence_epoch == wifi_mgr_radio_read(&s_wifi_mgr_obj->auto_evidence_epoch) &&
            !wifi_mgr_radio_cancelled(s_wifi_mgr_obj->auto_connect_obj->generation);
        uint32_t generation = s_wifi_mgr_obj->auto_connect_obj->generation;
        uint32_t epoch = s_wifi_mgr_obj->auto_connect_obj->evidence_epoch;
        WIFI_MGR_MUTEX_UNLOCK();
        if(run_auto && wifi_mgr_radio_begin(false, false, NULL) == 0){
            if (wifi_mgr_radio_cancelled(generation)) {
                wifi_mgr_radio_end(false, false, false);
                continue;
            }
            wifi_mgr_autoconn_cycle_result_t cycle = autoconnect_work_handler(generation, epoch);
            if (cycle == WIFI_MGR_AUTOCONN_CYCLE_NO_CANDIDATE) {
                s_wifi_mgr_obj->auto_connect_obj->no_candidate_streak++;
            } else {
                s_wifi_mgr_obj->auto_connect_obj->no_candidate_streak = 0;
            }

            s_wifi_mgr_obj->auto_connect_obj->current_interval_ms =
                wifi_mgr_autoconn_next_interval_ms(
                    s_wifi_mgr_obj->auto_connect_obj->base_interval_ms,
                    s_wifi_mgr_obj->auto_connect_obj->current_interval_ms,
                    s_wifi_mgr_obj->auto_connect_obj->no_candidate_streak,
                    cycle,
                    s_wifi_mgr_obj->auto_connect_obj->config.max_interval_ms);
            wifi_mgr_radio_end(false, false, false);
        }
    }
}
