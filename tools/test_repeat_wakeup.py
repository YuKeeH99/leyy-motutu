"""Exercise the real AP callback: delivery jitter must not merge two keywords."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.test_offline_action import c_function
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


class RepeatWakeupTest(unittest.TestCase):
    def test_source_positions_survive_batched_delivery_and_counter_reset(self):
        source = (ROOT / 'src/server/lschat_server/wakeup/app_wakeup.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
typedef uint32_t TickType_t;
typedef int acomp_wakeup_algo_mode_e;
#define ACOMP_WAKEUP_ALGO_MODE_WAKEUP 0
#define WAKEUP_CB_EVENT_ENGINE_RLT 1
#define WAKEUP_CB_EVENT_STREAM_UPDATE 2
#define WAKEUP_CB_EVENT_ENGINE_TIMEOUT 4
#define WAKEUP_CB_EVENT_ENGINE_ANGLE 8
#define WAKEUP_CB_EVENT_ENGINE_SWITCH_MODE 16
#define VOICE_MSG_WAKEUP_KEYWORD 1
#define VOICE_MSG_WAKEUP_COMMAND 2
#define VOICE_MSG_WAKEUP_RECOGNIZED_FINISH 3
#define VOICE_MSG_WAKEUP_RECOGNIZED_START 4
#define WAKEUP_KEYWORD_PUB_MIN_INTERVAL_MS 200
#define pdMS_TO_TICKS(n) (n)
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGD(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
static TickType_t now, s_last_wakeup_keyword_tick;
static unsigned published, commands, netcfg_diagnostics;
static int netcfg_valid;
static char netcfg_detail[64];
static void sys_network_diag_mark(const char *domain,const char *event,int value,const char *detail) {
 assert(strcmp(domain,"wakeup")==0 && strcmp(event,"netcfg_result")==0);
 ++netcfg_diagnostics;netcfg_valid=value;snprintf(netcfg_detail,sizeof netcfg_detail,"%s",detail);
}
static bool s_probe_active;
static unsigned s_probe_keywords,s_probe_commands;
static unsigned s_diag_results,s_diag_keywords,s_diag_parse_failed;
static unsigned s_diag_keyword_accepted,s_diag_keyword_throttled;
static int s_diag_mode;
static TickType_t xTaskGetTickCount(void) {return now;}
static void voice_msg_pub(int type,void *data,unsigned size) {
 if(type==VOICE_MSG_WAKEUP_KEYWORD) ++published;
 if(type==VOICE_MSG_WAKEUP_COMMAND) ++commands;
}
'''
        code += c_function(source, 'app_algo_command_extract')
        if 'static void app_wakeup_trace_netcfg(' in source:
            code += c_function(source, 'app_wakeup_trace_netcfg')
        if 'static bool app_wakeup_keyword_should_publish(' in source:
            code += c_function(source, 'app_wakeup_keyword_should_publish')
        code += c_function(source, 'wakeup_event_handler')
        code += r'''
static void wake(unsigned tick,const char *fields) {
 char raw[256];
 snprintf(raw,sizeof raw,"{\"rlt\":[{\"bMain\":1,\"intentStr\":\"hei1 qiu2 qiu2\",%s}]}",fields);
 now=tick;wakeup_event_handler(WAKEUP_CB_EVENT_ENGINE_RLT,raw,strlen(raw),NULL);
}
int main(void) {
 /* Two physical utterances, delivered 10 ms apart after RPC scheduling. */
 wake(10,"\"istart\":100,\"iframe\":144");assert(published==1);
 wake(20,"\"istart\":160,\"iframe\":204");assert(published==2);
 wake(30,"\"istart\":160,\"iframe\":204");assert(published==2);
 /* A later callback for the same detection may carry a newer current frame. */
 wake(40,"\"istart\":160,\"iframe\":205");assert(published==2);
 /* Bounded duplicate suppression cannot poison a restarted AP frame counter. */
 wake(1300,"\"istart\":160,\"iframe\":204");assert(published==3);
 wake(1310,"\"istart\":0,\"iframe\":44");assert(published==4);
 /* Missing / invalid metadata uses the legacy bounded debounce. */
 const char *bad[]={"\"istart\":-1,\"iframe\":44", "\"istart\":0.5,\"iframe\":44",
 "\"istart\":4294967296,\"iframe\":44", "\"istart\":10,\"iframe\":9",
 "\"istart\":10,\"iframe\":-1", "\"istart\":10,\"iframe\":12.5",
 "\"istart\":10,\"iframe\":4294967296", "\"istart\":\"10\",\"iframe\":44",
 "\"iframe\":44", "\"istart\":10"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];++i) {
  unsigned before=published;
  wake(1320+i,bad[i]);assert(published==before);
 }
 wake(1600,"\"iresid\":0");assert(published==5);
 wake(1610,"\"istart\":100,\"iframe\":144");assert(published==5);
 wake(1900,"\"istart\":4294967250,\"iframe\":4294967295");assert(published==6);
 wake(1910,"\"istart\":10,\"iframe\":54");assert(published==7);
 /* RTOS tick wrap must not turn the elapsed window into a signed compare. */
 wake(4294967250U,"\"istart\":50,\"iframe\":94");assert(published==8);
 wake(10,"\"istart\":50,\"iframe\":94");assert(published==8);
 wake(20,"\"istart\":110,\"iframe\":154");assert(published==9);
 assert(s_diag_keyword_accepted==published && s_diag_keyword_throttled>0);
 assert(netcfg_diagnostics==0);
 const char *netcfg[]={
  "{\"rlt\":[{\"bMain\":0,\"intentStr\":\"jin4 ru4 pei4 wang3 mo2 shi4\",\"istart\":30,\"iframe\":20,\"iduration\":40}]}",
  "{\"rlt\":[{\"bMain\":0,\"intentStr\":\"jin4 ru4 pei4 wang3 mo2 shi4\"}]}",
  "{\"rlt\":[{\"bMain\":0,\"intentStr\":\"jin4 ru4 pei4 wang3 mo2 shi4\",\"istart\":-1,\"iframe\":1.5,\"iduration\":4294967296}]}"};
 for(unsigned i=0;i<3;++i) {
  wakeup_event_handler(WAKEUP_CB_EVENT_ENGINE_RLT,(void*)netcfg[i],strlen(netcfg[i]),NULL);
  assert(commands==i+1 && netcfg_diagnostics==i+1);
  if(i==0) assert(netcfg_valid==7 && strcmp(netcfg_detail,"istart=30 iframe=20 iduration=40")==0);
  else assert(netcfg_valid==0 && strcmp(netcfg_detail,"istart=4294967295 iframe=4294967295 iduration=4294967295")==0);
 }
 const char *other="{\"rlt\":[{\"bMain\":0,\"intentStr\":\"jin4 ru4 lian2 wang3 mo2 shi4\"}]}";
 wakeup_event_handler(WAKEUP_CB_EVENT_ENGINE_RLT,(void*)other,strlen(other),NULL);
 assert(commands==4 && netcfg_diagnostics==3);
 const char *as_keyword="{\"rlt\":[{\"bMain\":1,\"intentStr\":\"jin4 ru4 pei4 wang3 mo2 shi4\"}]}";
 now+=1000;wakeup_event_handler(WAKEUP_CB_EVENT_ENGINE_RLT,(void*)as_keyword,strlen(as_keyword),NULL);
 assert(netcfg_diagnostics==3 && published==10);
 return 0;
}
'''
        cjson = ROOT / 'arcs-sdk/modules/cjson'
        with tempfile.TemporaryDirectory() as tmp:
            test, elf = Path(tmp) / 'repeat.c', Path(tmp) / 'repeat.elf'
            test.write_text(code, encoding='utf-8')
            subprocess.run([str(CC), '--specs=sim.specs', '-std=c11', '-I', str(cjson),
                            str(test), str(cjson/'cJSON.c'), '-lm', '-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
