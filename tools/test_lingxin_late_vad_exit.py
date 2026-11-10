"""A cloud no-input event may arrive after the local duration boundary."""
import unittest
from test_lingxin_round_races import COMMON, body, run_c


class LateVadExitTest(unittest.TestCase):
    def test_late_vad_exit_marks_downlink_round_for_exit(self):
        code=COMMON+r'''
enum {State_Idle,State_Upload_Transfer,State_Download_Init,State_Download_Play,
 State_Download_End,State_Task_Complete,State_Terminate};
static int chat_current_state, marks, completes;
typedef int StateEvent;
typedef void StateEventPayload;
static void state_upload_transfer_receive_event(int e,void *p) {marks++;}
static void *get_current_context(void) {return NULL;}
static void turn_to_task_complete(void *c,void *p) {completes++;}
static void state_machine_handle_vad_exit(StateEvent event,StateEventPayload *payload)
'''+body('third_party/AI2T_LingXinEngine/src/voice_chat/chat_state_machine.c','state_machine_handle_vad_exit')+r'''
int main(void) {
 int active[]={State_Upload_Transfer,State_Download_Init,State_Download_Play,State_Download_End,State_Task_Complete};
 for(int i=0;i<5;i++){chat_current_state=active[i];state_machine_handle_vad_exit(0,NULL);assert(marks==i+1);}
 assert(completes==1);
 chat_current_state=State_Terminate;state_machine_handle_vad_exit(0,NULL);assert(marks==5);
 chat_current_state=State_Idle;state_machine_handle_vad_exit(0,NULL);assert(marks==5);
}
'''
        run_c(code)
