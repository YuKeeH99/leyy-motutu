#include "lingxin_server_config.h"

#define TAG "lingxin.config"

#include <stdio.h>
#include <string.h>

#include "kv_user.h"
#include "lingxin_identity_priority.h"
#include "lisa_kv.h"
#include "lisa_log.h"

static char s_lingxin_app_id[64];
static char s_lingxin_license[128];
static char s_lingxin_app_code[128];
static char s_lingxin_device_code[128];
static char s_lingxin_sn[128];
static int s_lingxin_config_loaded;
static int s_lingxin_provisioned;

typedef struct {
    char *app_id;
    char *license;
    char *sn;
    char *app_code;
    char *device_code;
} lingxin_stored_bundle_t;

static int lingxin_string_is_empty(const char *value)
{
    return value == NULL || value[0] == '\0';
}

static void lingxin_copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    snprintf(dst, dst_size, "%s", src == NULL ? "" : src);
}

static void lingxin_stored_bundle_load(lingxin_stored_bundle_t *bundle)
{
    memset(bundle, 0, sizeof(*bundle));
    (void)lisa_kv_get_string(KV_KEY_LINGXIN_APP_ID, &bundle->app_id);
    (void)lisa_kv_get_string(KV_KEY_LINGXIN_LICENSE, &bundle->license);
    (void)lisa_kv_get_string(KV_KEY_LINGXIN_SN, &bundle->sn);
    (void)lisa_kv_get_string(KV_KEY_LINGXIN_APP_CODE, &bundle->app_code);
    (void)lisa_kv_get_string(KV_KEY_LINGXIN_DEVICE_CODE, &bundle->device_code);
}

/* These keys existed briefly in development images.  They are never read:
 * remove them at every configuration entry point so only user.lingxin.* can
 * remain authoritative on the device. */
static void lingxin_legacy_bundle_clear(void)
{
    static const char *const legacy_keys[] = {
        "lingxin.app_id",
        "lingxin.license",
        "lingxin.sn",
        "lingxin.app_code",
        "lingxin.device_code",
    };

    for (size_t i = 0; i < sizeof(legacy_keys) / sizeof(legacy_keys[0]); ++i) {
        (void)lisa_kv_del(legacy_keys[i]);
    }
}

static void lingxin_stored_bundle_free(lingxin_stored_bundle_t *bundle)
{
    lisa_kv_free(bundle->app_id);
    lisa_kv_free(bundle->license);
    lisa_kv_free(bundle->sn);
    lisa_kv_free(bundle->app_code);
    lisa_kv_free(bundle->device_code);
    memset(bundle, 0, sizeof(*bundle));
}

static lingxin_identity_bundle_t lingxin_stored_bundle_view(const lingxin_stored_bundle_t *bundle)
{
    lingxin_identity_bundle_t view = {
        .app_id = bundle->app_id,
        .license = bundle->license,
        .sn = bundle->sn,
        .app_code = bundle->app_code,
        .device_code = bundle->device_code,
    };
    return view;
}

static int lingxin_bundle_lengths_valid(const lingxin_identity_bundle_t *bundle)
{
    return lingxin_identity_bundle_complete(bundle) &&
           strlen(bundle->app_id) < sizeof(s_lingxin_app_id) &&
           strlen(bundle->license) < sizeof(s_lingxin_license) &&
           strlen(bundle->sn) < sizeof(s_lingxin_sn) &&
           strlen(bundle->app_code) < sizeof(s_lingxin_app_code) &&
           strlen(bundle->device_code) < sizeof(s_lingxin_device_code);
}

static void lingxin_bundle_activate(const lingxin_identity_bundle_t *bundle)
{
    lingxin_copy_string(s_lingxin_app_id, sizeof(s_lingxin_app_id), bundle->app_id);
    lingxin_copy_string(s_lingxin_license, sizeof(s_lingxin_license), bundle->license);
    lingxin_copy_string(s_lingxin_sn, sizeof(s_lingxin_sn), bundle->sn);
    lingxin_copy_string(s_lingxin_app_code, sizeof(s_lingxin_app_code), bundle->app_code);
    lingxin_copy_string(s_lingxin_device_code, sizeof(s_lingxin_device_code), bundle->device_code);
}

