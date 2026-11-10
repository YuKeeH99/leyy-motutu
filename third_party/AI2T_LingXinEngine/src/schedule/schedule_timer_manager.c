#include "chat_state_machine.h"
#include "lingxin_timer.h"
#include "schedule_ws_manager.h"
#include "schedule_timer_manager.h"
#include "lingxin_log.h"
#include "lingxin_mutex.h"
#include "lingxin_semaphore.h"
#include "lingxin_thread.h"
#include "lingxin_json_util.h"
#include <stdlib.h>
#include <string.h>
#include "lingxin_chat_api_inner.h"
#include "lingxin_memory.h"

typedef struct
{
  char *taskId;   // 任务唯一ID
  int timerId;    // 定时器ID（由系统分配）
  long countdown; // 倒计时时间（单位：毫秒或秒）
} TimerTask;

#define MAX_TIMER_TASKS 20

TimerTask timerTasks[MAX_TIMER_TASKS]; // 存储所有定时任务
int taskCount = 0;                     // 当前任务数量

static bool is_task_emit_error = false; // 定时任务是否触发失败,是否接收到error消息
static int is_schedule_task_on = 0;     // 定时任务是否开启

// 适配层代码包装
static void inner_delete_schedule_timer(int timerId);

// 更新任务队列相关
#define MAX_TASK_QUEUE 5
static ScheduleTaskList *task_queue[MAX_TASK_QUEUE];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_size = 0;
static lingxin_mutex_t queue_mutex = NULL;

// 定时器更新线程
static lingxin_semaphore_t timer_update_sem = NULL;
static lingxin_tid_t timer_update_thread_id = 0;
static void *timer_update_thread_entry(void *arg);
static void init_timer_update_thread();
static ScheduleTaskList *dequeue_task();
static bool enqueue_task(ScheduleTaskList *parseResult);
static bool set_timer(TimerTask *timerTask, int advance_connect_time);
static void updateTimerTaskList(TaskItem sync_task_list[], int task_num, int advance_connect_time);
void initTimerTaskList(char *scheduleStr);
static void clearTimerTask();
static void free_parse_result(ScheduleTaskList *parseResult);

// 定时器触发线程
static lingxin_semaphore_t timer_trigger_sem = NULL;
static lingxin_tid_t timer_trigger_thread_id = 0;
static bool is_timer_trigger_ready = true;
static void *timer_trigger_thread_entry();
static void init_timer_trigger_thread();
static void timer_callback(void *priv);
static void timer_trigger_func(void *timer_task);

#define MAX_TRIGGER_QUEUE_SIZE 10
typedef struct
{
  TimerTask *timer_tasks; // 存储定时器任务的数组
  int task_count;         // 任务计数（可能与size重复）
  int head;               // 队列头部索引
  int tail;               // 队列尾部索引
  int size;               // 当前队列中的元素数量
  int capacity;           // 队列最大容量
  lingxin_mutex_t mutex;  // 用于保护队列的互斥锁
} TimerTriggerQueue;
// 定时器触发队列
static TimerTriggerQueue timer_trigger_queue = {0};
static bool init_timer_trigger_queue();
static bool is_timer_task_exists(int timerId, const char *taskId);
static bool enqueue_timer_trigger(TimerTask *timer_task);
static TimerTask *dequeue_timer_trigger();
static void free_timer_trigger_task(TimerTask *task);

static bool is_update_thread_initialized = false;
static bool is_trigger_thread_initialized = false;
static bool is_trigger_queue_initialized = false;

