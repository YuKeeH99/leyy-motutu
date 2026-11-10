"""Run the fixed-RAM network history with production logging disabled/enabled."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_task_continue_guard import ROOT, CC, RUN
from test_lingxin_round_races import body


class NetworkDiagRingTest(unittest.TestCase):
    def run_history(self, timeline=False):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'FreeRTOS.h').write_text('#pragma once\n', encoding='utf-8')
            (work / 'task.h').write_text(
                '#pragma once\nvoid enter(void);void leave(void);\n'
                '#define taskENTER_CRITICAL() enter()\n#define taskEXIT_CRITICAL() leave()\n',
                encoding='utf-8')
            (work / 'lisa_time.h').write_text(
                '#pragma once\n#include <stdint.h>\nuint64_t lisa_os_get_tick_ms(void);\n',
                encoding='utf-8')
            (work / 'lisa_log.h').write_text(
                '#pragma once\nvoid diag_log(const char*,const char*,...);\n'
                '#define LISA_LOGI(...) diag_log(__VA_ARGS__)\n', encoding='utf-8')
            source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static unsigned locked, logs, now_ms, prints, inject_writes;
static char output[1024];
static unsigned output_len, dropped;
static void reset_output(void){output_len=128;dropped=0;memset(output,' ',128);output[128]=0;prints=0;}
void enter(void){assert(!locked);locked++;}
void leave(void){assert(locked==1);locked--;}
uint64_t lisa_os_get_tick_ms(void){assert(!locked);return now_ms;}
void diag_log(const char*tag,const char*fmt,...){(void)tag;(void)fmt;assert(!locked);logs++;}
'''
            source += '\n#include "' + (ROOT / 'src/system/sys_network_diag.c').as_posix() + '"\n'
            source += r'''
typedef int Shell;
static Shell shell;
static Shell *shellGetCurrent(void){assert(!locked);return &shell;}
static void shellPrint(Shell*s,const char*fmt,...){
 (void)s;(void)fmt;assert(!locked);prints++;
 char line[256];va_list ap;va_start(ap,fmt);int length=vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);
 assert(length>=0 && (unsigned)length<sizeof(line));
 /* ADB drains only after the command returns, never during shellPrint. */
 unsigned room=sizeof(output)-output_len-1;
 unsigned kept=(unsigned)length<room?(unsigned)length:room;
 memcpy(output+output_len,line,kept);output_len+=kept;output[output_len]=0;
 dropped+=(unsigned)length-kept;
 if(inject_writes){inject_writes=0;for(unsigned i=0;i<40;i++)sys_network_diag_mark("wifi","new",i,"newer");}
}
'''
            source += '\nstatic int wifi_diag(int argc,char**argv)\n' + body('src/shell/cmd/wifi.c', 'wifi_diag')
            source += r'''
int main(void){
 sys_network_diag_entry_t entry;
 uint32_t first=99;
 assert(SYS_NETWORK_DIAG_CAPACITY==32);
 assert(sys_network_diag_bounds(&first)==0 && first==0);
 assert(!sys_network_diag_read(0,&entry));
 assert(!sys_network_diag_read(1,NULL));
 for(unsigned i=1;i<=40;i++){
  now_ms=i*17;sys_network_diag_mark("wifi","reason",-(int)i,"error=2 reason=3");
 }
 assert(sys_network_diag_bounds(&first)==32 && first==9);
 assert(!sys_network_diag_read(8,&entry));
 assert(!sys_network_diag_read(41,&entry));
 for(unsigned seq=9;seq<=40;seq++){
  assert(sys_network_diag_read(seq,&entry));
  assert(entry.seq==seq && entry.uptime_ms==seq*17 && entry.value==-(int)seq);
  assert(!strcmp(entry.domain,"wifi") && !strcmp(entry.event,"reason"));
  assert(!strcmp(entry.detail,"error=2 reason=3"));
 }
 /* Worst-case strings and numbers still fit the fixed ADB TX queue, with
  * 128 bytes reserved for command echo/prompt and other shell output. */
 char wide[160];memset(wide,'w',159);wide[159]=0;
 s_network_diag_seq=UINT32_MAX-64U;s_network_diag_count=0;now_ms=UINT32_MAX;
 for(unsigned i=0;i<32;i++)sys_network_diag_mark(wide,wide,INT32_MIN,wide);
 uint32_t before=s_network_diag_seq+1U, collected=0;
 while(collected<32){
  char cursor[16];snprintf(cursor,sizeof(cursor),"%u",(unsigned)before);char *args[]={cursor};
  reset_output();assert(wifi_diag(collected?1:0,collected?args:NULL)==0);
  assert(!dropped);
  uint32_t expected=32-collected; if(expected>3)expected=3;
  assert(prints==expected+1);
  unsigned total,page,more,next_before;
  assert(sscanf(output+128,"network history count=%u page=%u more=%u next_before=%u",
                &total,&page,&more,&next_before)==4);
  assert(total==32 && page==expected && more==(collected+expected<32));
  assert(next_before==before-expected);
  char *line=output;unsigned parsed=0;
  while((line=strstr(line,"[NET-HD] seq="))!=NULL){
   unsigned seq;assert(sscanf(line,"[NET-HD] seq=%u",&seq)==1);
   assert(seq==before-expected+parsed);parsed++;line++;
  }
  assert(parsed==expected);before=next_before;collected+=expected;
 }
 char bad_cursor[]="4294967296";char *bad_args[]={bad_cursor};
 reset_output();assert(wifi_diag(1,bad_args)==-1 && !dropped);
 bad_args[0]="-1";reset_output();assert(wifi_diag(1,bad_args)==-1);
 bad_args[0]="12x";reset_output();assert(wifi_diag(1,bad_args)==-1);
 bad_args[0]="";reset_output();assert(wifi_diag(1,bad_args)==-1);
 bad_args[0]="9";reset_output();assert(wifi_diag(1,bad_args)==-1);
 s_network_diag_seq=40;s_network_diag_count=0;
 char text[160];memset(text,'x',sizeof(text));text[159]=0;
 sys_network_diag_mark(text,text,7,text);
 memset(text,'z',sizeof(text));
 assert(sys_network_diag_read(41,&entry));
 assert(strlen(entry.domain)==sizeof(entry.domain)-1);
 assert(strlen(entry.event)==sizeof(entry.event)-1);
 assert(strlen(entry.detail)==sizeof(entry.detail)-1);
 assert(entry.domain[0]=='x' && entry.event[0]=='x' && entry.detail[0]=='x');
 sys_network_diag_mark(NULL,NULL,0,NULL);
 assert(sys_network_diag_read(42,&entry));
 assert(!strcmp(entry.domain,"-") && !strcmp(entry.event,"-") && !strcmp(entry.detail,"-"));
 /* Exercise sequence overflow as well as ordinary slot wraparound. */
 s_network_diag_seq=UINT32_MAX-2U;s_network_diag_count=0;
 for(unsigned i=0;i<40;i++)sys_network_diag_mark("wifi","wrap",i,NULL);
 assert(sys_network_diag_bounds(&first)==32 && first==6);
 assert(sys_network_diag_read(37,&entry) && entry.value==39);
 assert(!sys_network_diag_read(UINT32_MAX,&entry));
 s_network_diag_seq=UINT32_MAX-2U;s_network_diag_count=0;
 for(unsigned i=0;i<4;i++)sys_network_diag_mark("wifi","wrap",i,NULL);
 assert(sys_network_diag_bounds(&first)==4 && first==UINT32_MAX-1U);
 assert(sys_network_diag_read(0,&entry) && entry.value==2);
 assert(sys_network_diag_read(1,&entry) && entry.value==3);
 assert(!sys_network_diag_read(2,&entry));
 unsigned saved=s_network_diag_seq;
 reset_output();assert(wifi_diag(0,NULL)==0 && prints==4 && s_network_diag_seq==saved && !dropped);
 char *zero_cursor[]={"0"};reset_output();assert(wifi_diag(1,zero_cursor)==0 && prints==3 && !dropped);
 reset_output();assert(wifi_diag(1,NULL)==-1 && prints==1 && s_network_diag_seq==saved);
 /* Writers can run during printing; never hold a lock or loop indefinitely. */
 reset_output();inject_writes=1;assert(wifi_diag(0,NULL)==0 && prints<=4 && !dropped);
#ifdef CONFIG_NETWORK_DIALOG_TIMELINE_LOG
 assert(logs>0);
#else
 assert(logs==0);
#endif
 assert(!locked);
}
'''
            test = work / 'diag.c'
            test.write_text(source, encoding='utf-8')
            elf = work / 'diag.elf'
            command = [str(CC), '--specs=sim.specs', '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I', str(work), str(test), '-o', str(elf)]
            if timeline:
                command.append('-DCONFIG_NETWORK_DIALOG_TIMELINE_LOG=1')
            subprocess.run(command, check=True, cwd=ROOT)
            subprocess.run([str(RUN), str(elf)], check=True, cwd=ROOT, timeout=20)

    def test_release_history_and_readonly_shell_without_timeline_log(self):
        self.run_history()

    def test_debug_logging_runs_outside_ring_lock(self):
        self.run_history(timeline=True)

    def test_ring_is_unconditional_and_never_queries_status_or_allocates(self):
        cmake = (ROOT / 'src/system/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertRegex(cmake, r'listenai_library_sources\([^)]*\bsys_network_diag\.c\b')
        source = (ROOT / 'src/system/sys_network_diag.c').read_text(encoding='utf-8')
        for symbol in ('sys_network_get_status', 'lisa_malloc', 'malloc', 'calloc', 'fopen', 'fwrite'):
            self.assertNotRegex(source, rf'\b{symbol}\s*\(')
        shell = (ROOT / 'src/shell/cmd/wifi.c').read_text(encoding='utf-8')
        self.assertRegex(shell, r'\{"diag",\s*wifi_diag,')


if __name__ == '__main__':
    unittest.main()
