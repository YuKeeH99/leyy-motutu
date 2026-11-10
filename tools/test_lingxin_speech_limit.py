"""Exercise the real WebRTC detector and speech-relative duration on RISC-V."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_v2_rgb_effect_contract import ROOT, CC, RUN


class SpeechLimitTest(unittest.TestCase):
    def test_confirmation_excludes_hangover_and_backdates_budget(self):
        directory = ROOT / 'src/server/lingxin_server/adapter'
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp/'vad').mkdir()
            (tmp/'vad/vad_core.h').write_text('''
#include <stdint.h>
typedef struct {int unused;} VadInstT;
int WebRtcVad_InitCore(VadInstT *v);
int WebRtcVad_set_mode_core(VadInstT *v,int mode);
int WebRtcVad_CalcVad16khz(VadInstT *v,const int16_t *p,int n);
''')
            test, elf = tmp/'onset.c', tmp/'onset.elf'
            test.write_text(r'''
#include <assert.h>
#include "lingxin_speech_limit.h"
#include "vad/vad_core.h"
static int result;
int WebRtcVad_InitCore(VadInstT*v){return 0;}
int WebRtcVad_set_mode_core(VadInstT*v,int m){return 0;}
int WebRtcVad_CalcVad16khz(VadInstT*v,const int16_t*p,int n){return result;}
int main(void) {
 int16_t pcm[160]={0};lingxin_speech_limit_result_t r;
 result=1;for(int i=0;i<19;i++)r=lingxin_speech_limit_process(1,pcm,160);
 assert(!r.speech_samples && r.window_peak==19);
 result=7;for(int i=0;i<50;i++)r=lingxin_speech_limit_process(1,pcm,160);
 assert(!r.speech_samples && r.positive_frames==19);
 /* Across a rolling window: 20 positives, interspersed negatives. */
 for(int i=0;i<29;i++) {
  result=i%3==2?0:1;r=lingxin_speech_limit_process(1,pcm,160);
  assert(r.accepted==160);
  if(i<28)assert(!r.speech_samples);
 }
 assert(r.speech_samples==29*160 && r.window_peak==20);
 assert(LINGXIN_RECORD_MAX_SAMPLES==192000U);
 unsigned remaining=192000-r.speech_samples,accepted=0;
 result=0;while(!r.reached){r=lingxin_speech_limit_process(1,pcm,160);accepted+=r.accepted;}
 assert(accepted==remaining && r.speech_samples==192000);
 /* Ten seconds before onset must not consume the new round's budget. */
 result=0;for(int i=0;i<1000;i++) {
  r=lingxin_speech_limit_process(2,pcm,160);
  assert(r.accepted==160 && !r.speech_samples && !r.reached);
 }
 assert(!r.positive_frames && !r.window_peak);
 result=1;for(int i=0;i<1199;i++) {
  r=lingxin_speech_limit_process(2,pcm,160);
  assert(r.accepted==160 && !r.reached);
 }
 assert(r.speech_samples==191840);
 r=lingxin_speech_limit_process(2,pcm,160);
 assert(r.reached && r.accepted==160 && r.speech_samples==192000);
 r=lingxin_speech_limit_process(2,pcm,160);assert(r.reached && !r.accepted);
}
''')
            subprocess.run([str(CC),'--specs=sim.specs','-std=c11','-I',str(tmp),'-I',str(directory),
                            str(test),str(directory/'lingxin_speech_limit.c'),'-o',str(elf)],check=True)
            subprocess.run([str(RUN),str(elf)],check=True,timeout=20)

    def test_silence_then_speech_budget_and_next_round(self):
        directory = ROOT / 'src/server/lingxin_server/adapter'
        source = directory / 'lingxin_speech_limit.c'
        self.assertTrue(source.exists(), 'Missing speech-relative duration detector')
        code = r'''
#include <assert.h>
#include "lingxin_speech_limit.h"
int main(void) {
 int16_t silence[256]={0}, speech[256];
 lingxin_speech_limit_result_t r;
 for(unsigned i=0;i<256;i++) speech[i]=(int16_t)(i*i);
 /* A short prompt tail must not start an irreversible twelve-second clock. */
 for(unsigned i=0;i<6;i++)
  r=lingxin_speech_limit_process(99,speech,256);
 for(unsigned i=0;i<1000;i++) {
  r=lingxin_speech_limit_process(99,silence,256);
  assert(r.accepted==256 && !r.speech_samples && !r.reached);
 }
 /* Thirty seconds of silence still uploads; listening time is not speech. */
 for(unsigned i=0;i<1875;i++) {
  r=lingxin_speech_limit_process(1,silence,256);
  assert(r.accepted==256 && !r.speech_samples && !r.reached);
 }
 unsigned submitted=0;
 do {
  r=lingxin_speech_limit_process(1,speech,256);
  submitted+=r.accepted;
 } while(!r.speech_samples && submitted<16000);
 assert(r.speech_samples && !r.reached);
 /* After onset, pauses count too. Exactly 192000 samples from onset. */
 unsigned left=LINGXIN_RECORD_MAX_SAMPLES-r.speech_samples, tail=0;
 while(!r.reached) {
  r=lingxin_speech_limit_process(1,silence,256);tail+=r.accepted;
  assert(r.speech_samples<=LINGXIN_RECORD_MAX_SAMPLES);
 }
 assert(tail==left && r.speech_samples==192000);
 r=lingxin_speech_limit_process(1,speech,256);assert(!r.accepted && r.reached);
 /* Fresh round discards both old VAD hangover and old partial-frame audio. */
 for(unsigned i=0;i<600;i++) {
  r=lingxin_speech_limit_process(2,silence,256);
  assert(r.accepted==256 && !r.speech_samples && !r.reached);
 }
 return 0;
}
'''
        library = ROOT / 'third_party/libfvad/src'
        with tempfile.TemporaryDirectory() as tmp:
            cfile, elf = Path(tmp)/'vad.c', Path(tmp)/'vad.elf'
            cfile.write_text(code, encoding='utf-8')
            sources = list((library/'vad').glob('*.c')) + list((library/'signal_processing').glob('*.c'))
            subprocess.run([str(CC),'--specs=sim.specs','-O2','-std=c11','-I',str(directory),
                            '-I',str(library),str(cfile),str(source),*map(str,sources),'-o',str(elf)],check=True)
            subprocess.run([str(RUN),str(elf)],check=True,timeout=30)
