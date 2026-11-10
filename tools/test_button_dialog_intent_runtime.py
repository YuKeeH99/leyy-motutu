"""Run the real physical button publisher and application dialog dispatch in C."""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_lingxin_round_races import ROOT, body, run_c

MAIN = "apps/arcs-mini/main.c"
SERVICE = "apps/arcs-mini/services/service_button.c"
SERVER = "src/server/lingxin_server/lingxin_server.c"


def definition(source, name):
    match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\([^;]+?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(name)
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def fixture():
    msg = (ROOT / "src/framework/voice_msg.h").read_text(encoding="utf-8")
    begin = msg.rindex("typedef enum", 0, msg.index("VOICE_MSG_BUTTON_ACTION_PRESS_DOWN"))
    end = msg.index("} voice_msg_button_evt_t;", begin) + len("} voice_msg_button_evt_t;")
    server_h = (ROOT / "src/server/lingxin_server/lingxin_server.h").read_text(encoding="utf-8")
    s = server_h.rindex("typedef enum", 0, server_h.index("LINGXIN_DIALOG_PHASE_UNAVAILABLE"))
    e = server_h.index("} lingxin_dialog_phase_t;", s) + len("} lingxin_dialog_phase_t;")
    types = msg[begin:end] + "\n" + server_h[s:e]
    header = (ROOT / "apps/arcs-mini/services/service_button.h").read_text(encoding="utf-8")
    types += "\n" + "\n".join(line for line in header.splitlines() if not line.startswith("#"))
    source = "#include <stdbool.h>\n#include <stdint.h>\n" + types + "\n"
    source += (ROOT / "tools/host_tests/button_dialog_intent_fixture.h").read_text(encoding="utf-8")
    source += "\n#include \"" + str(ROOT / "src/middleware/button/lisa_btn.h").replace("\\", "/") + "\"\n"
    source += "lingxin_dialog_phase_t lingxin_server_dialog_phase(void)\n" + body(SERVER, "lingxin_server_dialog_phase")
    service = (ROOT / SERVICE).read_text(encoding="utf-8")
    if "service_button_snapshot_event" in service:
        source += r'''
static uint32_t s_button_gesture_id;
static service_button_event_t s_button_press;
typedef enum {SERVICE_OFFLINE_BUTTON_REJECTED=0,SERVICE_OFFLINE_BUTTON_ONLINE=1,SERVICE_OFFLINE_BUTTON_OFFLINE=2} service_offline_button_result_t;
static service_offline_button_result_t service_offline_action_begin_button_for_generation(unsigned expected,unsigned*accepted){
 *accepted=0;if(expected!=owner || !claim_result)return SERVICE_OFFLINE_BUTTON_REJECTED;
 claims++;*accepted=++owner;return claim_result;
}
'''
        for name in ("service_button_snapshot_event", "service_button_dialog_click_current", "service_button_wakeup_event_current", "service_button_wakeup_request"):
            source += definition(service, name)
    source += definition(service, "map_lisa_btn_event")
    source += definition(service, "publish_button_event")
    main = (ROOT / MAIN).read_text(encoding="utf-8")
    for name in ("app_try_handle_ota_package_info", "app_lingxin_single_click_execute", "app_lingxin_single_click_retry_task", "app_lingxin_single_click_defer", "button_changed"):
        if name in main:
            source += definition(main, name)
    source += r'''
static void dispatch(void){assert(queued_size);button_changed(NULL,0,queued_event,queued_size,NULL);queued_size=0;}
static void physical(lisa_btn_event_t action){publish_button_event(0,action);if(queued_size)dispatch();}
static void finish_click(void){physical(LISA_BTN_PRESS_SHORT_UP);physical(LISA_BTN_PRESS_CLICK);}
'''
    return source


def consumer_fixture():
    source = fixture() + r'''
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
static unsigned registered_owner,starts;
static bool app_voice_interaction_blocked(void){return false;}
static bool service_offline_action_begin_button(void){claims++;owner++;return true;}
static bool voice_wakeup_should_open_info(uint32_t*p){return false;}
static void voice_wakeup_prompt_cloud_info(uint32_t p){assert(0);}
static void interrupt_photo_flow(const char*r){}
static void lingxin_server_diag_timing_begin(const char*r){}
static void lingxin_server_diag_timing_mark(const char*r,int n){}
static unsigned lingxin_server_audio_uplink_gate_close(const char*r){return 1;}
static void start_lingxin_chat_async_for_owner(const char*r,uint32_t accepted){registered_owner=accepted;starts++;}
'''
    source += definition((ROOT / "src/category/comm/msgs/voice_wakeup_msg.c").read_text(encoding="utf-8"), "voice_msg_btn_wakeup_start")
    return source


class ButtonDialogIntentRuntime(unittest.TestCase):
    def test_listening_intent_stops_playback_that_started_before_click(self):
        state_path = "third_party/AI2T_LingXinEngine/src/voice_chat/chat_state_machine.c"
        state_source = (ROOT / state_path).read_text(encoding="utf-8")
        a = state_source.rindex("  case State_Event_WillExit:")
        b = state_source.index("  case State_Event_VoiceChat_ExitEnd:", a)
        chain = r'''
#define LOGI(...) ((void)0)
#define lingxin_log_ut(...) ((void)0)
#define lingxin_log_debug(...) ((void)0)
#define LINGXIN_PROPS_INIT_TAG "init"
typedef int StateEvent;
enum {State_Event_WillExit=100};
typedef struct {const char*props_init_tag;bool disable_close_ws_immediately;} ExitChatProps;
typedef struct {bool disable_close_ws_immediately;} WillExitPayload;
typedef struct {WillExitPayload*will_exit_payload;} StateEventPayload;
typedef struct {bool record_terminated,buffer_play_terminated,voice_chat_terminated,voice_chat_exited,is_normal_exit;} InnerStateForExit;
typedef struct {InnerStateForExit*inner_state_for_exit;} InnerStateCollection;
typedef struct {int unused;} ChatStateRuntimeContext;
static bool s_lingxin_inited=true,audio_live=true;
static unsigned player_terminations,upload_terminations,close_requests;
static void player_stop(void){audio_live=false;player_terminations++;}
static struct {void(*terminate)(void);} player={player_stop},*s_player=&player;
static void state_machine_run_event_with_payload(int event,StateEventPayload*payload);
static void turn_to_with_preset_inner_state(int s,int event,InnerStateCollection*p);
static void upload_manager_terminate(void){upload_terminations++;}
static void module_voiceChat_terminate(void){}
static bool module_voiceChat_exit(void){close_requests++;return true;}
static void state_task_complete_receive_event(int e,StateEventPayload*p){assert(0);}
static ExitChatProps get_exit_chat_default_props(void){ExitChatProps p={LINGXIN_PROPS_INIT_TAG,false};return p;}
'''
        chain += definition((ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/state_module_manager/state_donwload_manager.c").read_text(encoding="utf-8"), "playback_manager_terminate")
        chain = chain.replace("enum {State_Event_WillExit=100};", "enum {State_Event_WillExit=100,State_Event_BufferPlay_TerminateEnd};")
        chain += definition((ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/task_state_strategies/state_task_exit_strategies.c").read_text(encoding="utf-8"), "turn_to_exit")
        chain += r'''
static void turn_to_with_preset_inner_state(int s,int e,InnerStateCollection*p){state=s;turn_to_exit(NULL,p,e);}
static void state_machine_run_event_with_payload(int event,StateEventPayload*payload){
 stops++;
#define chat_current_state state
 switch(event){
''' + state_source[a:b] + r'''
 default:assert(0);}}
'''
        chain += definition((ROOT / "third_party/AI2T_LingXinEngine/src/voice_chat/chat_api.c").read_text(encoding="utf-8"), "exit_chat")
        chain += definition((ROOT / SERVER).read_text(encoding="utf-8"), "lingxin_server_chat_stop")
        source = fixture().replace("static int lingxin_server_chat_stop(void){stops++;return 0;}", chain)
        run_c(source + r'''
int main(void){state=State_Upload_Transfer;physical(LISA_BTN_PRESS_DOWN);
 state=State_Download_Play;finish_click();
 assert(!wakes && stops==1 && exit_prompt==1 && !suppressed);
 assert(!audio_live && player_terminations==1 && upload_terminations==1 && close_requests==1);}
''')

    def test_local_exit_and_owner_validation_form_one_atomic_decision(self):
        run_c(fixture() + r'''
int main(void){state=State_Download_Play;physical(LISA_BTN_PRESS_DOWN);physical(LISA_BTN_PRESS_SHORT_UP);
 publish_button_event(0,LISA_BTN_PRESS_CLICK);local_exit_active=true;inject_on_unlock=true;dispatch();
 assert(cancelled_owner==10 && owner==11 && !wakes && !stops && !critical_depth);}
''')

    def test_press_snapshot_precedes_event_bus_delivery(self):
        run_c(fixture() + r'''
int main(void){state=State_Download_Play;publish_button_event(0,LISA_BTN_PRESS_DOWN);
 state=State_Upload_Transfer;dispatch();finish_click();assert(wakes==1 && !stops && !suppressed);}
''')

    def test_retired_click_cannot_cancel_a_new_local_owner(self):
        run_c(fixture() + r'''
int main(void){state=State_Download_Play;physical(LISA_BTN_PRESS_DOWN);
 physical(LISA_BTN_PRESS_SHORT_UP);publish_button_event(0,LISA_BTN_PRESS_CLICK);
 owner++;offline_exit_calls=0;dispatch();
 assert(!wakes && !stops && !claims && !offline_exit_calls);}
''')

    def test_accepted_owner_is_shared_without_subscription_order_dependency(self):
        run_c(consumer_fixture() + r'''
int main(void){state=State_Download_Play;
 service_button_wakeup_t copy;
 for(int controller_position=0;controller_position<5;controller_position++){
  physical(LISA_BTN_PRESS_DOWN);finish_click();memcpy(&copy,wake_payload,sizeof(copy));
  unsigned expected=owner,claim_count=claims;
  for(int subscriber=0;subscriber<5;subscriber++){
   if(subscriber==controller_position)voice_msg_btn_wakeup_start(NULL,VOICE_MSG_WAKEUP_BUTTON_START,&copy,sizeof copy,NULL);
   else assert(service_button_wakeup_event_current(&copy,sizeof copy));
   assert(claims==claim_count && owner==expected);
  }
  assert(registered_owner==expected && starts==(unsigned)controller_position+1);
 }
 /* A new, not-yet-confirmed PRESS does not partially retire an accepted wake. */
 physical(LISA_BTN_PRESS_DOWN);assert(service_button_wakeup_event_current(&copy,sizeof copy));
 owner++; /* A newly accepted keyword/command does retire it. */
 assert(!service_button_wakeup_event_current(&copy,sizeof copy));
 voice_msg_btn_wakeup_start(NULL,VOICE_MSG_WAKEUP_BUTTON_START,&copy,sizeof copy,NULL);assert(starts==5);
 assert(service_button_wakeup_event_current(NULL,0));
 voice_msg_btn_wakeup_start(NULL,VOICE_MSG_WAKEUP_BUTTON_START,NULL,0,NULL);
 assert(starts==6 && claims==6 && registered_owner==owner);
 assert(!service_button_wakeup_event_current(&copy,1));}
''')

    def test_every_owned_wake_consumer_checks_before_side_effects(self):
        checks = (
            ("apps/arcs-mini/services/service_voice_action.c", "voice_action_on_wakeup", "voice_player_beat_music_cancel"),
            ("apps/arcs-mini/services/service_v2_rgb_effect.c", "rgb_effect_on_voice", "rgb_effect_event_from_voice"),
            ("apps/arcs-mini/services/service_v2_voice_standby.c", "standby_on_voice", "STANDBY_NOTIFY_ACTIVITY"),
            ("src/category/comm/msgs/voice_wakeup_msg.c", "voice_msg_btn_wakeup_start", "get_app_datas"),
        )
        for path, name, first_side_effect in checks:
            actual = definition((ROOT / path).read_text(encoding="utf-8"), name)
            self.assertLess(actual.index("service_button_wakeup_event_current"), actual.index(first_side_effect))
        player = (ROOT / "apps/arcs-mini/voice_player.c").read_text(encoding="utf-8")
        case = player[player.index("case VOICE_MSG_WAKEUP_BUTTON_START:"):]
        self.assertLess(case.index("service_button_wakeup_event_current"), case.index("voice_player_begin_wakeup_gate"))

    def test_double_triple_and_long_hold_do_not_start_single_click(self):
        run_c(fixture() + r'''
int main(void){state=State_Download_Play;
 physical(LISA_BTN_PRESS_DOWN);physical(LISA_BTN_PRESS_SHORT_UP);physical(LISA_BTN_PRESS_SHORT_UP);
 physical(LISA_BTN_PRESS_DOUBLE_CLICK);assert(assembly==1 && !wakes);
 physical(LISA_BTN_PRESS_DOWN);physical(LISA_BTN_PRESS_SHORT_UP);physical(LISA_BTN_PRESS_TRIPLE_CLICK);
 assert(provisioning==1 && !wakes);
 physical(LISA_BTN_PRESS_DOWN);physical(LISA_BTN_PRESS_SHORT_START);physical(LISA_BTN_PRESS_LONG_START);
 physical(LISA_BTN_PRESS_LONG_HOLD);assert(shutdowns==1 && !wakes);
 physical(LISA_BTN_PRESS_LONG_HOLD_UP);physical(LISA_BTN_PRESS_CLICK);assert(!wakes);
 physical(LISA_BTN_PRESS_DOWN);finish_click();assert(wakes==1 && claims==1);}
''')

    def test_ota_confirmation_with_filtered_press_is_preserved(self):
        run_c(fixture() + r'''
int main(void){ota=OTA_STATE_PACKAGE_INFO;physical(LISA_BTN_PRESS_DOWN);assert(!queued_size);
 physical(LISA_BTN_PRESS_CLICK);assert(ota_confirms==1 && !claims && !wakes);
 physical(LISA_BTN_PRESS_LONG_HOLD);assert(ota_skips==1 && !shutdowns);
 ota=OTA_STATE_IDLE;physical(LISA_BTN_PRESS_DOWN);finish_click();assert(wakes==1);}
''')

    def test_gates_offline_handoff_and_publish_failure_do_not_fake_success(self):
        run_c(fixture() + r'''
int main(void){state=State_Idle;app.can_wakeup=false;
 physical(LISA_BTN_PRESS_DOWN);finish_click();assert(!claims && !wakes);
 app.can_wakeup=true;app.voice_work_mode=0;physical(LISA_BTN_PRESS_DOWN);finish_click();assert(!claims);
 app.voice_work_mode=1;claim_result=2;physical(LISA_BTN_PRESS_DOWN);finish_click();
 assert(claims==1 && !wakes && !closed_owner); /* Offline mailbox owns it. */
 claim_result=1;publish_result=-1;physical(LISA_BTN_PRESS_DOWN);finish_click();
 assert(claims==2 && !wakes && closed_owner==owner);
 publish_result=0;physical(LISA_BTN_PRESS_DOWN);finish_click();assert(claims==3 && wakes==1);}
''')

    def test_speaking_press_survives_natural_transition_to_listening(self):
        run_c(fixture() + r'''
int main(void){state=State_Download_Play;physical(LISA_BTN_PRESS_DOWN);
 assert(!wakes && !stops);state=State_Upload_Transfer;finish_click();
 assert(wakes==1 && stops==0 && !end_record && !exit_prompt);}
''')

    def test_transition_press_never_turns_into_listening_exit(self):
        run_c(fixture() + r'''
int main(void){state=State_Task_Complete;physical(LISA_BTN_PRESS_DOWN);finish_click();
 if(scheduled_worker){change_on_delay=true;scheduled_worker(scheduled_arg);}
 assert(wakes==1 && stops==0 && !end_record && !exit_prompt);}
''')

    def test_listening_press_preserves_deliberate_exit(self):
        run_c(fixture() + r'''
int main(void){state=State_Upload_Transfer;physical(LISA_BTN_PRESS_DOWN);finish_click();
 assert(!wakes && stops==1 && end_record==1 && exit_prompt==1);}
''')


if __name__ == "__main__":
    unittest.main()