static void lingxin_bundle_clear(void)
{
    memset(s_lingxin_app_id, 0, sizeof(s_lingxin_app_id));
    memset(s_lingxin_license, 0, sizeof(s_lingxin_license));
    memset(s_lingxin_sn, 0, sizeof(s_lingxin_sn));
    memset(s_lingxin_app_code, 0, sizeof(s_lingxin_app_code));
    memset(s_lingxin_device_code, 0, sizeof(s_lingxin_device_code));
    s_lingxin_provisioned = 0;
}

static int lingxin_primary_bundle_matches(const lingxin_identity_bundle_t *expected)
{
    lingxin_stored_bundle_t stored;
    lingxin_stored_bundle_load(&stored);
    lingxin_identity_bundle_t actual = lingxin_stored_bundle_view(&stored);
    int matches = lingxin_bundle_lengths_valid(&actual) &&
                  strcmp(actual.app_id, expected->app_id) == 0 &&
                  strcmp(actual.license, expected->license) == 0 &&
                  strcmp(actual.sn, expected->sn) == 0 &&
                  strcmp(actual.app_code, expected->app_code) == 0 &&
                  strcmp(actual.device_code, expected->device_code) == 0;
    lingxin_stored_bundle_free(&stored);
    return matches;
}

/* The marker is deliberately written last. A power loss can leave incomplete
 * strings, but can never make a partial identity look production-ready. */
static int lingxin_primary_bundle_persist(const lingxin_identity_bundle_t *bundle)
{
    (void)lisa_kv_set_int(KV_KEY_LINGXIN_PROVISIONED, 0);
    if (lisa_kv_set_string(KV_KEY_LINGXIN_APP_ID, bundle->app_id) != 0 ||
        lisa_kv_set_string(KV_KEY_LINGXIN_LICENSE, bundle->license) != 0 ||
        lisa_kv_set_string(KV_KEY_LINGXIN_SN, bundle->sn) != 0 ||
        lisa_kv_set_string(KV_KEY_LINGXIN_APP_CODE, bundle->app_code) != 0 ||
        lisa_kv_set_string(KV_KEY_LINGXIN_DEVICE_CODE, bundle->device_code) != 0) {
        LOGE("failed to persist complete Lingxin factory bundle");
        return -1;
    }
    if (!lingxin_primary_bundle_matches(bundle)) {
        LOGE("Lingxin factory bundle read-back verification failed");
        return -1;
    }
    if (lisa_kv_set_int(KV_KEY_LINGXIN_PROVISIONED, 1) != 0) {
        LOGE("failed to commit Lingxin provisioned marker");
        return -1;
    }
    return 0;
}

static void lingxin_server_config_load(void)
{
    lingxin_stored_bundle_t primary;
    int marker = 0;

    if (s_lingxin_config_loaded) {
        return;
    }
    lingxin_bundle_clear();
    lingxin_legacy_bundle_clear();

    (void)lisa_kv_get_int(KV_KEY_LINGXIN_PROVISIONED, &marker);
    lingxin_stored_bundle_load(&primary);
    lingxin_identity_bundle_t primary_view = lingxin_stored_bundle_view(&primary);
    lingxin_identity_source_t source =
        lingxin_identity_bundle_source(marker == 1, &primary_view);

    if (source == LINGXIN_IDENTITY_SOURCE_FACTORY && lingxin_bundle_lengths_valid(&primary_view)) {
        lingxin_bundle_activate(&primary_view);
        s_lingxin_provisioned = 1;
    } else {
        LOGW("Lingxin factory bundle unavailable; cloud requests are blocked");
    }

    LOGI("Lingxin identity source=%s provisioned=%d",
         lingxin_identity_source_name(source), s_lingxin_provisioned);
    lingxin_stored_bundle_free(&primary);
    s_lingxin_config_loaded = 1;
}

