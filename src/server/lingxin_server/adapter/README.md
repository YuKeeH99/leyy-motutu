# Lingxin Adapter

这里放项目侧适配代码，用来实现 `AI2T_LingXinEngine/inc/system_adapter` 中声明的接口。

后续适配模块建议按职责拆分：

```text
lingxin_http_adapter.c
lingxin_websocket_adapter.c
lingxin_thread_adapter.c
lingxin_sync_adapter.c
lingxin_file_adapter.c
lingxin_audio_adapter.c
lingxin_time_adapter.c
```

不要直接修改 `third_party/AI2T_LingXinEngine` 中的原厂 SDK 源码。

当前状态：

- HTTP、websocket、线程、锁、信号量、文件、时间、定时器、打印、设备信息已接到项目侧基础能力。
- 录音已通过 `audio0` 注册回调接入板子现有采集链路，并将麦克风通道转成 16 kHz/16 bit 单声道 PCM 送入 Lingxin SDK。
- 本地播放接入 `PLAYER_T_TONE`，流式播放接入 `PLAYER_T_CLOUD` 的 MP3 stream 写入路径；仍需要真机确认播放器对 `stream://type=mp3` 的实际兼容性和播放延迟。
- 非 Lingxin 链路选中时，这些适配文件只参与编译，不会主动初始化 SDK、订阅事件、创建线程或占用音频/网络资源。
