"""Inject duplicate OTA requests and thread failures into the real entry/worker."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.test_servo_output_gate import function
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


class OtaCheckLifecycleTest(unittest.TestCase):
    def run_lifecycle(self, assertions):
        source = (ROOT / "src/middleware/ota/ota_manager.c").read_text(encoding="utf-8")
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define LISA_OS_PRIORITY_LOW 6
#define pdMS_TO_TICKS(ms) (ms)
enum{OTA_STATE_IDLE,OTA_STATE_CHECKING,OTA_STATE_PACKAGE_INFO,OTA_STATE_UPDATING,OTA_STATE_SUCCESSED,OTA_STATE_FAILED,OTA_STATE_UP_TO_DATE};
static struct{int state;}s_ota_state;
static bool s_ota_check_active,locked,create_fails,reenter_on_create,reenter_on_wait;
static unsigned creates,work,deleted,waited;
static int work_result;
static void (*pending_entry)(void*);
typedef struct{const char*name;uint32_t stack_size,priority;}lisa_thread_attr_t;
typedef struct{int unused;}lisa_thread_t;
static lisa_thread_t thread;
static void enter_critical(void){assert(!locked);locked=true;}
static void exit_critical(void){assert(locked);locked=false;}
#define taskENTER_CRITICAL() enter_critical()
#define taskEXIT_CRITICAL() exit_critical()
int ota_manager_check_all(void);
static void vTaskDelay(unsigned ms){
 assert(!locked && ms==500);waited++;
 if(reenter_on_wait){reenter_on_wait=false;assert(ota_manager_check_all()==0);}
}
static int _ota_manager_check_all(void){work++;s_ota_state.state=work_result<0?OTA_STATE_FAILED:OTA_STATE_UP_TO_DATE;return work_result;}
static void lisa_thread_delete(void*p){assert(!locked);deleted++;}
static lisa_thread_t*lisa_thread_create(const lisa_thread_attr_t*a,void(*fn)(void*),void*arg){
 assert(!locked);creates++;
 if(reenter_on_create){reenter_on_create=false;assert(ota_manager_check_all()==0);}
 if(create_fails)return NULL;
 pending_entry=fn;return &thread;
}
''' + function(source, "ota_manager_check_task") + "\n" + function(source, "ota_manager_check_all") + assertions
        with tempfile.TemporaryDirectory() as directory:
            cfile, elf = Path(directory) / "ota.c", Path(directory) / "ota.elf"
            cfile.write_text(code, encoding="utf-8")
            built = subprocess.run([str(CC), "--specs=sim.specs", "-std=c11", str(cfile), "-o", str(elf)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(RUN), str(elf)], capture_output=True, text=True, timeout=20)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_requests_during_creation_and_initial_delay_share_one_worker(self):
        self.run_lifecycle(r'''
int main(void){
 reenter_on_create=true;assert(ota_manager_check_all()==0);
 assert(creates==1 && s_ota_check_active && !locked);
 assert(ota_manager_check_all()==0 && creates==1);
 reenter_on_wait=true;pending_entry(NULL);
 assert(creates==1 && work==1 && waited==1 && deleted==1 && !s_ota_check_active && !locked);
 assert(ota_manager_check_all()==0 && creates==2 && s_ota_check_active);
 pending_entry(NULL);assert(work==2 && deleted==2 && !s_ota_check_active);
}
''')

    def test_creation_failure_releases_reservation_and_returns_error(self):
        self.run_lifecycle(r'''
int main(void){
 create_fails=true;assert(ota_manager_check_all()<0);
 assert(creates==1 && !s_ota_check_active && !locked && s_ota_state.state==OTA_STATE_IDLE);
 create_fails=false;assert(ota_manager_check_all()==0 && creates==2 && s_ota_check_active);
 pending_entry(NULL);assert(!s_ota_check_active && deleted==1);
}
''')

    def test_worker_failure_and_existing_busy_states_remain_retryable(self):
        self.run_lifecycle(r'''
int main(void){
 for(int state=OTA_STATE_CHECKING;state<=OTA_STATE_UPDATING;state++){
  s_ota_state.state=state;assert(ota_manager_check_all()==0 && !creates && !locked);
 }
 s_ota_state.state=OTA_STATE_IDLE;work_result=-1;
 assert(ota_manager_check_all()==0 && creates==1);pending_entry(NULL);
 assert(!s_ota_check_active && s_ota_state.state==OTA_STATE_FAILED);
 work_result=0;assert(ota_manager_check_all()==0 && creates==2);pending_entry(NULL);
 assert(!s_ota_check_active && s_ota_state.state==OTA_STATE_UP_TO_DATE && deleted==2);
}
''')


if __name__ == "__main__":
    unittest.main()
