"""Execute the firmware's real BLE identity sender with the production cJSON."""
import json
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.test_offline_action import c_function
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


class DeviceInfoTest(unittest.TestCase):
    def test_identity_roundtrips_version_and_escaped_sn_without_truncation(self):
        source = (ROOT / 'src/middleware/ble/app_ble_common.c').read_text(encoding='utf-8')
        sender = c_function(source, 'app_ble_send_device_info_now')
        framing = c_function(source, 'app_ble_send_payload')
        code = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#define PROJECT_VERSION_STR "12.34.56"
#define TAG "test"
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static char long_sn[800];
static const char *current_sn="serial\"with\\escape";
static const char *app_chip_id_str_get(void) {return "0011223344556677";}
static bool lingxin_server_config_ready(void) {return true;}
static const char *lingxin_server_config_sn(void) {return current_sn;}
static bool app_ble_tx_valid(uint8_t conidx) {return conidx == 0;}
#define pdMS_TO_TICKS(ms) (ms)
#define vTaskDelay(ticks) ((void)(ticks))
static int ble_netcfg_bles_send_notify_raw(uint8_t conidx, size_t len, uint8_t *frame) {
 (void)conidx; for (size_t i=0;i<len;++i) printf("%02x",frame[i]);puts("");return 0;
}
''' + framing + sender + r'''
int main(void) {
 app_ble_send_device_info_now(0,"device_info");
 app_ble_send_device_info_now(0,"wifi_connected");
 memset(long_sn,'X',sizeof(long_sn)-1); current_sn=long_sn;
 app_ble_send_device_info_now(0,"device_info");
 return 0;
}
'''
        cjson = ROOT / 'arcs-sdk/modules/cjson'
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp) / 'identity.c', Path(temp) / 'identity.elf'
            cfile.write_text(code, encoding='utf-8')
            build = subprocess.run([str(CC), '--specs=sim.specs', '-std=c11',
                                    '-I', str(ROOT / 'third_party/AI2T_LingXinEngine/src/inc'), '-I', str(cjson),
                                    str(cfile), str(cjson / 'cJSON.c'), '-lm', '-o', str(elf)],
                                   capture_output=True, text=True)
            self.assertEqual(0, build.returncode, build.stderr)
            run = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(0, run.returncode, run.stderr)
        spec = importlib.util.spec_from_file_location('identity_ble', ROOT / '配网/ble_provision.py')
        ble = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ble)
        decoder = ble.NotificationDecoder()
        messages = []
        frame_count = 0
        for line in run.stdout.splitlines():
            frame = bytes.fromhex(line)
            self.assertLessEqual(len(frame), 20)
            self.assertEqual(len(frame) - 3, frame[2])
            self.assertGreater(frame[1], 1, 'Identity must exercise multiple BLE frames')
            messages.extend(decoder.feed(frame))
            frame_count += 1
        self.assertGreater(frame_count, 2)
        self.assertEqual(2, len(messages), 'Oversized identity must not send truncated JSON')
        for event, message in zip(('device_info', 'wifi_connected'), messages):
            self.assertEqual({'code': 0, 'event': event, 'chip_id': '0011223344556677',
                              'sn': 'serial"with\\escape', 'firmware_version': '12.34.56'}, json.loads(message))


if __name__ == '__main__':
    unittest.main()
