/*
 * Copyright (c) 2025, LISTENAI
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "cJSON.h"
#include "lisa_bluetooth.h"
#include "lisa_ble_api.h"
#include "ble_adv_data.h"
#include "bt_stack_cfg.h"
#include "bt_app_hal.h"
#include "lisa_kv.h"
#include "kv_user.h"
#include "netcfg_ble.h"
#include "netcfg_bles.h"
#include "ble_gap.h"
#include "ble_plf_config.h"
#include "voice_msg.h"
#include "sys_network_manager.h"
#include "sys_wifi.h"
#include "app_netcfg_portal.h"
#include "app_ble_common.h"
#include "app_datas.h"
#include "lingxin_server_config.h"
#include "project_version.h"

#if CONFIG_LISA_MODEM
#include "lisa_modem_module.h"
#endif

#if defined(CONFIG_CLOUD_PRODUCT_ID_DEFAULT)
#define PRODUCT_ID CONFIG_CLOUD_PRODUCT_ID_DEFAULT
#else
#define PRODUCT_ID "cf75e7a9-66b6-41e3-918a-141800aebb5b"
#endif

#define TAG "app_ble"
#include "lisa_log.h"

/*
 * BLE ADV data
 */

static uint8_t user_adv_data[] = {
    BLE_AD_FLAGS(GAP_AD_TYPE_FLAGS_GENERAL | GAP_AD_TYPE_FLAGS_BREDR_NOT_SUPPORTED),
    BLE_AD_MFG_DATA(19,
        0xab, 0x0a, 0xa1, 0xdc, 0xa8, 0x76, 0x83, 0x65,
        0x73, 0x83, 0x72, 0x65, 0x82, 0x67, 0x83, 0x68,
        0x00, 0x78, 0x00),
};

/* Keep the manufacturer block in the primary legacy packet.  The complete
 * 11-byte name belongs in scan response so neither payload exceeds 31 bytes. */
static const uint8_t user_scan_rsp_data[] = {
    BLE_AD_COMPLETE_NAME(11, 'x', 'i', 'a', 'o', 'q', 'i', 'u', 'q', 'u', 'a', 'n'),
    BLE_AD_UUID16_COMPLETE(2, 0x12, 0x18),
};

const uint8_t *lisa_bt_get_adv_data(uint8_t *len)
{
#if CONFIG_LISA_MODEM
    user_adv_data[sizeof(user_adv_data) - 1] = lisa_modem_is_present() ? 0x01 : 0x00;
#endif
    *len = sizeof(user_adv_data);
    return user_adv_data;
}

const uint8_t *lisa_bt_get_scan_rsp_data(uint8_t *len)
{
    *len = sizeof(user_scan_rsp_data);
    return user_scan_rsp_data;
}

/*
 * BLE netcfg - replaces SDK's app_net_cfg.c with AUTH_INFO support
 */

static lisa_ble_netcfg_handler_t s_netcfg_handler;
static bool s_ble_connected;
static bool s_ble_offline_paused;
static bool s_ble_transaction_busy;
static bool s_ble_subscribed;
static volatile uint32_t s_ble_generation;
static uint32_t s_ble_tx_generation;
static volatile bool s_ble_notify_running;
static volatile bool s_ble_scan_running;
static uint8_t s_ble_conidx;
static volatile bool s_ble_netcfg_adv_pending;
static volatile bool s_ble_netcfg_connect_pending;
static volatile bool s_ble_netcfg_active;
static volatile bool s_ble_netcfg_paused;
static bool s_ble_netcfg_resume_adv;
static bool s_ble_sleep_disconnect;
static uint8_t s_ble_sleep_disconnect_conidx;
static void app_ble_netcfg_adv_schedule(bool reset_bond);
static void app_ble_send_wifi_provision_fail(uint8_t conidx, uint8_t reason);
static void app_ble_send_device_info(uint8_t conidx, const char *event);

typedef enum {
    APP_BLE_NOTIFY_IDENTITY,
    APP_BLE_NOTIFY_CONNECTED,
    APP_BLE_NOTIFY_AUTH,
    APP_BLE_NOTIFY_FAILURE,
    APP_BLE_NOTIFY_SCAN,
} app_ble_notify_kind_t;
typedef struct {
    uint32_t generation;
    uint8_t conidx;
    app_ble_notify_kind_t kind;
    uint8_t reason;
    bool refresh;
} app_ble_notify_job_t;
static QueueHandle_t s_ble_notify_queue;
static bool app_ble_notify_enqueue(uint8_t conidx, app_ble_notify_kind_t kind, uint8_t reason, bool refresh);
static bool app_ble_notify_enqueue_at(uint8_t conidx, app_ble_notify_kind_t kind, uint8_t reason,
                                      bool refresh, uint32_t generation);