/********************定时任务模块对外暴露时机 ********************/
void novoice_user_custom_listener(StateEvent event)
{
  switch (event)
  {
  case State_Event_NoVoice_TerminateEnd:
  {
    lingxin_log_ut(LINGXIN_DEBUG, "schedule_task_before_trigger");
    // 用户在开启NoVoice循环前注入的自定义action(关闭tts/asr等)
    lingxin_emit_chat_event(CHAT_LIFE_CYCLE_EVENT_SCHEDULE_EMIT, NULL);
    lingxin_log_ut(LINGXIN_DEBUG, "schedule_task_before_trigger_finish");
    break;
  }
  default:
  {
    break;
  }
  }
}
void recieve_schedule_task_error()
{
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_task_recv_trigger_error");
  is_task_emit_error = true;
}

/********************定时任务模块初始化 ********************/
void module_schedule_init()
{
  is_schedule_task_on = 1;
  init_timer_update_thread();
  init_timer_trigger_thread();
  initScheduleChat();
}
void initTimerTaskList(char *scheduleStr)
{
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_list_update_begin");
  // 0. 同步定时任务须满足的配置
  if (!is_schedule_task_on)
  {
    // 0-1. 端侧定时任务是否开启
    lingxin_log_ut_with_args(LINGXIN_ERROR, "schedule_list_update_failed", "schedule task not open");
    return;
  }
  if (!is_update_thread_initialized || !is_trigger_thread_initialized)
  {
    // 0-2. 定时任务相关线程是否初始化完成
    lingxin_log_ut_with_args(LINGXIN_ERROR, "schedule_list_update_failed", "thread init not ready");
    return;
  }
  if (!is_trigger_queue_initialized)
  {
    // 0-3. 定时任务相关线程是否初始化完成
    lingxin_log_ut_with_args(LINGXIN_ERROR, "schedule_list_update_failed", "trigger queue init not ready");
    return;
  }

  // 1. 控制在接收到error后的system_event之后才向状态机发送事件拉起voice循环
  if (is_task_emit_error)
  {
    state_machine_run_event(State_Event_NoVoice_Error);
    is_task_emit_error = false;
  }
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_list_parse_begin");

  // 2. 从scheduleStr中解析出定时任务列表
  ScheduleTaskList *parseRes = (ScheduleTaskList *)lingxin_calloc(1, sizeof(ScheduleTaskList));
  if (!parseRes)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "schedule_list_parse_failed", "malloc taskList failed");
    return;
  }
  if (parseScheduleTaskList(scheduleStr, parseRes) == 0)
  {

    // 3-1. 解析成功，将任务添加到处理队列
    if (enqueue_task(parseRes))
    {
      // 4-1. 入队成功，通知更新线程同步定时任务列表
      // lingxin_semaphore_set_value(timer_update_sem, 0);
      lingxin_semaphore_post(timer_update_sem); // 添加成功后通知处理线程
      lingxin_log_debug("after timer_update_sem post");
    }
    else
    {
      // 4-2. 入队失败，释放内存
      lingxin_log_error("添加任务到队列失败");
      free_parse_result(parseRes);
    }
  }
  else
  {
    // 3-2. 任务解析失败，释放内存
    lingxin_free(parseRes);
    parseRes = NULL;
  }
  lingxin_log_debug("initTimerTaskList finish");
}

/********************定时任务模块适配层方法包装 ********************/

static void inner_delete_schedule_timer(int timerId)
{
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_task_delete_begin");
  // 删除定时器
  if (lingxin_one_shot_timer_delete(timerId) == 0)
  {
    lingxin_log_ut(LINGXIN_DEBUG, "schedule_task_delete_finished");
  }
  else
  {
    lingxin_log_ut(LINGXIN_ERROR, "schedule_task_delete_failed");
  }
}

