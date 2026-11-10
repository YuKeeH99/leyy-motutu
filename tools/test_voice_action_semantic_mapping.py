"""Execute the production cloud parser to keep names separate from trajectories."""
import re
import json
import unittest

from test_lingxin_round_races import ROOT, COMMON, body, run_c

VOICE = 'apps/arcs-mini/services/service_voice_action.c'
GAIT = 'apps/arcs-mini/services/service_v2_gait.c'


class VoiceActionSemanticMappingTest(unittest.TestCase):
    def test_manual_covers_every_parsed_action_code_and_valid_examples(self):
        manual = (ROOT / 'doc/指令.md').read_text(encoding='utf-8')
        source = (ROOT / VOICE).read_text(encoding='utf-8')
        codes = set(re.findall(r'\{"([A-Z_]+)", SERVICE_V2_GAIT_ACTION_[A-Z_]+\}', source))
        for path, pattern in [
            ('apps/arcs-mini/services/voice_lingxin_action.c', r'"([A-Z_]+)"'),
            ('apps/arcs-mini/services/service_voice_query_policy.c', r'"(GET_[A-Z_]+)"'),
        ]:
            found = set(re.findall(pattern, (ROOT / path).read_text(encoding='utf-8')))
            codes.update(found - {'VOLUME'})
        codes.add('SAY_GOODBYE')
        for code in codes:
            self.assertIn('`' + code + '`', manual, code)
        example_codes = set()
        for block in re.findall(r'^```json\n(.*?)\n```', manual, re.M | re.S):
            for line in block.splitlines():
                value = json.loads(line)
                for item in value if isinstance(value, list) else []:
                    if 'action' in item:
                        self.assertIn(item['action'], codes)
                        example_codes.add(item['action'])
        self.assertTrue({'DOG_HANDSTAND', 'DOG_FRONTFLIP', 'DOG_BACKFLIP'} <= example_codes)

    def test_handstand_frontflip_and_legacy_codes_dispatch_original_cycles(self):
        source = (ROOT / VOICE).read_text(encoding='utf-8')
        table = re.search(r'static const voice_action_code_t s_codes\[\] = \{.*?\n\};',
                          source, re.S).group()
        services = ROOT / 'apps/arcs-mini/services'
        harness = COMMON
        harness += '\n#include "' + (ROOT / 'arcs-sdk/modules/cjson/cJSON.c').as_posix() + '"\n'
        harness += '\n#include "' + (services / 'service_v2_gait.h').as_posix() + '"\n'
        harness += '\n#include "' + (services / 'service_voice_action_policy.c').as_posix() + '"\n'
        harness += r'''
#define LISA_LOGW(...) ((void)0)
#define VOICE_ACTION_CLOUD_MAX_CYCLES 20U
typedef int service_voice_query_t;
typedef struct {const char *code; service_v2_gait_action_t action;} voice_action_code_t;
static unsigned s_cloud_action_count, s_cloud_unknown_count, calls, got_cycles;
static service_v2_gait_action_t got_action;
static unsigned service_offline_action_generation(void){return 1;}
static bool service_offline_action_online_current(unsigned g){return true;}
static bool service_voice_query_from_cloud_code(const char*c,service_voice_query_t*q){return false;}
static bool voice_query_submit(service_voice_query_t q){return true;}
static bool voice_lingxin_action_is_supported(const char*c){return false;}
static void voice_action_dispatch(service_v2_gait_action_t a,uint32_t c,uint8_t s,const char*n){
 calls++;got_action=a;got_cycles=c;
}
'''
        harness += table + '\n'
        for kind, name, args, path in [
            ('bool', 'code_equal', 'const char *a,const char *b', VOICE),
            ('bool', 'cloud_code_is_double_roll', 'const char *code', VOICE),
            ('bool', 'service_voice_action_from_cloud_code', 'const char *code,service_v2_gait_action_t *action', VOICE),
            ('uint32_t', 'service_v2_gait_default_cycles', 'service_v2_gait_action_t action', GAIT),
            ('uint32_t', 'service_v2_gait_cycle_ms', 'service_v2_gait_action_t action', GAIT),
            ('uint32_t', 'voice_action_default_cycles', 'service_v2_gait_action_t action', VOICE),
            ('void', 'voice_action_on_cloud', 'void *unused,uint32_t msg_id,void *data,uint32_t len,void *user_data', VOICE),
        ]:
            harness += f'\n{kind} {name}({args})\n' + body(path, name)
        run_c(harness + r'''
static void check(const char*code,service_v2_gait_action_t action,unsigned cycles){
 char json[256];
 /* COUNT/TIME/SPEED must not convert either calibrated trajectory. */
 snprintf(json,sizeof(json),"{\"action\":\"%s\",\"params\":{\"COUNT\":19,\"cycles\":19,\"TIME\":19,\"SPEED\":100}}",code);
 unsigned before=calls;
 voice_action_on_cloud(NULL,0,json,strlen(json),NULL);
 assert(calls==before+1 && got_action==action && got_cycles==cycles);
}
int main(void){
 const char *once[]={"DOG_HANDSTAND","dog_handstand","DOG_FLIP_ONCE","DOG_FLIP","FLIP_ONCE","ROLL_ONCE"};
 const char *twice[]={"DOG_FRONTFLIP","dog_frontflip","DOG_FLIP_CONTINUOUS","DOG_CONTINUOUS_FLIP","FLIP_CONTINUOUS","CONTINUOUS_FLIP","ROLL_DOUBLE"};
 for(unsigned i=0;i<sizeof(once)/sizeof(*once);i++)check(once[i],SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,1);
 for(unsigned i=0;i<sizeof(twice)/sizeof(*twice);i++)check(twice[i],SERVICE_V2_GAIT_ACTION_FULL_ROLL_LOOP,2);
 check("DOG_BACKFLIP",SERVICE_V2_GAIT_ACTION_BACKFLIP,1);
 check("BACKFLIP",SERVICE_V2_GAIT_ACTION_BACKFLIP,1);
 assert(!s_cloud_unknown_count);
}
''')


if __name__ == '__main__':
    unittest.main()
