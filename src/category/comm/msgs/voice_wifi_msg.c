#include "stdint.h"
#include "stddef.h"

#include "lisa_log.h"
#include "voice_msg.h"
#include "sys_init.h"

#include "app_datas.h"

#define TAG "voice.app.wifi"

static void voice_wifi_ip_got(void *unused, uint32_t evt, void *data, uint32_t len, void *user_data)
{
    LOGI("voice_wifi_ip_got");
    struct app_datas *app_datas = get_app_datas();
    assert(app_datas != NULL);

    app_datas->wifi_connected = 1;
}

static void voice_wifi_disconnected(void *unused, uint32_t evt, void *data, uint32_t len, void *user_data)
{
    LOGI("voice_wifi_disconnected");
    struct app_datas *app_datas = get_app_datas();
    assert(app_datas != NULL);

    app_datas->wifi_connected = 0;
#ifdef CONFIG_VOICE_LINK_LINGXIN
    /* The Lingxin WebSocket is tied to the bearer.  Clear the cached cloud
     * flag so the next IP/probe event performs a real reconnect instead of
     * reporting a stale initialized state. */
    app_datas->voice_cloud_connected = 0;
    LOGI("[Lingxin] clear cloud connected flag after WiFi loss");
#endif
}

int voice_wifi_evt_init(void)
{
    LOGI("voice_wifi_evt_init");

    voice_msg_sub(VOICE_MSG_WIFI_IP_GOT, voice_wifi_ip_got, NULL);
    voice_msg_sub(VOICE_MSG_WIFI_DISCONNECTED, voice_wifi_disconnected, NULL);

    return 0;
}

SYS_INIT(voice_wifi_evt_init, SYS_INIT_LEVEL_PRE_APPLICATION, 50);