/******************定时器更新线程 ********************/
static void init_timer_update_thread() // 定时器更新线程初始化
{
  // 初始化同步对象
  if (queue_mutex == NULL)
  {
    queue_mutex = lingxin_mutex_create();
  }
  if (timer_update_sem == NULL)
  {
    timer_update_sem = lingxin_semaphore_create(0);
  }
  // 创建处理线程
  lingxin_thread_param_t thread_param = {
      .priority = 16,
      .stack_size = 2048 * 2,
      .name = "task_update",
  };
  int ret = lingxin_thread_create(&timer_update_thread_id, &thread_param, timer_update_thread_entry, NULL);
  if (ret == 0)
  {
    lingxin_log_debug("任务处理线程创建成功，PID: %d", timer_update_thread_id);
    is_update_thread_initialized = true; // 标记线程初始化完成
  }
  else
  {
    lingxin_log_error("任务处理线程创建失败，错误码: %d", ret);
    timer_update_thread_id = 0;
    is_update_thread_initialized = false;
  }
}
static void free_parse_result(ScheduleTaskList *parseResult)
{
  if (!parseResult)
  {
    return;
  }

  if (parseResult->tasks)
  {
    for (int i = 0; i < parseResult->taskCount; i++)
    {
      if (parseResult->tasks[i].taskId)
      {
        lingxin_free(parseResult->tasks[i].taskId);
        parseResult->tasks[i].taskId = NULL;
      }
    }
    lingxin_free(parseResult->tasks);
    parseResult->tasks = NULL;
  }
  lingxin_free(parseResult);
  parseResult = NULL;
}
static void *timer_update_thread_entry(void *arg) // 定时器更新线程入口
{
  lingxin_log_debug("定时器设置线程启动");
  while (1)
  {
    lingxin_log_debug("定时器设置线程 before pend");
    lingxin_semaphore_pend(timer_update_sem, 0);
    lingxin_log_debug("定时器设置线程 after pend");
    ScheduleTaskList *parseResult = dequeue_task();
    if (parseResult)
    {
      updateTimerTaskList(parseResult->tasks, parseResult->taskCount, parseResult->advanceConnectTime);
      free_parse_result(parseResult);
      lingxin_log_debug("定时任务线程单次设置完成");
    }
    lingxin_log_ut(LINGXIN_DEBUG, "schedule_list_update_finished");
  }
  lingxin_log_debug("定时器设置线程退出");
  return NULL;
}

static ScheduleTaskList *dequeue_task() // 定时任务更新队列出队操作，从队列头部取出任务
{
  lingxin_log_debug("dequeue_task start");
  ScheduleTaskList *parseRes = NULL;

  lingxin_mutex_lock(queue_mutex);

  if (queue_size > 0)
  {
    parseRes = task_queue[queue_head];
    queue_head = (queue_head + 1) % MAX_TASK_QUEUE;
    queue_size--;
  }
  lingxin_mutex_unlock(queue_mutex);
  lingxin_log_debug("dequeue_task finish");
  return parseRes;
}

