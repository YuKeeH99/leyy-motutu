"""Run the real OTA check flow with power/KV/network boundaries substituted."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.test_servo_output_gate import function
from tools.test_offline_action import c_function
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


SOURCE = ROOT / "src/middleware/ota/ota_manager.c"


class OtaBatteryGateTest(unittest.TestCase):
    def run_flow(self, assertions, *defines, real_update=False):
        source = SOURCE.read_text(encoding="utf-8")
        limit = "\n".join(re.findall(r"^#define (?:OTA_APP_MIN_BATTERY_PCT|OTA_APP_UPDATE_DEFERRED_POWER)[^\n]*", source, re.M))
        power_gate = c_function(source, "ota_manager_app_power_ready") if "static bool ota_manager_app_power_ready(" in source else ""
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
''' + '\n#include "' + (ROOT / "src/middleware/ota/ota_manager.h").as_posix() + '"\n' + \
            '#include "' + (ROOT / "src/middleware/ota/ota_api.h").as_posix() + '"\n' + \
            '#include "' + (ROOT / "src/middleware/battery/battery.h").as_posix() + '"\n' + limit + r'''
#define TAG "ota_manager"
#define PROJECT_VERSION_STR "test"
#define UBOOT_FEATURE_OTA 1
#define pdTICKS_TO_MS(ms) (ms)
#define KV_KEY_USER_DISABLE_APP_UPDATE 1
#define KV_KEY_USER_DISABLE_WAKEWORD_UPDATE 2
#define KV_KEY_USER_DISABLE_TONE_UPDATE 3
#define KV_KEY_USER_DISABLE_EMOJI_UPDATE 4
#define KV_KEY_SYS_WAKEWORD 5
#define KV_KEY_SYS_OTA_PENDING_PKG 6
#define KV_KEY_SYS_OTA_PENDING_VER 7
#define OTA_PART_WAKE_WORD_BIN 1
#define OTA_PART_PROMPT_TONE_BIN 2
#define OTA_PART_EMOJI_BIN 3
#define TONE_ID_89 89
#define TONE_ID_87 87
#define TONE_ID_90 90
#define TONE_ID_91 91
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
static char last_power_skip[256];
#define LISA_LOGI(tag,...) do {char line[256];snprintf(line,sizeof(line),__VA_ARGS__); \
 if(strstr(line,"Skip app OTA: on battery"))strcpy(last_power_skip,line);} while(0)
typedef enum {OTA_APP_DECISION_NONE,OTA_APP_DECISION_UPDATE,OTA_APP_DECISION_SKIP} ota_app_decision_e;
static ota_state_t s_ota_state;
static ota_dev_conf_t dev_conf;
static ota_manager_resources_updated_cb_t s_resources_updated_cb;
static void *s_resources_updated_cb_user_data;
static bool usb,boot_support=true,app_disabled,resource_available;
static bool confirm_update,drop_at_confirm,drop_at_tone,drop_at_download_report;
static battery_status_t battery_status=BATTERY_STATUS_NOT_CONNECT;
static uint8_t percent,drop_percent=34;
static unsigned app_checks,decisions,updates,status_reads,pct_reads,resource_checks,resource_updates,reboots;
static unsigned flash_begins,power_deferred_reports,upgrade_tones;
static uint32_t xTaskGetTickCount(void){return 0;}
static void ota_manager_reconcile_last_attempt(void){}
static bool uboot_features_has(int feature){return boot_support;}
static int lisa_kv_get_int(int key,int *value){
 *value=key==KV_KEY_USER_DISABLE_APP_UPDATE?app_disabled:!resource_available;return 0;
}
static int lisa_kv_get_string(int key,char **value){*value=NULL;return 0;}
static int lisa_kv_set_string(int key,const char *value){return 0;}
static void lisa_kv_free(void *value){free(value);}
bool power_is_usb_plugged(void){return usb;}
battery_status_t battery_get_status(void){status_reads++;return battery_status;}
uint8_t battery_get_pct_raw(void){pct_reads++;return percent;}
static void ota_manager_notify_state(ota_state_e state){s_ota_state.state=state;}
int ota_api_check_app(ota_app_package_t *pkg){
 app_checks++;memset(pkg,0,sizeof(*pkg));pkg->available=true;return 0;
}
static bool ota_manager_is_blacklisted(const char *id){return false;}
int ota_api_report_app(const char *id,const char *version,const char *status,uint32_t progress,const char *message){
 if(drop_at_download_report && !strcmp(status,"download_started")){usb=false;percent=drop_percent;}
 if(!strcmp(status,"user_deferred") && message && strstr(message,"battery"))power_deferred_reports++;
 return 0;
}
static ota_app_decision_e ota_manager_wait_app_decision(const ota_app_package_t *pkg){
 decisions++;if(drop_at_confirm){usb=false;percent=drop_percent;}
 return confirm_update?OTA_APP_DECISION_UPDATE:OTA_APP_DECISION_SKIP;
}
static void ota_manager_update_reboot_strategy(void){s_ota_state.reboot=OTA_REBOOT_STRATEGY_AUTO;}
int ota_api_get_dev_conf(ota_dev_conf_t *conf){resource_checks++;conf->prompt_tone.size=resource_available?1:0;return 0;}
static int ota_flash_verify(int part,const char *md5,uint32_t size){return 1;}
static void ota_manager_play_app_success_tone_if_needed(void){}
static void ota_manager_play_tone_sync(unsigned id,unsigned repeat){
 if(id==TONE_ID_87){upgrade_tones++;if(drop_at_tone){usb=false;percent=drop_percent;}}
}
static int ota_manager_wake_word_update(void){assert(0);return 0;}
static int ota_manager_prompt_tone_update(void){resource_updates++;return 0;}
static int ota_manager_emoji_update(void){assert(0);return 0;}
static void ota_manager_do_reboot(void){reboots++;}
''' + power_gate
        if real_update:
            code += r'''
#define CMN_FLASH_REGION 0x30000000U
#define OTA_PART_APP_STAGING 4
#define OTA_FLASH_SIZE_UNKNOWN UINT32_MAX
static uint32_t download_start_tick;
static int ota_flash_update_begin(int part,uint32_t size){assert(part==OTA_PART_APP_STAGING);flash_begins++;return -9;}
static int ota_flash_update_finish(int part){return 1;}
static int ota_flash_get(int part,const void **mapped,void *size){*mapped=(const void *)0x30a00000;return 0;}
static int ota_manager_app_download_cb(void*u,uint32_t offset,const uint8_t*data,uint32_t size,uint32_t total){return 0;}
int ota_api_download_app(const ota_app_package_t*p,ota_app_download_cb_t cb,void*u){return -1;}
static void ota_manager_publish_state_event(void){}
static int uboot_ota_start_from_flash(uint32_t offset,uint32_t size){return 0;}
''' + function(source, "ota_manager_app_update")
        else:
            code += "\nstatic int ota_manager_app_update(const ota_app_package_t *pkg){updates++;return -1;}\n"
        code += function(source, "_ota_manager_check_all") + r'''
static void reset(void){
 memset(&s_ota_state,0,sizeof(s_ota_state));last_power_skip[0]='\0';
 app_checks=decisions=updates=status_reads=pct_reads=resource_checks=resource_updates=reboots=0;
 flash_begins=power_deferred_reports=upgrade_tones=0;
 confirm_update=drop_at_confirm=drop_at_tone=drop_at_download_report=false;drop_percent=34;
 usb=app_disabled=resource_available=false;boot_support=true;
 battery_status=BATTERY_STATUS_NOT_CONNECT;percent=0;
}
''' + assertions
        with tempfile.TemporaryDirectory() as directory:
            cfile, elf = Path(directory) / "ota.c", Path(directory) / "ota.elf"
            cfile.write_text(code, encoding="utf-8")
            built = subprocess.run([str(CC), "--specs=sim.specs", "-std=c11", *defines,
                                    str(cfile), "-o", str(elf)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_34_percent_skips_but_35_percent_reaches_update_confirmation(self):
        self.run_flow(r'''
int main(void){
 const unsigned cases[]={35,34,0,49,50,100};
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  reset();percent=cases[i];assert(_ota_manager_check_all()==0);
  assert(app_checks==(percent>=35) && decisions==app_checks && !updates);
  assert(pct_reads==1 && status_reads==1);
  assert(s_ota_state.state==OTA_STATE_UP_TO_DATE);
  if(percent<35)assert(strstr(last_power_skip,"< 35%"));else assert(!last_power_skip[0]);
 }
}
''')

    def test_usb_and_no_battery_exemptions_remain_unchanged(self):
        self.run_flow(r'''
int main(void){
 for(unsigned status=BATTERY_STATUS_NO_BATTERY;status<=BATTERY_STATUS_UNKNOWN;status++){
  reset();usb=true;battery_status=status;percent=0;
  assert(_ota_manager_check_all()==0 && app_checks==1 && decisions==1);
  assert(!pct_reads && !status_reads && !last_power_skip[0]);
 }
 reset();battery_status=BATTERY_STATUS_NO_BATTERY;percent=0;
 assert(_ota_manager_check_all()==0 && app_checks==1 && decisions==1);
 assert(!pct_reads && status_reads==1 && !last_power_skip[0]);
 /* A stale charger status alone does not create a new exemption. */
 for(unsigned status=BATTERY_STATUS_NOT_CONNECT;status<=BATTERY_STATUS_UNKNOWN;status++){
  reset();battery_status=status;percent=34;
  assert(_ota_manager_check_all()==0 && !app_checks && !decisions && pct_reads==1);
 }
}
''')

    def test_disabling_app_ota_and_unsupported_boot_still_win(self):
        self.run_flow(r'''
int main(void){
 reset();app_disabled=true;usb=true;percent=100;
 assert(_ota_manager_check_all()==0 && !app_checks && !decisions && !pct_reads);
 reset();boot_support=false;usb=true;percent=100;
 assert(_ota_manager_check_all()==0 && !app_checks && !decisions && !pct_reads);
 reset();boot_support=false;percent=35;
 assert(_ota_manager_check_all()==0 && !app_checks && !pct_reads);
}
''')

    def test_compile_time_disable_is_not_bypassed_by_battery_or_usb(self):
        self.run_flow(r'''
int main(void){
 reset();usb=true;percent=100;
 assert(_ota_manager_check_all()==0 && !app_checks && !decisions && !pct_reads);
}
''', "-DCONFIG_OTA_DISABLE_APP_UPDATE=1")

    def test_low_battery_gate_does_not_change_existing_resource_update_flow(self):
        self.run_flow(r'''
int main(void){
 reset();percent=34;resource_available=true;
 assert(_ota_manager_check_all()==0 && !app_checks && !updates);
 assert(resource_checks==1 && resource_updates==1 && reboots==1);
 assert(s_ota_state.target==OTA_TARGET_PROMPT_TONE && s_ota_state.state==OTA_STATE_SUCCESSED);
}
''')

    def test_confirmation_power_drop_never_starts_system_flash_and_can_retry(self):
        self.run_flow(r'''
int main(void){
 for(unsigned mode=0;mode<4;mode++){
  reset();percent=50;confirm_update=true;
  if(mode==0)drop_at_confirm=true;
  if(mode==1){usb=true;percent=0;drop_at_confirm=true;}
  if(mode==2){usb=true;percent=0;drop_at_tone=true;}
  if(mode==3){usb=true;percent=0;drop_at_download_report=true;}
  assert(_ota_manager_check_all()==0);
  assert(app_checks==1 && decisions==1 && !flash_begins && !reboots);
  assert(power_deferred_reports==1 && s_ota_state.state==OTA_STATE_UP_TO_DATE);
  assert(s_ota_state.post_action==OTA_POST_ACTION_NONE);
  if(mode<2)assert(!upgrade_tones);
  /* A later explicit check is still permitted after the power problem clears. */
  drop_at_confirm=drop_at_tone=drop_at_download_report=false;percent=35;confirm_update=false;
  assert(_ota_manager_check_all()==0 && app_checks==2 && decisions==2 && !flash_begins);
 }
}
''', real_update=True)

    def test_preflash_recheck_keeps_35_percent_usb_and_no_battery_exemptions(self):
        self.run_flow(r'''
int main(void){
 for(unsigned mode=0;mode<4;mode++){
  reset();confirm_update=true;
  if(mode==0)percent=35;
  if(mode==1)usb=true;
  if(mode==2)battery_status=BATTERY_STATUS_NO_BATTERY;
  if(mode==3){usb=true;drop_at_confirm=true;drop_percent=35;}
  /* The fake flash boundary intentionally fails after recording entry. */
  (void)_ota_manager_check_all();assert(flash_begins==1);
  assert(!power_deferred_reports && decisions==1);
 }
}
''', real_update=True)


if __name__ == "__main__":
    unittest.main()
