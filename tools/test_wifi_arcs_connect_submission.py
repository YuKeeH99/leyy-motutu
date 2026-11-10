"""Compile and execute the actual ARCS connect function against HAL/RTOS seams."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "arcs-sdk/modules/wifi_manager"
BIN = Path(os.environ.get("NUCLEI_TOOLCHAIN_PATH", Path.home() / ".listenai/gcc")) / "bin"
EXT = ".exe" if os.name == "nt" else ""


class ArcsConnectSubmissionTest(unittest.TestCase):
    def test_rejected_submission_never_waits_for_a_terminal_event(self):
        source = (MODULE / "port/wifi_ops/arcs/wifi_arcs.c").read_text(encoding="utf-8")
        start = source.index("int arcs_wifi_sta_connect(")
        end = source.index("int arcs_wifi_sta_disconnect(", start)
        program = r'''
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "wifi_manager/wifi_manager_wifi_ops.h"
#include "wifi_api.h"
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGD(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define GROUP_EVENTS_WIFI_STA_CONNECT_FINISH 1U
#define GROUP_EVENTS_WIFI_STA_CONNECT_FAILED 2U
typedef uint32_t EventBits_t;
static int wifi_events, connect_retry = 2;
static bool s_sta_connected, allocation_fails;
static wifi_mgr_wifi_sta_config_t s_sta_config;
static unsigned waits, submits, frees, clears;
static int submit_result;
static EventBits_t completed_bits;
static uint32_t waited_ms;
static void *allocation;
static void *test_alloc(size_t size) {
    assert(allocation == NULL);
    if (allocation_fails) return NULL;
    allocation = malloc(size); assert(allocation); return allocation;
}
static void test_free(void *ptr) {
    assert(ptr == allocation && allocation != NULL); frees++;
    free(ptr); allocation = NULL;
}
#define PLATFORM_MEM_NOCACHE_MALLOC(size) test_alloc(size)
#define PLATFORM_MEM_FREE(ptr) test_free(ptr)
static wifi_security_e convert_to_ls_wifi_type(wifi_mgr_wifi_encryption_mode_t mode) {
    (void)mode; return WIFI_SEC_AUTO;
}
static uint16_t arcs_wifi_channel_to_freq(int channel) { (void)channel; return 0; }
static void arcs_wifi_parse_bssid(const char *src, uint8_t *out) { (void)src; (void)out; }
ls_err_t wifi_set_pmk(const uint8_t *pmk) { (void)pmk; return LS_OK; }
ls_err_t wifi_sta_connect(wifi_connect_cfg_t *cfg) {
    assert(cfg == allocation); assert(!strcmp((char *)cfg->ssid, "saved"));
    submits++; return submit_result;
}
static EventBits_t xEventGroupClearBits(int group, EventBits_t bits) {
    (void)group; assert(bits == 3); clears++; return 0;
}
static EventBits_t xEventGroupWaitBits(int group, EventBits_t bits, bool clear, bool all, uint32_t ms) {
    (void)group; assert(bits == 3 && clear && !all);
    waits++; waited_ms = ms; return completed_bits;
}
'''
        program += source[start:end]
        program += r'''
static void reset(void) {
    assert(allocation == NULL);
    waits = submits = frees = clears = 0; waited_ms = 0;
    submit_result = LS_OK; completed_bits = 0;
    s_sta_connected = false; allocation_fails = false;
}
int main(void) {
    wifi_mgr_wifi_sta_config_t ap = {.ssid = "saved", .pwd = "password"};
    const uint32_t timeouts[] = {0, 15000};
    for (unsigned i = 0; i < 2; i++) {
        reset(); submit_result = -1;
        assert(arcs_wifi_sta_connect(&ap, timeouts[i]) == -EIO);
        assert(submits == 1 && waits == 0 && frees == 1 && clears == 1);
    }
    reset(); /* Async accepted: submission succeeds without waiting. */
    assert(arcs_wifi_sta_connect(&ap, 0) == 0);
    assert(submits == 1 && waits == 0 && frees == 1);
    reset(); completed_bits = 1; s_sta_connected = true;
    assert(arcs_wifi_sta_connect(&ap, 15000) == 0);
    assert(waits == 1 && waited_ms == 15000 && frees == 1);
    reset(); completed_bits = 2;
    assert(arcs_wifi_sta_connect(&ap, 15000) == -EIO);
    assert(waits == 1 && frees == 1);
    reset();
    assert(arcs_wifi_sta_connect(&ap, 15000) == -ETIMEDOUT);
    assert(waits == 1 && frees == 1);
    reset(); completed_bits = 1;
    assert(arcs_wifi_sta_connect(&ap, 15000) == -ECONNRESET);
    assert(waits == 1 && frees == 1);
    reset(); allocation_fails = true;
    assert(arcs_wifi_sta_connect(&ap, 15000) == -ENOMEM);
    assert(submits == 0 && waits == 0 && frees == 0);
    puts("PASS: 8 ARCS connection submission/completion scenarios");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cfile = Path(directory) / "arcs_connect.c"
            elf = Path(directory) / "arcs_connect.elf"
            cfile.write_text(program, encoding="utf-8")
            subprocess.run([str(BIN / ("riscv64-unknown-elf-gcc" + EXT)),
                            "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(MODULE / "include"),
                            "-I", str(MODULE / "test/common/mock_wifi_api"),
                            str(cfile), "-o", str(elf)], check=True)
            subprocess.run([str(BIN / ("riscv64-unknown-elf-run" + EXT)), str(elf)],
                           check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
