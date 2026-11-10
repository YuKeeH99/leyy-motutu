"""Exercise the firmware's actual resource JSON parser with the server response.

Only the HTTP boundary is substituted; cJSON and both production parsers run on
the RISC-V simulator. No board, live service or flash write is involved.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.test_servo_output_gate import function
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


class ResourceResponseTest(unittest.TestCase):
    def test_prompt_tone_works_without_wake_resources_or_emoji(self):
        source = (ROOT / "src/middleware/ota/ota_api.c").read_text(encoding="utf-8")
        code = r'''
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "ota_api.h"
#define LISA_LOGE(...) ((void)0)
#define LISA_LOGI(...) ((void)0)
static const char *reply;
static cJSON *conf_get(void) { return cJSON_Parse(reply); }
''' + function(source, "parse_md5_hex") + "\n" + function(source, "ota_api_parse_res_info") + "\n" + function(source, "ota_api_get_dev_conf") + r'''
int main(void) {
 ota_dev_conf_t conf;
 reply = "{\"code\":0,\"data\":{\"wakeup_word\":{\"text\":\"\",\"resources\":[]},"
         "\"prompt_tone\":{\"name\":\"tone.bin\",\"size\":879616,"
         "\"md5\":\"ab38cb030e87f5b6050083887e423b4a\","
         "\"url\":\"https://console.databerrypi.com/uploads/other/2026/09/86e6adba-8660-4247-8b09-b4fffc039314.bin\"}}}";
 const unsigned char expected_md5[16] = {
  0xab,0x38,0xcb,0x03,0x0e,0x87,0xf5,0xb6,0x05,0x00,0x83,0x88,0x7e,0x42,0x3b,0x4a};
 assert(ota_api_get_dev_conf(&conf) == 0);
 assert(conf.prompt_tone.size == 879616);
 assert(memcmp(conf.prompt_tone.md5, expected_md5, 16) == 0);
 assert(strcmp(conf.prompt_tone.url,
  "https://console.databerrypi.com/uploads/other/2026/09/86e6adba-8660-4247-8b09-b4fffc039314.bin") == 0);
 assert(conf.wakeup_word.resource.size == 0 && conf.emoji.size == 0);
 /* Invalid metadata must never become a download candidate. */
 reply = "{\"data\":{\"prompt_tone\":{\"size\":879616,\"md5\":\"not-an-md5\",\"url\":\"https://example.invalid/tone.bin\"}}}";
 assert(ota_api_get_dev_conf(&conf) == 0);
 assert(conf.prompt_tone.size == 0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp) / "resource.c", Path(temp) / "resource.elf"
            cfile.write_text(code, encoding="utf-8")
            cjson = ROOT / "arcs-sdk/modules/cjson"
            result = subprocess.run([str(CC), "--specs=sim.specs", "-std=c11",
                                     "-I", str(cjson), "-I", str(ROOT / "src/middleware/ota"),
                                     str(cfile), str(cjson / "cJSON.c"), "-lm", "-o", str(elf)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
