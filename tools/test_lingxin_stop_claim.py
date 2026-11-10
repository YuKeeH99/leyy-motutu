"""Execute stop arbitration under local/cloud/termination interleavings."""
import unittest
from test_lingxin_round_races import COMMON, body, run_c

RECORDER = 'third_party/AI2T_LingXinEngine/src/voice_chat/recorder_manager.c'


class StopClaimTest(unittest.TestCase):
    def test_same_round_dedup_and_stale_waiter(self):
        code = COMMON + r'''
#define LINGXIN_RECORDER_CALLBACK_LIST_MAX_LENGTH 5
typedef void (*LingxinRecorderStopCallback)(bool);
typedef void (*lingxin_recorder_callback_t)(int);
static LingxinRecorderStopCallback temp_stop_callback_list[5], claimed_stop_callbacks[5];
#include <limits.h>
static int next_stop_ticket,temp_stop_callback_tickets[5];
static bool claimed_stop_persistent[5];
static unsigned temp_stop_callback_epochs[5],claimed_stop_epoch,epoch=7;
static int claimed_stop_count,temp_stop_callback_list_length;
static int record_close_mutex=1,record_close_callback_list_mutex=2,locks[3];
static int stop_pending_until_server_ready,server_ready=1,closes,normal,terminated;
static int record_close_stop_ticket;
static unsigned int record_close_stop_epoch;
static void *lingxin_recorder=(void*)1;
static bool switch_epoch_on_lock;
static bool restart_in_callback;
static int register_stop_callback_once(unsigned epoch,LingxinRecorderStopCallback callback,bool persistent);
static unsigned voice_chat_current_epoch(void) {return epoch;}
static void lingxin_mutex_lock(int m) {
 assert(!locks[m]);locks[m]=1;
 if(m==1 && switch_epoch_on_lock){epoch++;switch_epoch_on_lock=false;}
}
static void lingxin_mutex_unlock(int m){assert(locks[m]);locks[m]=0;}
static void lingxin_clean_recorder_prepare_stop(void *r,bool drain){}
static void record_close_callback_for_stop_wait_send_left(int r){lingxin_mutex_unlock(1);}
static void record_close_callback_for_stop(int r){lingxin_mutex_unlock(1);}
static void lingxin_recorder_close(void *r,lingxin_recorder_callback_t cb){closes++;cb(0);}
static void stopped(bool ok){
 assert(ok);normal++;
 if(restart_in_callback){restart_in_callback=false;epoch++;assert(register_stop_callback_once(epoch,stopped,true)==1);}
}
static void cancelled(bool ok){assert(ok);terminated++;}
'''
        for name, signature in [
            ('register_stop_callback_once', 'static int register_stop_callback_once(unsigned epoch,LingxinRecorderStopCallback callback,bool persistent)'),
            ('run_stop_callback', 'static void run_stop_callback(bool is_success, unsigned completing_epoch, int completing_ticket)'),
            ('module_record_stop_impl', 'static void module_record_stop(int wait_send_left,LingxinRecorderStopCallback stop_callback,bool persistent)'),
        ]:
            code += signature + body(RECORDER, name)
        code += r'''
int main(void){
 /* Local limit claimed first, cloud VAD arrives with close mutex occupied. */
 assert(register_stop_callback_once(epoch,stopped,true)==1);
 locks[1]=1;module_record_stop(1,stopped,true);locks[1]=0;
 assert(!closes && temp_stop_callback_list_length==1);
 run_stop_callback(true,epoch,INT_MAX);assert(normal==1);
 module_record_stop(1,stopped,true);assert(!closes && normal==1);
 /* Cloud first: local completion cannot register a second drain. */
 epoch++;module_record_stop(1,stopped,true);assert(closes==1);
 assert(register_stop_callback_once(epoch,stopped,true)==0);
 /* Termination must still be admitted while normal stop is pending. */
 module_record_stop(0,cancelled,false);assert(closes==2);
 run_stop_callback(true,epoch,INT_MAX);assert(normal==2 && terminated==1);
 /* A waiter that acquired the close lock in a later epoch must not close it. */
 epoch++;switch_epoch_on_lock=true;module_record_stop(1,stopped,true);
 assert(closes==2);run_stop_callback(true,epoch,INT_MAX);assert(normal==2);
 module_record_stop(1,stopped,true);assert(closes==3);
 run_stop_callback(true,epoch,INT_MAX);assert(normal==3);
 /* A completion may start another round, whose pending stop must survive. */
 epoch++;assert(register_stop_callback_once(epoch,stopped,true)==1);
 unsigned completed_epoch=epoch;restart_in_callback=true;
 run_stop_callback(true,completed_epoch,INT_MAX);
 assert(normal==4 && temp_stop_callback_list_length==1);
 run_stop_callback(true,completed_epoch,INT_MAX);assert(normal==4 && temp_stop_callback_list_length==1);
 run_stop_callback(true,epoch,INT_MAX);assert(normal==5);
 /* Terminate -> recorder reopens -> terminate in the same protocol epoch. */
 module_record_stop(0,cancelled,false);run_stop_callback(true,epoch,INT_MAX);
 assert(terminated==2);
 module_record_stop(0,cancelled,false);run_stop_callback(true,epoch,INT_MAX);
 assert(terminated==3);
 /* Old sender completion arrives after a same-epoch recorder restart. */
 int old_ticket=next_stop_ticket;
 module_record_stop(0,cancelled,false);
 assert(temp_stop_callback_list_length==1);
 run_stop_callback(true,epoch,old_ticket);
 assert(terminated==3 && temp_stop_callback_list_length==1);
 module_record_stop(0,cancelled,false); /* Pending request still deduplicates. */
 assert(temp_stop_callback_list_length==1);
 run_stop_callback(true,epoch,next_stop_ticket);assert(terminated==4);
 assert(!locks[1] && !locks[2]);
}
'''
        run_c(code, '-DCONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX')

    def test_immediate_cancel_cannot_be_changed_back_to_drain(self):
        run_c(COMMON + r'''
static int send_thread_mutex=1,send_thread_running=1,send_thread_stop_flag=1;
static int send_thread_stop_ticket;
static unsigned send_thread_stop_epoch=7;
static void *send_r_sem=(void*)1;
static int locked,completed;
static unsigned voice_chat_current_epoch(void){return 7;}
static void lingxin_mutex_lock(int m){assert(!locked);locked=1;}
static void lingxin_mutex_unlock(int m){assert(locked);locked=0;}
static void lingxin_semaphore_set_value(void *s,int v){}
static void lingxin_semaphore_post(void *s){}
static void lingxin_thread_sleep(int ms){assert(!locked);send_thread_running=0;}
static void cleanup_send_thread_sync_objects(void){}
static void run_stop_callback(bool ok,unsigned epoch,int ticket){completed++;}
static int stop_send_thread(int wait_send,unsigned int epoch,int ticket)
''' + body(RECORDER, 'stop_send_thread') + r'''
int main(void){
 stop_send_thread(1,7,1);assert(send_thread_stop_flag==1 && !completed && !locked);
 stop_send_thread(0,7,2);assert(!send_thread_running && send_thread_stop_flag==1 && !locked);
 send_thread_running=1;send_thread_stop_flag=0;
 stop_send_thread(1,6,1);assert(send_thread_stop_flag==0 && !completed && !locked);
}
''')


if __name__ == '__main__':
    unittest.main()
