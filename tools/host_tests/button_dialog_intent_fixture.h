
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_VOICE_LINK_LINGXIN 1
#define CONFIG_LINGXIN_ENGINE_ENABLE 1
#define CONFIG_BOARD_ARCS_MINI_V2_1 1
#define CONFIG_LISA_CAMERA_DEVICE 0
#define CONFIG_LISA_DISPLAY_DEVICE 0
#define LISA_LOGI(...) ((void)0)
#define LISA_LOGW(...) ((void)0)
#define LISA_LOGE(...) ((void)0)
#define LINGXIN_BUTTON_TRANSITION_RETRY_MS 50U
#define LINGXIN_BUTTON_TRANSITION_RETRY_COUNT 40U
#define LINGXIN_BUTTON_RETRY_TASK_STACK 2048
#define LINGXIN_BUTTON_RETRY_TASK_PRIORITY 5
#define pdMS_TO_TICKS(n) (n)
#define pdPASS 1
#define QR_STATUS_CONNECTED 1
#define UBOOT_FEATURE_POWER_GUARD 1
enum {State_Idle,State_Upload_Init,State_Upload_Transfer,State_Welcome,
 State_Download_Init,State_Download_Play,State_Download_End,State_NoVoice_Start,
 State_NoVoice_Terminate,State_Terminate,State_Exit,State_Task_Complete};
enum {VOICE_MSG_WAKEUP_BUTTON_START=1,VOICE_MSG_BUTTON_CHANGE=2};
static int state=State_Idle,wakes,stops,end_record,exit_prompt,suppressed,threads,deletes;
static unsigned elapsed_ms;
static bool change_on_delay, s_lingxin_button_click_retry_pending;
static void (*scheduled_worker)(void*);
static void *scheduled_arg;
static int get_current_chat_state(void){return state;}
static _Alignas(uint32_t) unsigned char queued_event[128],wake_payload[128];
static unsigned queued_size,wake_size;
static int publish_result;
static int voice_msg_pub(uint32_t id,void*d,uint32_t n){
 if(id==VOICE_MSG_BUTTON_CHANGE){assert(n<=sizeof queued_event);memcpy(queued_event,d,n);queued_size=n;return 0;}
 assert(id==VOICE_MSG_WAKEUP_BUTTON_START);if(publish_result)return publish_result;
 assert(n<=sizeof wake_payload);if(n)memcpy(wake_payload,d,n);wake_size=n;wakes++;return 0;
}
static void lingxin_server_suppress_current_tts(const char*why){suppressed++;}
static int lingxin_server_chat_end_record(void){end_record++;return 0;}
static int lingxin_server_chat_stop(void){stops++;return 0;}
static bool lingxin_server_chat_is_active(void){return state!=State_Idle;}
static void app_play_dialog_exit_prompt(void){exit_prompt++;}
static void vTaskDelay(unsigned ms){elapsed_ms+=ms;if(change_on_delay){state=State_Upload_Transfer;change_on_delay=false;}}
static void vTaskDelete(void*p){assert(!p);deletes++;}
static int xTaskCreate(void(*f)(void*),const char*n,unsigned stack,void*arg,unsigned priority,void*out){
 assert(!scheduled_worker);scheduled_worker=f;scheduled_arg=arg;threads++;return pdPASS;
}
static unsigned owner;
static int offline_exit_calls;
static bool local_exit_active;
static unsigned cancelled_owner;
static bool service_offline_action_button_exit(int action){offline_exit_calls++;
 if(action==VOICE_MSG_BUTTON_ACTION_CLICK && local_exit_active){cancelled_owner=owner;return true;}return false;}
static int service_v2_voice_standby_request(void){assert(0);return 0;}
static void service_v2_voice_standby_note_activity(void){}
static void app_clear_all_wifi_and_enter_netcfg_async(void){assert(0);}
static int provisioning;static void app_enter_web_netcfg_by_button(void){provisioning++;}
static int assembly; static int service_v2_voice_standby_wake_pose(bool b){assembly++;return 0;}
static bool app_button_action_should_redirect_cloud_info(int action){return false;}
static bool app_should_open_info_by_cloud_state(uint32_t*status){return false;}
static void app_prompt_cloud_info(uint32_t status){assert(0);}
static bool alarm_ring_is_active(void){return false;}
static void alarm_handle_snooze(void){assert(0);}
static void alarm_ring_stop(void){assert(0);}
static void app_ble_netcfg_enter_async(void){assert(0);}
static void factory_reset(void){assert(0);}
static void alarm_handle_stop_and_next(void){assert(0);}
static bool uboot_features_has(int n){return true;}
static bool power_is_usb_plugged(void){return false;}
static int shutdowns;static bool power_request_shutdown(void){shutdowns++;return true;}

#define CONFIG_OTA 1
#define taskENTER_CRITICAL() (critical_depth++)
#define taskEXIT_CRITICAL() fixture_unlock()
#define VOICE_WORK_MODE_BUTTON_WAKEUP 1
static unsigned owner=10,claims,closed_owner,claim_result=1;
static int critical_depth;
static bool inject_on_unlock;
static void fixture_unlock(void){assert(critical_depth>0);if(--critical_depth==0 && inject_on_unlock){owner++;inject_on_unlock=false;}}
static bool sync_blocked;
struct app_datas {unsigned voice_work_mode;bool can_wakeup;};
static struct app_datas app={1,true};
static struct app_datas*get_app_datas(void){return &app;}
static bool service_sd_music_is_syncing(void){return sync_blocked;}
static void service_image_waiting_cancel(void){}
static unsigned service_offline_action_generation(void){return owner;}
static bool service_offline_action_online_current(unsigned g){return g==owner;}
static void service_offline_action_close_online_window(unsigned g){if(owner==g)closed_owner=g;}
enum {OTA_STATE_IDLE,OTA_STATE_CHECKING,OTA_STATE_UPDATING,OTA_STATE_PACKAGE_INFO};
typedef int ota_state_e;static ota_state_e ota;
static ota_state_e ota_manager_get_state(void){return ota;}
static unsigned ota_confirms,ota_skips;
static bool ota_manager_app_update_input_ready(void){return true;}
static int ota_manager_confirm_app_update(void){ota_confirms++;return 0;}
static int ota_manager_skip_app_update(void){ota_skips++;return 0;}
