"""Execute actual portal scan functions with the Wi-Fi driver boundary faked."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = Path(os.environ.get('NUCLEI_TOOLCHAIN_PATH', Path.home() / '.listenai/gcc')) / 'bin'
SUFFIX = '.exe' if os.name == 'nt' else ''


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


class FirmwareTests(unittest.TestCase):
    def test_result_job_cannot_be_relabelled_for_a_new_connection(self):
        source = (ROOT / 'src/middleware/ble/app_ble_common.c').read_text(encoding='utf-8')
        self.assertTrue('static bool app_ble_notify_enqueue_at(' in source,
                        'Result queue must retain the credential connection generation')
        # The source also declares this function; select its definition.
        body = function(source[source.index('static bool app_ble_notify_enqueue_at(',
                        source.index('static bool app_ble_notify_enqueue_at(') + 1):],
                        'static bool app_ble_notify_enqueue_at(')
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define pdTRUE 1
typedef int app_ble_notify_kind_t;
typedef struct {uint32_t generation; uint8_t conidx; int kind; uint8_t reason; bool refresh;} app_ble_notify_job_t;
static bool s_ble_connected = true, s_ble_subscribed = true, s_ble_netcfg_paused;
static uint32_t s_ble_generation = 9;
static uint8_t s_ble_conidx = 1;
static void *s_ble_notify_queue = (void *)1;
static int queued;
static int xQueueSend(void *q, const app_ble_notify_job_t *job, int wait) {
 (void)q; (void)wait; assert(job->generation == 9); queued++; return 1;
}
'''
        main = r'''
int main(void) {
 assert(!app_ble_notify_enqueue_at(1, 1, 0, false, 8)); assert(queued == 0);
 assert(app_ble_notify_enqueue_at(1, 1, 0, false, 9)); assert(queued == 1);
 s_ble_netcfg_paused = true;
 assert(!app_ble_notify_enqueue_at(1, 1, 0, false, 9)); assert(queued == 1);
 return 0;
}
'''
        self.run_c(harness + body + main)

    def test_notifications_stop_at_disconnect_or_sleep_and_keep_frame_order(self):
        source = (ROOT / 'src/middleware/ble/app_ble_common.c').read_text(encoding='utf-8')
        self.assertTrue('static bool app_ble_send_payload(' in source,
                        'The generation-checked notification sender is missing')
        bodies = '\n'.join(function(source, s) for s in (
            'static bool app_ble_tx_valid(', 'static bool app_ble_send_payload('))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define pdMS_TO_TICKS(x) (x)
static bool s_ble_connected = true, s_ble_subscribed = true, s_ble_netcfg_paused;
static uint8_t s_ble_conidx = 1;
static uint32_t s_ble_generation = 7, s_ble_tx_generation = 7;
static int sends, cancel;
static uint8_t frames[4][20]; static int sizes[4];
static void vTaskDelay(int ms) {(void)ms; if (cancel) s_ble_generation++;}
static uint16_t ble_netcfg_bles_send_notify_raw(uint8_t conidx, uint16_t length, const uint8_t *data) {
 assert(conidx == 1); sizes[sends] = length; memcpy(frames[sends++], data, length); return 0;
}
'''
        main = r'''
int main(void) {
 uint8_t payload[] = "abcdefghijklmnopqrst";
 assert(app_ble_send_payload(1, payload, 20));
 assert(sends == 2 && sizes[0] == 20 && sizes[1] == 6);
 assert(frames[0][0] == 1 && frames[0][1] == 2 && frames[0][2] == 17);
 assert(frames[1][0] == 2 && frames[1][1] == 2 && frames[1][2] == 3);
 assert(!memcmp(frames[1] + 3, "rst", 3));
 sends = 0; cancel = 1;
 assert(!app_ble_send_payload(1, payload, 20)); assert(sends == 1);
 s_ble_generation = s_ble_tx_generation; sends = 0; s_ble_netcfg_paused = true;
 assert(!app_ble_send_payload(1, payload, 20)); assert(sends == 0);
 return 0;
}
'''
        self.run_c(harness + bodies + main)

    def run_c(self, source):
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'runtime.c'
            elf = Path(tmp) / 'runtime.elf'
            c.write_text(source, encoding='utf-8')
            subprocess.run([str(TOOLCHAIN / ('riscv64-unknown-elf-gcc' + SUFFIX)),
                            '--specs=sim.specs', '-std=c11', '-Wall', '-Werror',
                            '-I', str(ROOT / 'src/middleware/netcfg_portal'),
                            str(c), '-o', str(elf)], check=True)
            subprocess.run([str(TOOLCHAIN / ('riscv64-unknown-elf-run' + SUFFIX)), str(elf)], check=True)

    def test_scan_preserves_softap_and_sleep_and_filters_5ghz(self):
        source = (ROOT / 'src/middleware/netcfg_portal/app_netcfg_portal.c').read_text(encoding='utf-8')
        self.assertIn('app_netcfg_portal_wifi_list(', source, 'The shared BLE scan API is not implemented')
        bodies = '\n'.join(function(source, s) for s in (
            'static bool encryption_is_open(', 'static bool sta_up(',
            'static int scan_and_cache(', 'int app_netcfg_portal_wifi_list('))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "app_netcfg_portal.h"
#include "netcfg_portal_priv.h"
#define WIFI_MGR_WIFI_AUTH_OPEN 0
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGI(tag, ...) ((void)printf(__VA_ARGS__))
#define pdMS_TO_TICKS(x) (x)
typedef int wifi_mgr_wifi_encryption_mode_t;
typedef struct {char ssid[32]; char bssid[18]; int channel; int rssi; int encryption_mode;} wifi_mgr_scan_info_t;
static struct {bool inited, active, scan_busy, ap_cache_valid; int ap_count; netcfg_portal_ap_t aps[24];} s_ctx;
static bool suspended, started, sta_enabled = true;
static int scans, scan_result = 4;
static void lock(void) {} static void unlock(void) {}
static void vTaskDelay(int ms) {(void)ms;}
static bool sys_wifi_radio_suspended(void) {return suspended;}
static bool sys_wifi_is_started(void) {return started;}
static bool wifi_mgr_sta_is_enable(void) {return sta_enabled;}
static void wifi_mgr_sta_enable(void) {sta_enabled = true;}
static int wifi_mgr_scan_ap(wifi_mgr_scan_info_t *out, int max, bool async) {
 (void)max; (void)async; scans++;
 wifi_mgr_scan_info_t entries[] = {
  {"Home", "01", 1, -70, 1}, {"Only5G", "02", 36, -10, 1},
  {"Home", "03", 6, -30, 1}, {"Cafe", "04", 11, -40, 0}};
 memcpy(out, entries, sizeof(entries)); return scan_result;
}
'''
        main = r'''
int main(void) {
 app_netcfg_wifi_ap_t aps[24]; int count = -1;
 s_ctx.inited = true;
 assert(app_netcfg_portal_wifi_list(true, aps, 24, &count) == APP_NETCFG_WIFI_FRESH);
 assert(count == 2 && scans == 1);
 assert(!strcmp(aps[0].ssid,"Home") && aps[0].rssi == -30 && aps[0].secured);
 assert(!strcmp(aps[1].ssid,"Cafe") && !aps[1].secured);
 s_ctx.active = true;
 assert(app_netcfg_portal_wifi_list(true, aps, 24, &count) == APP_NETCFG_WIFI_CACHED);
 assert(scans == 1 && count == 2);
 s_ctx.active = false; suspended = true;
 assert(app_netcfg_portal_wifi_list(true, aps, 24, &count) == APP_NETCFG_WIFI_PAUSED);
 assert(scans == 1);
 suspended = false; started = true;
 assert(app_netcfg_portal_wifi_list(true, aps, 24, &count) == APP_NETCFG_WIFI_BUSY);
 assert(scans == 1);
 started = false; scan_result = -1;
 assert(app_netcfg_portal_wifi_list(true, aps, 24, &count) == APP_NETCFG_WIFI_FAILED);
 assert(count == 2 && !strcmp(aps[0].ssid,"Home"));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'scan.c'
            elf = Path(tmp) / 'scan.elf'
            c.write_text(harness + bodies + main, encoding='utf-8')
            subprocess.run([str(TOOLCHAIN / ('riscv64-unknown-elf-gcc' + SUFFIX)),
                            '--specs=sim.specs', '-std=c11', '-Wall', '-Werror',
                            '-I', str(ROOT / 'src/middleware/netcfg_portal'),
                            str(c), '-o', str(elf)], check=True)
            subprocess.run([str(TOOLCHAIN / ('riscv64-unknown-elf-run' + SUFFIX)), str(elf)], check=True)


if __name__ == '__main__':
    unittest.main()
