import unittest
from tools import test_offline_action as support


class PushupTtsCancellationTest(unittest.TestCase):
    run_c = support.OfflineActionTest.run_c

    def test_owner_change_aborts_readiness_wait_before_next_download(self):
        source = (support.ROOT / 'apps/arcs-mini/voice_player.c').read_text(encoding='utf-8')
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define PUSHUP_TTS_DOWNLOAD_MAX_BYTES 64
#define PUSHUP_TTS_READY_MAX_ATTEMPTS 21
#define PUSHUP_TTS_READY_STABLE_COUNT 2
#define PUSHUP_TTS_READY_POLL_MS 1000
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#define pdMS_TO_TICKS(n) (n)
#define psram_malloc malloc
#define psram_free free
static unsigned owner=7,downloads;
static bool service_offline_action_online_current(uint32_t generation) {return generation==owner;}
static bool voice_player_tts_url_is_latest(const char *url) {return true;}
static int voice_player_download_pushup_tts_mp3(const char *url,uint8_t *buf,size_t *len,uint32_t generation) {
 downloads++;*len=32;return 0;
}
static void vTaskDelay(unsigned ms) {owner++;}
''' + support.c_function(source, 'voice_player_wait_pushup_tts_mp3_ready') + r'''
int main(void) {
 assert(voice_player_wait_pushup_tts_mp3_ready("test.mp3",7)==1);
 assert(downloads==1);
 assert(voice_player_wait_pushup_tts_mp3_ready("test.mp3",7)==1);
 assert(downloads==1);
 return 0;
}
''')