#define APP_BLE_NETCFG_ADV_DELAY_MS 1000U
#define APP_BLE_NETCFG_CONNECT_TIMEOUT_MS 20000U
#define APP_BLE_NETCFG_CONNECT_POLL_MS 200U

typedef struct {
    uint8_t conidx;
    uint32_t generation;
    struct netcfg_ble_data data;
} app_ble_netcfg_connect_ctx_t;

void lisa_ble_netcfg_set_handler(lisa_ble_netcfg_handler_t handler)
{
    s_netcfg_handler = handler;
}

static void app_ble_on_connected(uint8_t conidx, uint16_t conhdl, const gap_bdaddr_t *peer_addr)
{
    (void)conhdl;
    (void)peer_addr;

    s_ble_connected = true;
    s_ble_conidx = conidx;
    s_ble_subscribed = false;
    s_ble_generation++;
    LISA_LOGI(TAG, "BLE connected, conidx=%u", conidx);
    if (s_ble_netcfg_paused && !s_ble_offline_paused) {
        s_ble_sleep_disconnect = true;
        s_ble_sleep_disconnect_conidx = conidx;
        lisa_ble_disconnect(conidx, 0x13);
    }
}

void netcfg_bles_con_cleanup(uint8_t conidx, uint16_t reason)
{
    LISA_LOGI(TAG, "BLE disconnected, conidx=%u, reason=0x%04X", conidx, reason);
    if (s_ble_connected && s_ble_conidx == conidx) {
        s_ble_connected = false;
        s_ble_subscribed = false;
        s_ble_generation++;
    }
    netcfg_bles_set_state(NETCFG_BLE_IDLE);
}

static void app_ble_on_disconnected(uint8_t conidx, uint16_t conhdl, uint16_t reason)
{
    (void)conhdl;
    bool preserve_bond = s_ble_sleep_disconnect && s_ble_sleep_disconnect_conidx == conidx;
    if (preserve_bond) {
        s_ble_sleep_disconnect = false;
    }
    netcfg_bles_con_cleanup(conidx, reason);
    if (!s_ble_netcfg_paused && s_ble_netcfg_active && !s_ble_netcfg_connect_pending) {
        LISA_LOGI(TAG, "BLE netcfg link dropped before completion, resume advertising");
        app_ble_netcfg_adv_schedule(!preserve_bond);
    }
}

static void app_ble_on_bond(uint8_t conidx, uint8_t info, uint8_t value)
{
    LISA_LOGI(TAG, "BLE bond event, conidx=%u, info=%u, value=%u", conidx, info, value);
}

void app_ble_netcfg_prepare(void)
{
    lisa_ble_adv_stop(0);
    if (s_ble_connected) {
        LISA_LOGI(TAG, "Disconnect current BLE link before netcfg, conidx=%u", s_ble_conidx);
        lisa_ble_disconnect(s_ble_conidx, 0x13);
    }

    LISA_LOGI(TAG, "Clear BLE bond info before netcfg");
    ble_gap_delete_bond(NULL);
    bt_stack_nvs_del(NVS_ID_PEER_ADDRESS);
    netcfg_bles_set_state(NETCFG_BLE_IDLE);
}

static void app_ble_netcfg_adv_start_task(void *arg)
{
    uint8_t ret = 0;

    bool reset_bond = (uintptr_t)arg != 0U;

    if (!s_ble_netcfg_paused && reset_bond) {
        app_ble_netcfg_prepare();
    }
    vTaskDelay(pdMS_TO_TICKS(APP_BLE_NETCFG_ADV_DELAY_MS));

    /* Credentials may arrive through the web portal while this delayed task
     * is sleeping.  Never resurrect provisioning advertisements after the
     * provisioning session has completed. */
    if (s_ble_netcfg_paused || !s_ble_netcfg_active) {
        LISA_LOGI(TAG, "Delayed BLE adv start cancelled: netcfg completed");
        s_ble_netcfg_adv_pending = false;
        vTaskDelete(NULL);
        return;
    }

    ret = app_ble_adv_start(0, BLE_ADV_GEN);
    if (ret != 0) {
        LISA_LOGW(TAG, "Delayed BLE adv start failed: %u", (unsigned)ret);
    }

    s_ble_netcfg_adv_pending = false;
    vTaskDelete(NULL);
}