static bool enqueue_task(ScheduleTaskList *parseResult) // 定时任务更新队列入队操作，添加任务到队列尾部
{
  lingxin_log_debug("enqueue_task start");
  bool result = false;
  if (!parseResult)
  {
    return false;
  }
  lingxin_mutex_lock(queue_mutex);
  // 如果队列已满，移除最旧的任务并释放其内存
  if (queue_size >= MAX_TASK_QUEUE)
  {
    lingxin_log_debug("full task queue, remove latest task");
    free_parse_result(parseResult);
    lingxin_mutex_unlock(queue_mutex);
    return false;
  }
  // 添加新任务
  task_queue[queue_tail] = parseResult;
  queue_tail = (queue_tail + 1) % MAX_TASK_QUEUE;
  queue_size++;
  result = true;
  lingxin_mutex_unlock(queue_mutex);
  lingxin_log_debug("enqueue task success, queue size: %d, queue head: %d, queue tail: %d", queue_size, queue_head, queue_tail);
  return result;
}
/********************定时器设置逻辑 ********************/
static void clearTimerTask() // 清空当前维护的计时器数组
{
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_list_clear_begin");
  int cleared_count = 0;
  for (int i = 0; i < taskCount && i < MAX_TIMER_TASKS; i++)
  {
    if (timerTasks[i].timerId != INVALID_TIMER_ID)
    {
      inner_delete_schedule_timer(timerTasks[i].timerId);
    }
    // ✅ 释放 taskId 内存
    if (timerTasks[i].taskId)
    {
      lingxin_free(timerTasks[i].taskId);
      timerTasks[i].taskId = NULL;
    }
    // 重置结构体内容
    timerTasks[i].timerId = INVALID_TIMER_ID;
    timerTasks[i].countdown = 0;
    cleared_count++;
  }
  taskCount = 0;
  lingxin_log_ut_with_args(LINGXIN_DEBUG, "schedule_list_clear_finished", "cleared %d tasks, current task count is %d",
                           cleared_count, taskCount);
}
static bool set_timer(TimerTask *timerTask, int advance_connect_time)
{
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_task_set_begin");
  timerTask->timerId = lingxin_one_shot_timer_create(timerTask, timer_callback, timerTask->countdown * 1000);
  if (timerTask->timerId == INVALID_TIMER_ID)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "schedule_task_set_failed", "invalid timerId");
    // 如果 taskId 分配了内存，但注册失败，应立即释放
    if (timerTask->taskId)
    {
      lingxin_free(timerTask->taskId);
      timerTask->taskId = NULL;
    }
    return false;
  }
  taskCount++;
  lingxin_log_ut_with_args(LINGXIN_DEBUG, "schedule_task_set_success", "taskId is %s, timerId is %d, finished task count is %d", timerTask->taskId, timerTask->timerId, taskCount);
  return true;
}
static void updateTimerTaskList(TaskItem sync_task_list[], int task_num, int advance_connect_time)
{
  clearTimerTask();
  lingxin_log_ut_with_args(LINGXIN_DEBUG, "schedule_list_sync_begin", "current parsed task total count is %d, advanced time is %d", task_num, advance_connect_time);

  // 根据传入的任务列表重新设置定时器
  for (int i = 0; i < task_num && i < MAX_TIMER_TASKS; i++)
  {
    lingxin_log_debug("定时任务列表添加任务 %d,任务id为%s，倒计时为%d", i, sync_task_list[i].taskId, sync_task_list[i].countdown);
    TaskItem *task = &sync_task_list[i];
    TimerTask *timer = &timerTasks[i];
    timer->taskId = task->taskId ? lingxin_strdup(task->taskId) : NULL;
    int countdownSec = task->countdown - advance_connect_time;
    timer->countdown = (countdownSec > 0) ? (long)countdownSec : 0;
  }
  // 基本属性设置完后再设置定时器
  for (int i = 0; i < task_num && i < MAX_TIMER_TASKS; i++)
  {
    TimerTask *timer = &timerTasks[i];
    set_timer(timer, advance_connect_time); // 设置软件定时器
  }
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_list_sync_finished");
}

