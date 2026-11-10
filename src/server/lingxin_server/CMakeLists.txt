listenai_include_directories(./)

get_filename_component(PROJECT_TOP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(LINGXIN_ENGINE_DIR ${PROJECT_TOP_DIR}/third_party/AI2T_LingXinEngine)

listenai_include_directories(
    ./adapter
    ${LINGXIN_ENGINE_DIR}/inc/func
    ${LINGXIN_ENGINE_DIR}/inc/system_adapter
    ${LINGXIN_ENGINE_DIR}/src/inc
    ${LINGXIN_ENGINE_DIR}/src/inc/state_module_manager
    ${LINGXIN_ENGINE_DIR}/src/inc/task_state_strategies
    ${LINGXIN_ENGINE_DIR}/tests
)

file(GLOB_RECURSE LINGXIN_ENGINE_SRCS
    ${LINGXIN_ENGINE_DIR}/src/*.c
)

set(LINGXIN_ENGINE_BASIC_SRCS
    ${LINGXIN_ENGINE_DIR}/src/log/lingxin_log.c
    ${LINGXIN_ENGINE_DIR}/src/utils/base64_utils.c
    ${LINGXIN_ENGINE_DIR}/src/utils/cbuffer.c
    ${LINGXIN_ENGINE_DIR}/src/utils/common.c
    ${LINGXIN_ENGINE_DIR}/src/utils/json_util.c
    ${LINGXIN_ENGINE_DIR}/src/utils/lingxin_memory.c
    ${LINGXIN_ENGINE_DIR}/src/utils/tls_utils.c
    ${LINGXIN_ENGINE_DIR}/src/utils/voice_queue.c
    ${LINGXIN_ENGINE_DIR}/src/websocket/lingxin_hook_websocket.c
    adapter/lingxin_user_track_stub.c
)

set(LINGXIN_ENGINE_CHAT_CORE_SRCS
    ${LINGXIN_ENGINE_BASIC_SRCS}
    ${LINGXIN_ENGINE_DIR}/src/schedule/schedule_protocol.c
    ${LINGXIN_ENGINE_DIR}/src/schedule/schedule_timer_manager.c
    ${LINGXIN_ENGINE_DIR}/src/schedule/schedule_ws_manager.c
    ${LINGXIN_ENGINE_DIR}/src/utils/event_queue.c
)
if(CONFIG_LINGXIN_USER_TRACK_STUB)
list(APPEND LINGXIN_ENGINE_CHAT_CORE_SRCS
    adapter/lingxin_user_track_stub.c
)
else()
list(APPEND LINGXIN_ENGINE_CHAT_CORE_SRCS
    ${LINGXIN_ENGINE_DIR}/src/log/lingxin_user_track.c
)
endif()
file(GLOB_RECURSE LINGXIN_ENGINE_VOICE_CHAT_SRCS
    ${LINGXIN_ENGINE_DIR}/src/voice_chat/*.c
)
list(APPEND LINGXIN_ENGINE_CHAT_CORE_SRCS
    ${LINGXIN_ENGINE_VOICE_CHAT_SRCS}
)

listenai_library_named(lingxin_server)

listenai_library_sources(
    lingxin_action_compat.c
    lingxin_identity_priority.c
    lingxin_server.c
    lingxin_server_config.c
)

if(CONFIG_LINGXIN_ENGINE_ENABLE)
if(CONFIG_LINGXIN_CLOUD_VAD_HALF_DUPLEX)
set(LOCAL_VAD_DIR ${PROJECT_TOP_DIR}/third_party/libfvad/src)
listenai_include_directories(${LOCAL_VAD_DIR})
file(GLOB LOCAL_VAD_SRCS ${LOCAL_VAD_DIR}/vad/*.c ${LOCAL_VAD_DIR}/signal_processing/*.c)
listenai_library_sources(adapter/lingxin_speech_limit.c ${LOCAL_VAD_SRCS})
endif()
listenai_library_sources(
    adapter/lingxin_audio_adapter.c
    adapter/lingxin_capture_buffer.c
    adapter/lingxin_clean_recorder.c
    adapter/lingxin_file_adapter.c
    adapter/lingxin_http_adapter.c
    adapter/lingxin_info_adapter.c
    adapter/lingxin_log_adapter.c
    adapter/lingxin_sync_adapter.c
    adapter/lingxin_thread_adapter.c
    adapter/lingxin_trace.c
    adapter/lingxin_time_adapter.c
    adapter/lingxin_timer_adapter.c
    adapter/lingxin_tts_suppress_policy.c
    adapter/lingxin_test_adapter.c
    adapter/lingxin_websocket_adapter.c
    adapter/lingxin_ws_close_guard.c
)

if(CONFIG_LINGXIN_ENGINE_PROFILE_BASIC)
listenai_library_sources(
    ${LINGXIN_ENGINE_BASIC_SRCS}
)
elseif(CONFIG_LINGXIN_ENGINE_PROFILE_CHAT_CORE)
listenai_library_sources(
    ${LINGXIN_ENGINE_CHAT_CORE_SRCS}
)
else()
listenai_library_sources(
    ${LINGXIN_ENGINE_SRCS}
)
endif()
endif()