static void app_ble_netcfg_adv_schedule(bool reset_bond)
{
    taskENTER_CRITICAL();
    if (s_ble_netcfg_paused) {
        taskEXIT_CRITICAL();
        return;
    }
    s_ble_netcfg_active = true;
    if (s_ble_connected) {
        taskEXIT_CRITICAL();
        LISA_LOGI(TAG, "BLE netcfg already connected, keep current link");
        return;
    }
    if (s_ble_netcfg_adv_pending) {
        taskEXIT_CRITICAL();
        return;
    }

    s_ble_netcfg_adv_pending = true;
    taskEXIT_CRITICAL();
    if (xTaskCreate(app_ble_netcfg_adv_start_task, "ble_adv_delay", 2048,
                    (void *)(uintptr_t)(reset_bond ? 1U : 0U), 5, NULL) != pdPASS) {
        s_ble_netcfg_adv_pending = false;
        LISA_LOGW(TAG, "Failed to create delayed BLE adv task");
    }
}

static int netcfg_ble_wifi_connect(const int8_t *ssid, const int8_t *pwd)
{
    if (!ssid || !pwd)
        return -1;

    if (!s_netcfg_handler)
        return -1;

    return s_netcfg_handler((const char *)ssid, (const char *)pwd);
}

static bool netcfg_ble_data_is_empty(const struct netcfg_ble_data *data)
{
    return data && data->ssid[0] == '\0' && data->pwd[0] == '\0';
}

uint16_t netcfg_ble_notify_wifi(struct netcfg_ble_data *data)
{
    int status;

    LISA_LOGI(TAG, "BLE netcfg received WiFi credentials for ssid=%s", data->ssid);

    status = netcfg_ble_wifi_connect((const int8_t *)data->ssid, (const int8_t *)data->pwd);
    if (status != 0)
        return NETCFG_BLE_ERR;

    return NETCFG_BLE_SUCCESS;
}

void app_ble_netcfg_adv_start_delayed(void)
{
    app_ble_netcfg_adv_schedule(true);
}

void app_ble_netcfg_complete(void)
{
    s_ble_netcfg_active = false;
    s_ble_netcfg_resume_adv = false;
    lisa_ble_adv_stop(0);
    LISA_LOGI(TAG, "BLE netcfg advertising stopped after provisioning completion");
}

int app_ble_netcfg_set_paused(bool paused)
{
    bool busy;
    bool restore_adv = false;
    taskENTER_CRITICAL();
    s_ble_offline_paused = false; /* Explicit standby owns its own pause. */
    if (paused && !s_ble_netcfg_paused) {
        s_ble_netcfg_resume_adv = s_ble_netcfg_active;
    } else if (!paused && s_ble_netcfg_paused) {
        restore_adv = s_ble_netcfg_resume_adv;
        s_ble_netcfg_resume_adv = false;
    }
    s_ble_netcfg_paused = paused;
    busy = s_ble_netcfg_connect_pending || s_ble_netcfg_adv_pending || s_ble_notify_running;
    taskEXIT_CRITICAL();
    if (paused) {
        lisa_ble_adv_stop(0);
        if (s_ble_connected) {
            s_ble_sleep_disconnect = true;
            s_ble_sleep_disconnect_conidx = s_ble_conidx;
            lisa_ble_disconnect(s_ble_conidx, 0x13);
        }
    } else if (restore_adv) {
        app_ble_netcfg_adv_schedule(false);
    }
    return paused && busy ? -EBUSY : 0;
}

bool app_ble_netcfg_offline_blocked(void)
{
    taskENTER_CRITICAL();
    bool blocked = s_ble_transaction_busy;
    taskEXIT_CRITICAL();
    return blocked;
}

int app_ble_netcfg_offline_acquire(void)
{
    /* Compatibility gate only: offline interaction never owns the radio. */
    return app_ble_netcfg_offline_blocked() ? -EBUSY : 0;
}

void app_ble_netcfg_offline_release(void)
{
    /* Explicit standby alone may pause advertising or disconnect a phone. */
}

static bool app_ble_wifi_result_is_failure(sys_wifi_connect_result_t result)
{
    return result == SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP ||
           result == SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD ||
           result == SYS_WIFI_CONNECT_RESULT_FAIL_IP ||
           result == SYS_WIFI_CONNECT_RESULT_FAIL_OTHER;
}

