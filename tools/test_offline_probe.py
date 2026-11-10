"""Run the real diagnostic worker and classifier; board/AP calls are boundary fakes."""
import subprocess
import tempfile
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.test_servo_output_gate import function
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


class OfflineProbeTest(unittest.TestCase):
    def test_classifier_and_natural_wakeup_probe_restore(self):
        source = (ROOT / 'src/server/lschat_server/wakeup/app_wakeup.c').read_text(encoding='utf-8')
        self.assertIn('static void wakeup_probe_task(', source, 'missing bounded recognition-only probe')
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define pdPASS 1
#define shellPrint(...) ((void)0)
#define ACOMP_ERR_OK 0
#define ACOMP_WAKEUP_ALGO_MODE_WAKEUP 0
#define ACOMP_WAKEUP_ALGO_MODE_ESR 1
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(n) (n)
typedef uint32_t TickType_t;
typedef struct {bool connected; bool wifi_connected;} sys_network_status_t;
static bool s_probe_active, s_probe_stop;
static uint32_t s_probe_seconds, s_probe_commands, s_probe_keywords;
static TickType_t ticks;
static int calls[8], count, fail_timeout, restore_fail, network_at;
static int releases, acquires, create_ok=1;
static bool acquire_ok=true, released_ok;
static bool s_audio_inited=true, s_wakeup_feed_enabled=true;
static int s_diag_mode;
static bool service_offline_action_diagnostic_acquire(void) {++acquires; return acquire_ok;}
static void service_offline_action_diagnostic_release(bool ok) {++releases;released_ok=ok;}
static int xTaskCreate(void(*f)(void*),const char*n,int sz,void*a,int p,void*t) {return create_ok;}
static int acomp_wakeup_set_timeout(uint32_t ms) {calls[count++]=ms; return ms ? fail_timeout : 0;}
static int acomp_wakeup_set_algo_mode(int mode) {assert(mode==0); calls[count++]=mode+100; return restore_fail;}
static TickType_t xTaskGetTickCount(void) {return ticks;}
static void vTaskDelay(TickType_t ms) {ticks+=ms;}
static void vTaskDelete(void *p) {(void)p;}
static int sys_network_get_status(sys_network_status_t *s) {s->connected=(network_at && ticks>=network_at);s->wifi_connected=false;return 0;}
''' + function(source, 'app_algo_command_extract') + '\n' + function(source, 'wakeup_probe_task') + '\n' + function(source, 'shell_wakeup_probe') + r'''
int main(void) {
 uint8_t text[64]; bool wake=false;
 assert(app_algo_command_extract((const uint8_t*)"{\"rlt\":[{\"bMain\":1,\"intent\":\"hello\"}]}",text,64,&wake)==0 && wake);
 assert(app_algo_command_extract((const uint8_t*)"{\"rlt\":[{\"bMain\":0,\"intent\":\"wang3-qiu2-zheng4-shou3\"}]}",text,64,&wake)==0 && !wake);
 assert(strcmp((char*)text,"wang3-qiu2-zheng4-shou3")==0);
 assert(app_algo_command_extract((const uint8_t*)"{\"rlt\":[{\"bMain\":false,\"intentStr\":\"2\"}]}",text,64,&wake)==0 && !wake);
 assert(app_algo_command_extract((const uint8_t*)"{}",text,64,&wake)!=0);
 s_probe_active=true; s_probe_seconds=5;
 wakeup_probe_task(NULL);
 assert(ticks==5000 && !s_probe_active && count==3);
 assert(releases==1 && released_ok);
 assert(calls[0]==5000 && calls[1]==0 && calls[2]==100);
 count=0; ticks=0; fail_timeout=-1; s_probe_active=true;
 wakeup_probe_task(NULL);
 assert(!s_probe_active && ticks==0 && count==3 && calls[1]==0 && calls[2]==100);
 count=0; fail_timeout=0; network_at=200; s_probe_active=true;
 wakeup_probe_task(NULL);
 assert(!s_probe_active && ticks==200 && count==3);
 count=0; ticks=0; network_at=0; s_probe_active=true; s_probe_stop=true;
 wakeup_probe_task(NULL);
 assert(!s_probe_active && ticks==0 && count==3);
 count=0; s_probe_active=true; restore_fail=-1;
 wakeup_probe_task(NULL);
 assert(s_probe_active && count==3);
 assert(releases==5 && !released_ok);
 char *argv[]={"wakeup_probe","10"};
 s_probe_active=false;acquire_ok=false;
 assert(shell_wakeup_probe(2,argv)==-1 && acquires==1 && !s_probe_active);
 acquire_ok=true;create_ok=0;
 assert(shell_wakeup_probe(2,argv)==-1 && acquires==2 && releases==6 && released_ok);
 create_ok=1;
 assert(shell_wakeup_probe(2,argv)==0 && acquires==3 && s_probe_active);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'probe.c', Path(temp)/'probe.elf'
            cfile.write_text(code, encoding='utf-8')
            cjson = ROOT / 'arcs-sdk/modules/cjson'
            compile_run = subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(cjson),
                                          str(cfile), str(cjson/'cJSON.c'), '-lm', '-o', str(elf)], capture_output=True, text=True)
            self.assertEqual(compile_run.returncode, 0, compile_run.stderr)
            result = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

if __name__ == '__main__':
    unittest.main()
