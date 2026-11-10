"""Bounded diagnostic capture must stop, preserve rounds, and never auto-arm."""
import tempfile
import subprocess
import unittest
from pathlib import Path
from test_task_continue_guard import ROOT, CC, RUN


class UplinkSampleTest(unittest.TestCase):
    def test_one_shot_bounds_and_round_tags(self):
        source = ROOT / 'apps/arcs-mini/uplink_sample.c'
        self.assertTrue(source.exists(), 'temporary capture not implemented')
        code = r'''
#define UPLINK_SAMPLE_TEST
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#define __psram_bss__
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
static uint32_t now;
static uint32_t lisa_os_get_tick_ms(void) {return now;}
''' + '#include "' + source.as_posix() + '"\n' + r'''
int main(void) {
 unsigned char pcm[1280]; memset(pcm, 0x35, sizeof pcm);
 lingxin_uplink_sample_append(pcm,sizeof pcm,3); assert(!s_bytes);
 assert(sample_arm()==0); now=100;
 lingxin_uplink_sample_append(pcm,sizeof pcm,3);
 memset(pcm,0x57,sizeof pcm); now=140;
 lingxin_uplink_sample_append(pcm,sizeof pcm,4);
 assert(s_bytes==2560 && s_frames==2);
 assert(s_pcm[0]==0x35 && s_pcm[1280]==0x57);
 assert(s_meta[0].epoch==3 && s_meta[1].epoch==4);
 now=20100; lingxin_uplink_sample_append(pcm,sizeof pcm,4);
 assert(!s_active && s_bytes==2560);
 assert(sample_arm()!=0); /* No accidental second recording. */
 s_used=false; assert(sample_arm()==0); now=20200;
 for(int i=0;i<600;i++) lingxin_uplink_sample_append(pcm,sizeof pcm,5);
 assert(!s_active && s_bytes==640000 && s_frames==500);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            test, elf = Path(d)/'test.c', Path(d)/'test.elf'
            test.write_text(code, encoding='utf-8')
            subprocess.run([str(CC),'--specs=sim.specs','-std=c11',str(test),'-o',str(elf)],check=True)
            subprocess.run([str(RUN),str(elf)],check=True,timeout=25)
