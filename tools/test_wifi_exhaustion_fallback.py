"""Execute the application fallback functions with controlled network state."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("NUCLEI_TOOLCHAIN_PATH", Path.home() / ".listenai/gcc")) / "bin"
EXT = ".exe" if os.name == "nt" else ""


def function(source, name):
    start = source.index("static void " + name + "(")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class WifiExhaustionFallbackTest(unittest.TestCase):
    def run_fallback(self, scenario):
        source = (ROOT / "apps/arcs-mini/main.c").read_text(encoding="utf-8")
        start = source.index("#define WIFI_NETCFG_FALLBACK_")
        globals_code = source[start:source.index("#endif", start)]
        program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define CONFIG_APP_STATIC_WIFI_BOOTSTRAP 0
#define SYS_NETWORK_MODE_WIFI 1
#define APP_NETCFG_PORTAL_REASON_CONNECT_FAILED 1
#define VOICE_MSG_WIFI_IP_GOT 2
#define VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS 3
#define pdMS_TO_TICKS(x) (x)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
typedef uint32_t TickType_t;
typedef struct { int mode; bool wifi_available, switching, wifi_connected; } sys_network_status_t;
static sys_network_status_t net = {1, true, false, false};
static bool portal, asleep, exhausted, network_busy, race_connected, race_busy;
static bool probe_event_valid = true;
static bool s_boot_probe_fail_prompted;
static unsigned portal_calls, ble_calls;
static TickType_t tick = 1;
static TickType_t xTaskGetTickCount(void) { return tick; }
static bool app_netcfg_portal_is_active(void) { return portal; }
static bool service_offline_action_network_busy(void) { return network_busy; }
static int sys_network_get_status(sys_network_status_t *out) { *out = net; return 0; }
static bool sys_wifi_radio_suspended(void) { return asleep; }
static bool sys_wifi_is_connected(void) { return net.wifi_connected; }
static bool sys_wifi_reconnect_saved_exhausted(void) {
 bool result = exhausted && !net.wifi_connected && !asleep;
 if (race_connected) net.wifi_connected = true;
 if (race_busy) network_busy = true;
 return result;
}
static bool service_v2_voice_standby_background_audio_suppressed(void) { return asleep; }
static bool network_probe_event_current(const void *data,uint32_t len,bool connected) {(void)data;(void)len;(void)connected;return probe_event_valid;}
static int app_netcfg_portal_start(int reason) { (void)reason; portal_calls++; portal = true; return 0; }
static void app_ble_netcfg_adv_start_delayed(void) { ble_calls++; }
'''
        program += globals_code
        program += function(source, "voice_system_network_probe_success")
        program += function(source, "app_wifi_netcfg_fallback_disable")
        program += function(source, "app_wifi_netcfg_fallback_poll")
        program += r'''
static void poll(void) { tick += 61000; app_wifi_netcfg_fallback_poll(); }
int main(void) {''' + scenario + r'''
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cfile = Path(directory) / "fallback.c"
            elf = Path(directory) / "fallback.elf"
            cfile.write_text(program, encoding="utf-8")
            subprocess.run([str(BIN / ("riscv64-unknown-elf-gcc" + EXT)),
                            "--specs=sim.specs", "-std=c11", "-Wall", "-Wextra",
                            "-Wno-unused-function", str(cfile), "-o", str(elf)], check=True)
            result = subprocess.run([str(BIN / ("riscv64-unknown-elf-run" + EXT)), str(elf)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_initial_boot_exhaustion_enters_only_once(self):
        self.run_fallback(r'''
    poll(); poll(); assert(portal_calls == 0); /* elapsed time proves nothing */
    asleep = true; exhausted = true; poll(); assert(portal_calls == 0);
    asleep = false; exhausted = false; poll(); assert(portal_calls == 0);
    exhausted = true; network_busy = true;
    for (int i=0;i<100;++i) poll(); /* Includes retry backoff. */
    assert(portal_calls == 0);
    network_busy = false; poll(); assert(portal_calls == 1 && ble_calls == 1);
    poll(); portal = false; poll(); poll(); assert(portal_calls == 1);
''')

    def test_successful_boot_never_converts_later_drop_to_portal(self):
        self.run_fallback(r'''
    net.wifi_connected = true; poll(); net.wifi_connected = false;
    exhausted = true;
    for (int i=0;i<100;i++) poll();
    assert(portal_calls == 0 && ble_calls == 0);
''')

    def test_ip_success_between_polls_permanently_disables_boot_fallback(self):
        self.run_fallback(r'''
    voice_system_network_probe_success(0,VOICE_MSG_WIFI_IP_GOT,0,0,0);
    exhausted = true; poll(); poll();
    assert(portal_calls == 0);
''')

    def test_stale_probe_success_does_not_consume_initial_wifi_fallback(self):
        self.run_fallback(r'''
    probe_event_valid = false;
    voice_system_network_probe_success(0,VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS,0,0,0);
    exhausted = true; poll();
    assert(portal_calls == 1 && ble_calls == 1);
''')

    def test_recovery_claim_or_success_during_exhaustion_check_wins(self):
        for trigger in ('race_connected', 'race_busy'):
            with self.subTest(trigger=trigger):
                self.run_fallback(trigger + r''' = true; exhausted = true;
    poll(); assert(portal_calls == 0);
''')


if __name__ == "__main__":
    unittest.main()
