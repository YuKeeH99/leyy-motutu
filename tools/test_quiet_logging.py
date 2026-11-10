"""Run production logging code under the RISC-V simulator in both profiles."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_v2_rgb_effect_contract import CC, RUN, ROOT

SDK = ROOT / 'third_party/AI2T_LingXinEngine'
ADAPTER = ROOT / 'src/server/lingxin_server/adapter'


class QuietLoggingTest(unittest.TestCase):
    def run_c(self, code, sources=(), headers=None, verbose=False):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            for name, body in (headers or {}).items():
                (tmp / name).write_text(body, encoding='utf-8')
            source, elf = tmp / 'test.c', tmp / 'test.elf'
            source.write_text(code, encoding='utf-8')
            args = [str(CC), '--specs=sim.specs', '-std=gnu11', '-I', str(tmp),
                    '-I', str(SDK / 'inc/func'), '-I', str(SDK / 'inc/system_adapter'),
                    '-I', str(ADAPTER)]
            if verbose:
                args += ['-DCONFIG_LINGXIN_VERBOSE_LOG=1', '-DCONFIG_LINGXIN_TRACE_HISTORY=1']
            subprocess.run(args + [str(source)] + [str(s) for s in sources] + ['-o', str(elf)], check=True)
            subprocess.run([str(RUN), str(elf)], check=True)

    def test_disabled_debug_does_not_evaluate_arguments(self):
        for verbose in (False, True):
            with self.subTest(verbose=verbose):
                self.run_c(r'''
#include <assert.h>
#include <stddef.h>
#include "lingxin_log.h"
static int evaluations, emitted;
void _lingxin_log_print_internal_(int level, int ut, const char *file,
    int line, const char *node, const char *format, ...) {++emitted;}
int main(void) {
    lingxin_log_debug("debug %d", ++evaluations);
    lingxin_log_ut_with_args(LINGXIN_DEBUG, "node", "%d", ++evaluations);
    lingxin_log_ut(LINGXIN_DEBUG, (++evaluations, "node"));
#ifdef CONFIG_LINGXIN_VERBOSE_LOG
    assert(evaluations==3 && emitted==3);
#else
    assert(evaluations==0 && emitted==0);
#endif
    int before=emitted;
    lingxin_log_warn("warning");
    lingxin_log_error("error");
    lingxin_log_ut(LINGXIN_WARN, "node");
    assert(emitted==before+3);
    return 0;
}
''', verbose=verbose)

    def test_severity_survives_warning_only_backend(self):
        self.run_c(r'''
#include <assert.h>
#include "lingxin_log.h"
#include "lingxin_printf.h"
int warnings, errors;
int main(void) {
    lingxin_log_printf(LINGXIN_DEBUG, "hidden");
    lingxin_log_printf(LINGXIN_WARN, "warning");
    lingxin_log_printf(LINGXIN_ERROR, "error");
    lingxin_log_printf(LINGXIN_ERROR, 0);
    assert(warnings==1 && errors==1);
    return 0;
}
''', [ADAPTER / 'lingxin_log_adapter.c'], {'lisa_log.h': '''
extern int warnings, errors;
#define LOGI(...) ((void)0)
#define LOGW(...) (++warnings)
#define LOGE(...) (++errors)
'''})

    def test_formatter_filters_direct_calls_before_metadata_and_upload(self):
        for verbose in (False, True):
            with self.subTest(verbose=verbose):
                self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "lingxin_log.h"
#include "lingxin_system_time.h"
static int formatted, uploaded, outputs, last_level;
static char last_message[512];
void parse_file_name_from_path(char *out, const char *path) {strcpy(out,"test");}
char *get_lingxin_device_name(void) {return "device";}
char *get_lingxin_device_version(void) {return "test";}
char *lingxin_get_current_thread_name(void) {return "test";}
void lingxin_get_current_time(LINGXIN_TIME *t) {++formatted;}
bool is_user_track_init(void) {return true;}
void user_track_record(char *s) {++uploaded;}
void lingxin_log_printf(int level, char *s) {
    ++outputs; last_level=level; strcpy(last_message,s);
}
int main(void) {
    _lingxin_log_print_internal_(LINGXIN_DEBUG, 1, "test.c", 1, 0, "hidden");
#ifdef CONFIG_LINGXIN_VERBOSE_LOG
    assert(formatted==1 && outputs==1 && uploaded==1);
#else
    assert(formatted==0 && outputs==0 && uploaded==0);
#endif
    int before=outputs;
    lingxin_log_warn("warning %d", 42);
    assert(outputs==before+1 && last_level==LINGXIN_WARN);
    assert(strstr(last_message,"[W]") && strstr(last_message,"warning 42"));
    lingxin_log_error("error %d", 7);
    assert(outputs==before+2 && last_level==LINGXIN_ERROR);
    assert(strstr(last_message,"[E]") && strstr(last_message,"error 7"));
    return 0;
}
''', [SDK / 'src/log/lingxin_log.c'], {
                    'lingxin_version.h': '',
                    'lingxin_common.h': 'void parse_file_name_from_path(char *, const char *);\n',
                    'lingxin_user_track.h': '#include <stdbool.h>\nbool is_user_track_init(void);\nvoid user_track_record(char *);\n',
                    'lingxin_device_info.h': 'char *get_lingxin_device_name(void);\nchar *get_lingxin_device_version(void);\n',
                    'lingxin_thread.h': 'char *lingxin_get_current_thread_name(void);\n',
                }, verbose=verbose)

    def test_trace_is_noop_when_disabled_and_restored_when_enabled(self):
        for verbose in (False, True):
            with self.subTest(verbose=verbose):
                self.run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "lingxin_trace.h"
int locks;
int main(void) {
    char *out=0;
    assert(lingxin_trace_clear()==0);
    assert(lingxin_trace_set("wake")==0);
    assert(lingxin_trace_set("record")==0);
    assert(lingxin_trace_set(0)==-1);
#ifdef CONFIG_LINGXIN_TRACE_HISTORY
    assert(locks>0);
    assert(lingxin_trace_get(&out)==0);
    assert(strcmp(out,"wake|record")==0);
    free(out);
#else
    assert(locks==0);
    assert(lingxin_trace_get(&out)==-1 && out==0);
#endif
    lingxin_trace_clear();
    out=0;
    assert(lingxin_trace_try_get(&out)==-1);
    return 0;
}
''', [ADAPTER / 'lingxin_trace.c'], {
                    'FreeRTOS.h': '',
                    'task.h': 'extern int locks;\n#define taskENTER_CRITICAL() (++locks)\n#define taskEXIT_CRITICAL() ((void)0)\n',
                    'lisa_log.h': '#define LOGI(...) ((void)0)\n',
                    'lisa_mem.h': '#include <stdint.h>\n#include <stdlib.h>\n#define lisa_mem_alloc malloc\n',
                }, verbose=verbose)


if __name__ == '__main__':
    unittest.main()