const char *lingxin_server_config_cloud_host(void) { return CONFIG_LINGXIN_CLOUD_HOST; }
int lingxin_server_config_cloud_port(void) { return CONFIG_LINGXIN_CLOUD_PORT; }
const char *lingxin_server_config_app_id(void) { lingxin_server_config_load(); return s_lingxin_app_id; }
const char *lingxin_server_config_license(void) { lingxin_server_config_load(); return s_lingxin_license; }
const char *lingxin_server_config_app_code(void) { lingxin_server_config_load(); return s_lingxin_app_code; }
const char *lingxin_server_config_device_code(void) { lingxin_server_config_load(); return s_lingxin_device_code; }
const char *lingxin_server_config_sn(void) { lingxin_server_config_load(); return s_lingxin_sn; }

int lingxin_server_config_set(const char *app_id, const char *license, const char *sn,
                              const char *app_code, const char *device_code)
{
    const lingxin_identity_bundle_t bundle = {app_id, license, sn, app_code, device_code};
    if (!lingxin_bundle_lengths_valid(&bundle)) {
        LOGE("invalid or incomplete Lingxin factory bundle");
        return -1;
    }
    lingxin_legacy_bundle_clear();
    if (lingxin_primary_bundle_persist(&bundle) != 0) {
        lingxin_bundle_clear();
        s_lingxin_config_loaded = 1;
        return -1;
    }
    lingxin_bundle_activate(&bundle);
    s_lingxin_provisioned = 1;
    s_lingxin_config_loaded = 1;
    LOGI("Lingxin factory bundle committed and verified");
    return 0;
}

int lingxin_server_config_clear(void)
{
    /* Remove the commit marker first, so a reset during deletion can never
     * expose a partially deleted identity as provisioned. */
    (void)lisa_kv_del(KV_KEY_LINGXIN_PROVISIONED);
    (void)lisa_kv_del(KV_KEY_LINGXIN_APP_ID);
    (void)lisa_kv_del(KV_KEY_LINGXIN_LICENSE);
    (void)lisa_kv_del(KV_KEY_LINGXIN_SN);
    (void)lisa_kv_del(KV_KEY_LINGXIN_APP_CODE);
    (void)lisa_kv_del(KV_KEY_LINGXIN_DEVICE_CODE);
    lingxin_legacy_bundle_clear();
    lingxin_bundle_clear();
    s_lingxin_config_loaded = 1;
    LOGI("Lingxin factory bundle and obsolete keys cleared");
    return 0;
}

int lingxin_server_config_reload(void)
{
    /* Used by factory tools after a direct raw KV transaction.
     * Normal production writes should use config_set(), which already updates
     * both Flash and these runtime buffers atomically. */
    s_lingxin_config_loaded = 0;
    lingxin_server_config_load();
    if (!lingxin_server_config_ready()) {
        LOGE("Lingxin factory bundle reload failed");
        return -1;
    }
    LOGI("Lingxin factory bundle reloaded without reboot");
    return 0;
}

int lingxin_server_config_fetch(void)
{
    LOGW("remote Lingxin identity fetch disabled; use LifelikeCoreMassTool");
    return -1;
}

int lingxin_server_config_provisioned(void)
{
    lingxin_server_config_load();
    return s_lingxin_provisioned;
}

int lingxin_server_config_ready(void)
{
    lingxin_server_config_load();
    return s_lingxin_provisioned &&
           !lingxin_string_is_empty(s_lingxin_app_id) &&
           !lingxin_string_is_empty(s_lingxin_license) &&
           !lingxin_string_is_empty(s_lingxin_sn) &&
           !lingxin_string_is_empty(s_lingxin_app_code) &&
           !lingxin_string_is_empty(s_lingxin_device_code);
}

void lingxin_server_config_dump(void)
{
    lingxin_server_config_load();
    LOGI("cloud=%s:%d", lingxin_server_config_cloud_host(), lingxin_server_config_cloud_port());
    LOGI("factory_identity=%s", lingxin_server_config_ready() ? "<verified>" : "<missing>");
    LOGI("app_id=%s", lingxin_string_is_empty(s_lingxin_app_id) ? "<empty>" : "<configured>");
    LOGI("app_code=%s", lingxin_string_is_empty(s_lingxin_app_code) ? "<empty>" : "<configured>");
    LOGI("device_code=%s", lingxin_string_is_empty(s_lingxin_device_code) ? "<empty>" : "<configured>");
    LOGI("sn=%s", lingxin_string_is_empty(s_lingxin_sn) ? "<empty>" : "<configured>");
}