static void app_ble_netcfg_connect_task(void *arg)
{
    app_ble_netcfg_connect_ctx_t *ctx = (app_ble_netcfg_connect_ctx_t *)arg;
    sys_network_status_t network = {0};
    sys_wifi_connect_result_t result = SYS_WIFI_CONNECT_RESULT_FAIL_OTHER;
    bool credentials_saved_only = false;
    bool connected = false;
    bool portal_was_active = false;

    if (ctx == NULL) {
        s_ble_transaction_busy = false;
        s_ble_netcfg_connect_pending = false;
        vTaskDelete(NULL);
        return;
    }

    /* The V2.1 board has one Wi-Fi VIF. The web portal may advertise at the
     * same time as BLE, but it must release SoftAP before BLE credentials can
     * be applied in STA mode. */
    if (app_netcfg_portal_is_active()) {
        portal_was_active = true;
        LISA_LOGI(TAG, "BLE netcfg selected, stop web portal before STA connect");
        if (app_netcfg_portal_stop() != 0) {
            LISA_LOGW(TAG, "BLE netcfg could not stop web portal");
            goto done;
        }
    }

    if (sys_network_get_status(&network) == 0 &&
        network.active_bearer == SYS_NETWORK_BEARER_MODEM) {
        credentials_saved_only = true;
    }

    sys_wifi_reset_connect_result();
    if (netcfg_ble_notify_wifi(&ctx->data) != NETCFG_BLE_SUCCESS) {
        result = sys_wifi_get_connect_result();
        goto done;
    }
    s_ble_transaction_busy = false; /* Association/DHCP waiting is not a mode transaction. */

    if (credentials_saved_only) {
        LISA_LOGI(TAG, "BLE netcfg saved WiFi credentials while modem bearer is active");
        connected = true;
        goto done;
    }

    for (uint32_t waited = 0; waited < APP_BLE_NETCFG_CONNECT_TIMEOUT_MS;
         waited += APP_BLE_NETCFG_CONNECT_POLL_MS) {
        if (sys_wifi_is_connected()) {
            connected = true;
            break;
        }

        result = sys_wifi_get_connect_result();
        if (app_ble_wifi_result_is_failure(result)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_BLE_NETCFG_CONNECT_POLL_MS));
    }

done:
    s_ble_transaction_busy = false;
    if (connected) {
        app_ble_netcfg_complete();
        LISA_LOGI(TAG, "BLE netcfg completed successfully");
        netcfg_bles_set_state(NETCFG_BLE_SUCCESS);
        if (s_ble_connected && s_ble_conidx == ctx->conidx && s_ble_generation == ctx->generation) {
            lisa_ble_netcfg_send_notify(ctx->conidx, 0, 0, 0, NULL);
            /* Keep the legacy success notification, then send a structured
             * result that also gives the provisioning client ChipID and SN. */
            app_ble_notify_enqueue_at(ctx->conidx, APP_BLE_NOTIFY_CONNECTED, 0, false, ctx->generation);
        }
        voice_msg_pub(VOICE_MSG_BLE_CONNECT_DONE, NULL, 0);
    } else {
        result = sys_wifi_get_connect_result();
        if (!app_ble_wifi_result_is_failure(result)) {
            result = SYS_WIFI_CONNECT_RESULT_FAIL_OTHER;
        }
        LISA_LOGW(TAG, "BLE netcfg failed, result=%u", (unsigned)result);
        netcfg_bles_set_state(NETCFG_BLE_ERR);
        if (s_ble_connected && s_ble_conidx == ctx->conidx && s_ble_generation == ctx->generation) {
            app_ble_notify_enqueue_at(ctx->conidx, APP_BLE_NOTIFY_FAILURE, (uint8_t)result,
                                      false, ctx->generation);
        } else if (!s_ble_netcfg_paused) {
            lisa_ble_adv_start(0, LISA_BLE_ADV_GEN);
        }
        if (portal_was_active && !s_ble_netcfg_paused) {
            int portal_ret = app_netcfg_portal_start(APP_NETCFG_PORTAL_REASON_USER_REQUEST);
            if (portal_ret != 0 && portal_ret != -EALREADY) {
                LISA_LOGW(TAG, "failed to restore web portal after BLE failure: %d", portal_ret);
            }
        }
    }

    memset(ctx->data.pwd, 0, sizeof(ctx->data.pwd));
    vPortFree(ctx);
    s_ble_netcfg_connect_pending = false;
    vTaskDelete(NULL);
}

