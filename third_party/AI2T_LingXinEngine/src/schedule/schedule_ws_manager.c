#include "schedule_first_ws.h"
#include "schedule_ws_manager.h"
#include "chat_state_machine.h"
#include "lingxin_voice_chat_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lingxin_common.h"
#include "lingxin_log.h"
#include "lingxin_memory.h"

extern char *generateUUID(int length);

static ScheduleChatConfig *scheduleConfig = NULL;
static bool isFirstExec = true;

static void getConfig()
{
  if (scheduleConfig == NULL)
  {
    scheduleConfig = (ScheduleChatConfig *)lingxin_calloc(1, sizeof(ScheduleChatConfig));
  }
  char *appKey = lingxin_auth_license_get();
  if (!appKey || strlen(appKey) == 0)
  {
    lingxin_log_error("license is null, please check lingxin_auth_license_get() implementation");
    return;
  }
  char *sn = lingxin_auth_sn_get();
  if (!sn || strlen(sn) == 0)
  {
    lingxin_log_error("sn is null, please check lingxin_auth_sn_get() implementation");
    return;
  }
  char *appId = lingxin_auth_appId_get();
  if (!appId || strlen(appId) == 0)
  {
    lingxin_log_error("appId is null, please check lingxin_auth_appId_get() implementation");
    return;
  }
  char *agentCode = lingxin_auth_appCode_get();
  if (!agentCode || strlen(agentCode) == 0)
  {
    lingxin_log_error("appCode is null, please check lingxin_auth_appCode_get() implementation");
    return;
  }

  scheduleConfig->serverPath = WEBSOCKET_CHAT_PATH;
  scheduleConfig->appKey = appKey;
  scheduleConfig->sn = sn;
  scheduleConfig->appId = appId;
  scheduleConfig->showLog = true;
  scheduleConfig->taskId = generateUUID(32);
}

static void systemEventListener(ScheduleChatHandler *globalHandler, const char *data)
{
  lingxin_log_debug("-----SCHEDULECHAT_EVENT_ON_SYSTEM_EVENT-----%s", data);
  state_machine_receive_schedule_data((void *)data);
}

static void doCreate()
{
  getConfig();
  startFirstScheduleConnect(scheduleConfig, systemEventListener);
}

void initScheduleChat()
{
  if (isFirstExec)
  {
    isFirstExec = false;
    doCreate();
  }
}