/********************定时器触发线程 ********************/
static void *timer_trigger_thread_entry()
{
  while (1)
  {
    lingxin_log_debug("timer_trigger_thread_entry start");
    lingxin_semaphore_pend(timer_trigger_sem, 0);
    lingxin_log_debug("timer_trigger_thread_entry after pend");
    if (is_timer_trigger_ready)
    {
      is_timer_trigger_ready = false;
      TimerTask *timer_task = dequeue_timer_trigger();
      if (timer_task != NULL)
      {
        timer_trigger_func(timer_task);
        free_timer_trigger_task(timer_task);
      }
      is_timer_trigger_ready = true;
    }
    lingxin_log_debug("timer_trigger_thread_entry finish");
  }
  return NULL;
}
static void init_timer_trigger_thread()
{
  // 初始化定时器触发队列
  if (init_timer_trigger_queue())
  {
    is_trigger_queue_initialized = true;
    lingxin_log_debug("Timer threads initialized successfully");
  }
  else
  {
    lingxin_log_error("Failed to initialize timer trigger queue");
    is_trigger_queue_initialized = false;
  }

  if (timer_trigger_sem == NULL)
  {
    timer_trigger_sem = lingxin_semaphore_create(0);
  }
  // 创建定时任务触发线程
  lingxin_thread_param_t thread_param = {
      .priority = 16,
      .stack_size = 4096,
      .name = "timer_trigger",
  };
  int ret = lingxin_thread_create(&timer_trigger_thread_id, &thread_param, timer_trigger_thread_entry, NULL);
  if (ret == 0)
  { // 检查线程创建是否成功
    lingxin_log_debug("线程timer_trigger创建成功，PID: %d", timer_trigger_thread_id);
    is_trigger_thread_initialized = true;
  }
  else
  {
    lingxin_log_error("Error: 线程timer_trigger创建失败，错误码: %d", ret);
    timer_trigger_thread_id = 0;
    is_trigger_thread_initialized = false;
  }
}
/********************定时器触发逻辑 ********************/
static void timer_callback(void *priv)
{
  TimerTask *timer_task = (TimerTask *)priv;
  if (enqueue_timer_trigger(timer_task))
  {
    lingxin_semaphore_post(timer_trigger_sem);
    lingxin_log_debug("timer_trriger post semaphore");
  }
  else
  {
    lingxin_log_error("Failed to enqueue timer trigger task.");
  }
}
static void timer_trigger_func(void *timer_task)
{
  TimerTask *timer_task_info = (TimerTask *)timer_task;
  lingxin_log_ut_with_args(LINGXIN_DEBUG, "schedule_task_callback_begin", "trigger timerId: %d, trigger taskId: %s, countdown: %d", timer_task_info->timerId, timer_task_info->taskId, timer_task_info->countdown);

  if (!timer_task_info->taskId)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "schedule_task_callback_failed", "taskId is null");
    lingxin_log_debug("当前定时任务id为空%d", timer_task_info->taskId);
    return;
  }
  if (timer_task_info->timerId == INVALID_TIMER_ID)
  {
    lingxin_log_ut_with_args(LINGXIN_ERROR, "schedule_task_callback_failed", "timerId is invalid");
    lingxin_log_debug("当前定时任务倒计时id为无效timerId\n");
    return;
  }
  // 定时器触发后的处理逻辑
  inner_delete_schedule_timer(timer_task_info->timerId);
  lingxin_log_ut(LINGXIN_DEBUG, "schedule_task_trigger_success");
  ScheduleTimerPayload schedule_trigger_payload = {0};
  if (timer_task_info->taskId != NULL)
  {
    schedule_trigger_payload.schedule_task_id = lingxin_strdup(timer_task_info->taskId);
    if (schedule_trigger_payload.schedule_task_id == NULL)
    {
      lingxin_log_error("scheduleTaskId内存分配失败");
    }
  }
  schedule_trigger_payload.input_mode = "no_voice"; 
  StateEventPayload payload = {
        .schedule_timer_payload = &schedule_trigger_payload};
  state_machine_run_event_with_payload(State_Event_NoVoice_Start, &payload);
}