static uint16_t app_ble_netcfg_connect_async(uint8_t conidx, const struct netcfg_ble_data *data)
{
    app_ble_netcfg_connect_ctx_t *ctx;

    if (data == NULL || s_ble_netcfg_connect_pending || s_ble_netcfg_paused || s_ble_scan_running) {
        return NETCFG_BLE_ERR;
    }

    ctx = pvPortMalloc(sizeof(*ctx));
    if (ctx == NULL) {
        return NETCFG_BLE_ERR;
    }

    ctx->conidx = conidx;
    ctx->generation = s_ble_generation;
    memcpy(&ctx->data, data, sizeof(ctx->data));
    taskENTER_CRITICAL();
    bool blocked = s_ble_netcfg_paused || s_ble_netcfg_connect_pending || s_ble_scan_running;
    if (!blocked) {
        s_ble_netcfg_connect_pending = true;
        s_ble_transaction_busy = true;
    }
    taskEXIT_CRITICAL();
    if (blocked) {
        memset(ctx->data.pwd, 0, sizeof(ctx->data.pwd));
        vPortFree(ctx);
        return NETCFG_BLE_ERR;
    }
    if (xTaskCreate(app_ble_netcfg_connect_task, "ble_wifi_cfg", 3072, ctx, 5, NULL) != pdPASS) {
        memset(ctx->data.pwd, 0, sizeof(ctx->data.pwd));
        vPortFree(ctx);
        s_ble_netcfg_connect_pending = false;
        s_ble_transaction_busy = false;
        return NETCFG_BLE_ERR;
    }

    return NETCFG_BLE_INPROCESS;
}

static int get_current_product_id(char *product_id, int max_len)
{
    if (!product_id || max_len <= 0)
        return -1;

    char *pid = NULL;
    int ret = lisa_kv_get_string(KV_KEY_USER_PID, &pid);

    if (ret != 0 || pid == NULL) {
        if (strlen(PRODUCT_ID) >= max_len)
            return -1;
        strcpy(product_id, PRODUCT_ID);
        return 0;
    }

    if (strlen(pid) >= max_len) {
        lisa_kv_free(pid);
        return -1;
    }
    strcpy(product_id, pid);
    lisa_kv_free(pid);
    return 0;
}

int get_current_device_id(char *device_id, int max_len)
{
    if (!device_id || max_len <= 0) {
        return -1;
    }
    char *kv_device_id = NULL;
    int ret = lisa_kv_get_string(KV_KEY_USER_DEVICE_ID, &kv_device_id);

    if (ret == 0 && kv_device_id != NULL && strlen(kv_device_id) > 0) {
        if (strlen(kv_device_id) >= max_len) {
            lisa_kv_free(kv_device_id);
            return -1;
        }
        strcpy(device_id, kv_device_id);
        lisa_kv_free(kv_device_id);
        return 0;
    } else {
        uint32_t *id_1 = (uint32_t *)0x48600208;
        uint32_t *id_2 = (uint32_t *)0x4860020c;
        uint8_t id_buffer[8];

        if (*id_1 == 0 && *id_2 == 0) {
            return -1;
        }

        if (max_len < 17) {
            return -1;
        }

        memcpy(id_buffer, id_1, sizeof(uint32_t));
        memcpy(id_buffer + 4, id_2, sizeof(uint32_t));

        snprintf(device_id, max_len, "%02x%02x%02x%02x%02x%02x%02x%02x",
                id_buffer[0], id_buffer[1], id_buffer[2], id_buffer[3],
                id_buffer[4], id_buffer[5], id_buffer[6], id_buffer[7]);

        return 0;
    }
}

static bool app_ble_tx_valid(uint8_t conidx)
{
    return s_ble_connected && s_ble_subscribed && !s_ble_netcfg_paused &&
           s_ble_conidx == conidx && s_ble_generation == s_ble_tx_generation;
}

/* Called only by the single notification worker. Identity, list pages and
 * failures cannot interleave their custom frames, even across GATT requests. */
