"""Exercise literal local version token sequences, including malformed input."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from tools.test_v2_rgb_effect_contract import ROOT, CC, RUN


class LocalVersionTokensTest(unittest.TestCase):
    def test_version_tokens_are_bounded_digits_and_decimal_points(self):
        source = ROOT / 'apps/arcs-mini/services/service_local_version.c'
        production = source.read_text(encoding='utf-8') if source.exists() else '''
#include <stddef.h>
#include <stdint.h>
size_t service_local_version_tokens(const char *v, uint16_t *t, size_t n) {return 0;}
'''
        code = production + r'''
#include <assert.h>
#include <string.h>
int main(void) {
 uint16_t tokens[32]={0};
 const uint16_t expected[]={119,123,120,127,120,124};
 assert(service_local_version_tokens("2.6.3",tokens,32)==6);
 assert(memcmp(tokens,expected,sizeof(expected))==0);
 const uint16_t multi[]={119,122,123,120,124,125,120,121};
 assert(service_local_version_tokens("12.34.0",tokens,32)==8);
 assert(memcmp(tokens,multi,sizeof(multi))==0);
 assert(service_local_version_tokens("2.6.3",tokens,5)==0);
 assert(service_local_version_tokens("",tokens,32)==0);
 assert(service_local_version_tokens("2..3",tokens,32)==0);
 assert(service_local_version_tokens(".2",tokens,32)==0);
 assert(service_local_version_tokens("2.",tokens,32)==0);
 assert(service_local_version_tokens("2.6-dev",tokens,32)==0);
 assert(service_local_version_tokens("1234567890123456789012345678901234",tokens,32)==0);
 assert(service_local_version_tokens(NULL,tokens,32)==0);
 assert(service_local_version_tokens("2",NULL,32)==0);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cfile, elf = Path(temp)/'version.c', Path(temp)/'version.elf'
            cfile.write_text(code, encoding='utf-8')
            result = subprocess.run([str(CC),'--specs=sim.specs','-std=c11','-I',str(source.parent),str(cfile),'-o',str(elf)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(RUN),str(elf)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
