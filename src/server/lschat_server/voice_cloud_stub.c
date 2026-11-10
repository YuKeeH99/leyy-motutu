#define TAG "voice.cloud.stub"

#include <stddef.h>
#include <stdint.h>

#include "voice_cloud.h"
#include "lisa_log.h"

static volatile uint8_t s_warned_connect;
static volatile uint8_t s_warned_chat;
static volatile uint8_t s_warned_audio;
static volatile uint8_t s_warned_tts;

static void voice_cloud_stub_warn(uint8_t *flag, const char *msg)
{
    if (*flag) {
        return;
    }

    *flag = 1U;
    LOGW("%s", msg);
}

int voice_cloud_connect(struct voice_cloud_connect_config *config)
{
    (void)config;
    voice_cloud_stub_warn((uint8_t *)&s_warned_connect,
                          "LSChat stub active in Lingxin build: connect ignored");
    return -1;
}

int voice_cloud_disconnect(void)
{
    return 0;
}

int voice_cloud_chat_start(struct voice_cloud_chat_config *config)
{
    (void)config;
    voice_cloud_stub_warn((uint8_t *)&s_warned_chat,
                          "LSChat stub active in Lingxin build: chat start ignored");
    return -1;
}

int voice_cloud_chat_stop(void)
{
    return 0;
}

int voice_cloud_chat_send_audio(uint8_t *data, int len)
{
    (void)data;
    (void)len;
    voice_cloud_stub_warn((uint8_t *)&s_warned_audio,
                          "LSChat stub active in Lingxin build: audio upload ignored");
    return -1;
}

int voice_cloud_image_recognition(uint8_t *jpg_image, uint32_t len)
{
    (void)jpg_image;
    (void)len;
    return -1;
}

void voice_cloud_image_recognition_drop_pending_result(void)
{
}

int voice_cloud_audio_recognition_start(void)
{
    return -1;
}

int voice_cloud_audio_recognition_stop(void)
{
    return 0;
}

int voice_cloud_upload_audio_pause(void)
{
    return 0;
}

int voice_cloud_upload_audio_resume(void)
{
    return 0;
}

int voice_cloud_upload_jpeg_img(const uint8_t *jpeg_data, size_t jpeg_size, char **url_out)
{
    (void)jpeg_data;
    (void)jpeg_size;
    if (url_out != NULL) {
        *url_out = NULL;
    }
    return -1;
}

void voice_cloud_jpeg_img_url_free(void *url)
{
    (void)url;
}

int voice_cloud_tts_synth(const char *txt)
{
    (void)txt;
    voice_cloud_stub_warn((uint8_t *)&s_warned_tts,
                          "LSChat stub active in Lingxin build: TTS synth ignored");
    return -1;
}

int voice_cloud_is_connected(void)
{
    return 0;
}

voice_cloud_state_t voice_cloud_get_state(void)
{
    return VOICE_CLOUD_STATE_CONNECT_FAILED;
}

int voice_cloud_is_session_active(void)
{
    return 0;
}

int voice_cloud_is_uploading_audio(void)
{
    return 0;
}
