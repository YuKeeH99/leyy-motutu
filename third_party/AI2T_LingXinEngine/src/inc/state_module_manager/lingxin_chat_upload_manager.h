#ifndef LINGXIN_CHAT_UPLOAD_MANAGER_H
#define LINGXIN_CHAT_UPLOAD_MANAGER_H

// chat_api 调用，直接停用 上行模块录音
void lingxin_chat_upload_manager_stop_record();
void lingxin_chat_upload_manager_poll_record_limit(void);

// 本地 VAD 判定整轮无人声时，停止录音并按无输入超时退出对话
void lingxin_chat_upload_manager_vad_exit();

#endif // LINGXIN_CHAT_UPLOAD_MANAGER_H