static bool app_ble_send_payload(uint8_t conidx, const uint8_t *payload, size_t length)
{
    if (payload == NULL || length == 0 || length > 17U * 255U) {
        return false;
    }
    uint8_t total = (length + 16U) / 17U;
    for (size_t offset = 0; offset < length; offset += 17U) {
        uint8_t frame[20];
        size_t chunk = length - offset < 17U ? length - offset : 17U;
        frame[0] = offset / 17U + 1U;
        frame[1] = total;
        frame[2] = chunk;
        memcpy(frame + 3, payload + offset, chunk);
        if (!app_ble_tx_valid(conidx) ||
            ble_netcfg_bles_send_notify_raw(conidx, chunk + 3U, frame) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    return true;
}

static void app_ble_send_device_info_now(uint8_t conidx, const char *event)
{
    const char *chip_id = app_chip_id_str_get();
    const char *sn = "";
    char payload[384] = {0};

    if (chip_id == NULL || chip_id[0] == '\0') {
        chip_id = "0000000000000000";
    }
    if (lingxin_server_config_ready()) {
        sn = lingxin_server_config_sn();
    }

    if (sn == NULL) {
        sn = "";
    }
    cJSON *identity = cJSON_CreateObject();
    const char *keys[] = {"code", "event", "chip_id", "sn", "firmware_version"};
    const char *values[] = {NULL, event != NULL ? event : "device_info", chip_id, sn, PROJECT_VERSION_STR};
    bool encoded = identity != NULL;
    for (size_t i = 0; encoded && i < sizeof(keys) / sizeof(keys[0]); ++i) {
        cJSON *item = i == 0 ? cJSON_CreateNumber(0) : cJSON_CreateString(values[i]);
        if (item == NULL) {
            encoded = false;
            break;
        }
        /* The engine header declares AddItemToObject as void. Check ownership
         * explicitly so allocation failures cannot send incomplete identities. */
        cJSON_AddItemToObject(identity, keys[i], item);
        if (cJSON_GetObjectItem(identity, keys[i]) != item) {
            cJSON_Delete(item);
            encoded = false;
        }
    }
    encoded = encoded && cJSON_PrintPreallocated(identity, payload, sizeof(payload), false);
    cJSON_Delete(identity);
    if (!encoded) {
        LISA_LOGE(TAG, "BLE identity serialization failed; no truncated payload sent");
        return;
    }
    LISA_LOGI(TAG, "BLE identity event=%s chip_id=%s sn=%s",
              event != NULL ? event : "device_info", chip_id, sn[0] != '\0' ? sn : "(unset)");
    app_ble_send_payload(conidx, (uint8_t *)payload, strlen(payload));
}

static void app_ble_send_device_info(uint8_t conidx, const char *event)
{
    app_ble_notify_enqueue(conidx, strcmp(event, "wifi_connected") == 0 ?
                           APP_BLE_NOTIFY_CONNECTED : APP_BLE_NOTIFY_IDENTITY, 0, false);
}

static void app_ble_send_auth_now(uint8_t conidx)
{
    char product_id[64] = {0};
    char device_id[32] = {0};
    char auth_info[256] = {0};
    if (get_current_product_id(product_id, sizeof(product_id)) != 0) {
        strcpy(product_id, "00000000-0000-0000-0000-000000000000");
    }
    if (get_current_device_id(device_id, sizeof(device_id)) != 0) {
        strcpy(device_id, "0000000000000000");
    }
    snprintf(auth_info, sizeof(auth_info),
             "{\"code\":0,\"product_id\":\"%s\",\"device_id\":\"%s\"}", product_id, device_id);
    app_ble_send_payload(conidx, (uint8_t *)auth_info, strlen(auth_info));
    voice_msg_pub(VOICE_MSG_BLE_AUTH_INFO_DONE, NULL, 0);
}

static void app_ble_send_scan_now(uint8_t conidx, bool refresh)
{
    static uint32_t scan_id;
    static const char *const status_names[] = {"fresh", "cached", "busy", "failed", "paused"};
    app_netcfg_wifi_ap_t aps[APP_NETCFG_WIFI_AP_MAX];
    int count = 0;
    int status;
    taskENTER_CRITICAL();
    bool allowed = app_ble_tx_valid(conidx) && s_ble_netcfg_active && !s_ble_netcfg_connect_pending;
    s_ble_scan_running = allowed;
    taskEXIT_CRITICAL();
    status = app_netcfg_portal_wifi_list(refresh && allowed, aps, APP_NETCFG_WIFI_AP_MAX, &count);
    if (!allowed) {
        status = APP_NETCFG_WIFI_BUSY;
    }
    s_ble_scan_running = false;
    const uint32_t id = ++scan_id;
    const int pages = count > 0 ? (count + 5) / 6 : 1;
    for (int page = 0; page < pages && app_ble_tx_valid(conidx); page++) {
        cJSON *root = cJSON_CreateObject();
        cJSON *list = root != NULL ? cJSON_CreateArray() : NULL;
        bool ok = root && list;
        if (ok) {
            cJSON_AddItemToObject(root, "aps", list);
            cJSON_AddStringToObject(root, "event", "wifi_scan");
            cJSON_AddNumberToObject(root, "scan_id", id);
            cJSON_AddNumberToObject(root, "page", page + 1);
            cJSON_AddNumberToObject(root, "pages", pages);
            cJSON_AddNumberToObject(root, "code", status == APP_NETCFG_WIFI_FAILED ? 1 : 0);
            cJSON_AddStringToObject(root, "status", status_names[status]);
            cJSON_AddStringToObject(root, "band", "2.4GHz");
            ok = cJSON_GetArraySize(root) == 8;
        }
        for (int i = page * 6; ok && i < count && i < (page + 1) * 6; i++) {
            cJSON *ap = cJSON_CreateObject();
            if (ap == NULL) {
                ok = false;
                break;
            }
            cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(ap, "secured", aps[i].secured);
            ok = cJSON_GetArraySize(ap) == 3;
            cJSON_AddItemToArray(list, ap);
        }
        char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
        bool sent = json && app_ble_send_payload(conidx, (uint8_t *)json, strlen(json));
        cJSON_free(json);
        cJSON_Delete(root);
        if (!sent) {
            /* An incomplete page is discarded by the client. Allocation or
             * transport failure receives a complete, bounded failure reply. */
            char failure[160];
            snprintf(failure, sizeof(failure),
                     "{\"event\":\"wifi_scan\",\"scan_id\":%lu,\"page\":1,\"pages\":1,"
                     "\"code\":1,\"status\":\"failed\",\"aps\":[]}", (unsigned long)++scan_id);
            app_ble_send_payload(conidx, (uint8_t *)failure, strlen(failure));
            break;
        }
    }
}

static void app_ble_notify_task(void *arg)
{
    (void)arg;
    app_ble_notify_job_t job;
    for (;;) {
        if (xQueueReceive(s_ble_notify_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        taskENTER_CRITICAL();
        s_ble_tx_generation = job.generation;
        bool valid = app_ble_tx_valid(job.conidx);
        s_ble_notify_running = valid;
        taskEXIT_CRITICAL();
        if (!valid) {
            continue;
        }
        switch (job.kind) {
        case APP_BLE_NOTIFY_IDENTITY:
            app_ble_send_device_info_now(job.conidx, "device_info");
            break;
        case APP_BLE_NOTIFY_CONNECTED:
            app_ble_send_device_info_now(job.conidx, "wifi_connected");
            break;
        case APP_BLE_NOTIFY_AUTH:
            app_ble_send_auth_now(job.conidx);
            break;
        case APP_BLE_NOTIFY_SCAN:
            app_ble_send_scan_now(job.conidx, job.refresh);
            break;
        case APP_BLE_NOTIFY_FAILURE: {
            uint8_t payload[] = {0x0A, 0x01, job.reason};
            app_ble_send_payload(job.conidx, payload, sizeof(payload));
            break;
        }
        }
        s_ble_notify_running = false;
    }
}

static bool app_ble_notify_enqueue_at(uint8_t conidx, app_ble_notify_kind_t kind, uint8_t reason,
                                      bool refresh, uint32_t generation)
{
    app_ble_notify_job_t job = {
        .conidx = conidx, .kind = kind, .reason = reason, .refresh = refresh,
    };
    taskENTER_CRITICAL();
    job.generation = generation;
    bool valid = s_ble_connected && s_ble_subscribed && !s_ble_netcfg_paused &&
                 s_ble_conidx == conidx && s_ble_generation == generation;
    taskEXIT_CRITICAL();
    return valid && s_ble_notify_queue != NULL && xQueueSend(s_ble_notify_queue, &job, 0) == pdTRUE;
}

static bool app_ble_notify_enqueue(uint8_t conidx, app_ble_notify_kind_t kind, uint8_t reason, bool refresh)
{
    return app_ble_notify_enqueue_at(conidx, kind, reason, refresh, s_ble_generation);
}

void netcfg_bles_notify_subscription_changed(uint8_t conidx, bool enabled)
{
    if (!s_ble_connected || s_ble_conidx != conidx) {
        return;
    }
    bool was_enabled = s_ble_subscribed;
    s_ble_subscribed = enabled;
    if (!enabled) {
        s_ble_generation++;
    } else if (!was_enabled) {
        app_ble_notify_enqueue(conidx, APP_BLE_NOTIFY_IDENTITY, 0, false);
        app_ble_notify_enqueue(conidx, APP_BLE_NOTIFY_SCAN, 0, true);
    }
}

uint16_t netcfg_bles_profile_set_cb(uint8_t conidx, uint8_t att_idx, uint16_t op, uint8_t *p_value)
{
    uint16_t status = NETCFG_BLE_ERR;
    LISA_LOGI(TAG, "netcfg_bles_profile_set_cb op: 0x%04X", op);

    switch (op) {
    case NETCFG_BLE_OP_SSID:
    case NETCFG_BLE_OP_PWD:
        break;
    case NETCFG_BLE_OP_SKIP_WIFI:
        LISA_LOGI(TAG, "Skip Wi-Fi connect by BLE netcfg command");
        lisa_ble_netcfg_send_notify(conidx, 0, 0, 0, NULL);
        app_ble_netcfg_complete();
        status = NETCFG_BLE_SUCCESS;
        break;
    case NETCFG_BLE_OP_DONE:
        if (netcfg_ble_data_is_empty((struct netcfg_ble_data *)p_value)) {
            LISA_LOGI(TAG, "Skip Wi-Fi connect for empty BLE netcfg data");
            lisa_ble_netcfg_send_notify(conidx, 0, 0, 0, NULL);
            status = NETCFG_BLE_SUCCESS;
            break;
        }
        status = app_ble_netcfg_connect_async(conidx, (struct netcfg_ble_data *)p_value);
        if (status == NETCFG_BLE_ERR) {
            app_ble_send_wifi_provision_fail(conidx, SYS_WIFI_CONNECT_RESULT_FAIL_OTHER);
        }
        break;
    case NETCFG_BLE_OP_REBOOT:
        ble_gap_disconnect(conidx, 0x13);
        break;
    case NETCFG_BLE_AUTH_INFO:
        status = app_ble_notify_enqueue(conidx, APP_BLE_NOTIFY_AUTH, 0, false) ?
                 NETCFG_BLE_SUCCESS : NETCFG_BLE_ERR;
        break;
    case NETCFG_BLE_OP_DEVICE_INFO:
        LISA_LOGI(TAG, "receive device info request");
        app_ble_send_device_info(conidx, "device_info");
        status = NETCFG_BLE_SUCCESS;
        break;
    case NETCFG_BLE_OP_WIFI_SCAN:
        status = app_ble_notify_enqueue(conidx, APP_BLE_NOTIFY_SCAN, 0, true) ?
                 NETCFG_BLE_SUCCESS : NETCFG_BLE_ERR;
        break;
    default:
        break;
    }
    return status;
}

/*
 * BLE netcfg init - register WiFi connect handler
 */

static int netcfg_wifi_connect_handler(const char *ssid, const char *pwd)
{
    LISA_LOGI(TAG, "netcfg wifi apply ssid:%s", ssid);

    int result = sys_network_connect_wifi(ssid, pwd, NULL);

    LISA_LOGI(TAG, "netcfg wifi apply result:%d", result);
    return result;
}

static void app_ble_send_wifi_provision_fail(uint8_t conidx, uint8_t reason)
{
    app_ble_notify_enqueue(conidx, APP_BLE_NOTIFY_FAILURE, reason, false);
}

void app_ble_netcfg_init(void)
{
    s_ble_connected = false;
    s_ble_conidx = 0;
    s_ble_netcfg_connect_pending = false;
    s_ble_netcfg_active = false;
    s_ble_notify_queue = xQueueCreate(8, sizeof(app_ble_notify_job_t));
    if (s_ble_notify_queue != NULL &&
        xTaskCreate(app_ble_notify_task, "ble_notify", 3072, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_ble_notify_queue);
        s_ble_notify_queue = NULL;
    }
    if (s_ble_notify_queue == NULL) {
        LISA_LOGE(TAG, "BLE notification worker unavailable");
    }
    lisa_ble_register_conn_cb(app_ble_on_connected);
    lisa_ble_register_disc_cb(app_ble_on_disconnected);
    lisa_ble_register_bond_cb(app_ble_on_bond);
    lisa_ble_netcfg_set_handler(netcfg_wifi_connect_handler);
}