// 定时器触发队列相关函数
static bool init_timer_trigger_queue()
{
  if (timer_trigger_queue.timer_tasks != NULL)
  {
    return true; // 已经初始化
  }

  timer_trigger_queue.timer_tasks = (TimerTask *)lingxin_calloc(MAX_TRIGGER_QUEUE_SIZE, sizeof(TimerTask));
  if (timer_trigger_queue.timer_tasks == NULL)
  {
    lingxin_log_error("Failed to allocate memory for timer trigger queue");
    return false;
  }

  timer_trigger_queue.capacity = MAX_TRIGGER_QUEUE_SIZE;
  timer_trigger_queue.task_count = 0;
  timer_trigger_queue.head = 0;
  timer_trigger_queue.tail = 0;
  timer_trigger_queue.size = 0;
  timer_trigger_queue.mutex = lingxin_mutex_create();

  if (timer_trigger_queue.mutex == NULL)
  {
    lingxin_log_error("Failed to create mutex for timer trigger queue");
    lingxin_free(timer_trigger_queue.timer_tasks);
    timer_trigger_queue.timer_tasks = NULL;
    return false;
  }

  return true;
}
static bool is_timer_task_exists(int timerId, const char *taskId)
{
  for (int i = 0; i < timer_trigger_queue.size; i++)
  {
    int index = (timer_trigger_queue.head + i) % timer_trigger_queue.capacity;
    if (timer_trigger_queue.timer_tasks[index].timerId == timerId)
    {
      return true;
    }
    // 检查 taskId 是否重复（两个都非空且相等）
    if (taskId && timer_trigger_queue.timer_tasks[index].taskId &&
        strcmp(taskId, timer_trigger_queue.timer_tasks[index].taskId) == 0)
    {
      return true;
    }
  }
  return false;
}

static bool enqueue_timer_trigger(TimerTask *timer_task)
{
  if (!timer_task || timer_task->timerId == INVALID_TIMER_ID || timer_task->taskId == NULL)
  {
    return false;
  }
  lingxin_mutex_lock(timer_trigger_queue.mutex);
  // 检查是否已存在相同timerId或相同taskId的任务
  if (is_timer_task_exists(timer_task->timerId, timer_task->taskId))
  {
    lingxin_log_debug("Timer task with timerId %d already exists in queue, skipping", timer_task->timerId);
    lingxin_mutex_unlock(timer_trigger_queue.mutex);
    return false;
  }

  // 如果队列已满，丢弃最新触发的任务
  if (timer_trigger_queue.size >= timer_trigger_queue.capacity)
  {
    lingxin_log_debug("Timer trigger queue is full, discarding new task to keep queue unchanged");
    free_timer_trigger_task(timer_task);// 释放新任务的内存
    lingxin_mutex_unlock(timer_trigger_queue.mutex);
    return false;
  }

  // 添加新任务到队尾
  TimerTask *queue_task = &timer_trigger_queue.timer_tasks[timer_trigger_queue.tail];
  queue_task->timerId = timer_task->timerId;
  queue_task->countdown = timer_task->countdown;
  if (timer_task->taskId)
  {
    queue_task->taskId = lingxin_strdup(timer_task->taskId);
  }
  else
  {
    queue_task->taskId = NULL;
  }

  timer_trigger_queue.tail = (timer_trigger_queue.tail + 1) % timer_trigger_queue.capacity;
  timer_trigger_queue.size++;

  lingxin_mutex_unlock(timer_trigger_queue.mutex);

  lingxin_log_debug("Enqueued timer task, timerId: %d, taskId: %s, queue size: %d",
                    timer_task->timerId, timer_task->taskId ? timer_task->taskId : "NULL", timer_trigger_queue.size);

  return true;
}

static TimerTask *dequeue_timer_trigger()
{
  lingxin_mutex_lock(timer_trigger_queue.mutex);

  if (timer_trigger_queue.size <= 0)
  {
    lingxin_mutex_unlock(timer_trigger_queue.mutex);
    return NULL;
  }

  TimerTask *task = &timer_trigger_queue.timer_tasks[timer_trigger_queue.head];
  timer_trigger_queue.head = (timer_trigger_queue.head + 1) % timer_trigger_queue.capacity;
  timer_trigger_queue.size--;

  lingxin_mutex_unlock(timer_trigger_queue.mutex);
  lingxin_log_debug("dequeued timer task, timerId: %d, taskId: %s, queue size: %d",
                    task->timerId, task->taskId ? task->taskId : "NULL", timer_trigger_queue.size);

  return task;
}
static void free_timer_trigger_task(TimerTask *task)
{
  if (task && task->taskId)
  {
    lingxin_free(task->taskId);
    task->taskId = NULL;
  }
}