cmake_minimum_required(VERSION 3.13)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 当前产品默认并锁定为 V2.1。只有明确构建其它硬件变体时才允许解锁，
# 避免复用 build 缓存或漏写 -DBOARD 后生成能编译、但音频通道错误的固件。
set(ARCS_MINI_PRODUCT_BOARD "arcs_mini_v2_1" CACHE STRING "Locked product board")
option(ARCS_MINI_ALLOW_BOARD_OVERRIDE "Allow an intentional non-product board build" OFF)

# 设置默认板型 (如果未在命令行指定)
if(NOT DEFINED BOARD)
    set(BOARD "${ARCS_MINI_PRODUCT_BOARD}")
    message(STATUS "Using default BOARD: ${BOARD}")
endif()

if(NOT ARCS_MINI_ALLOW_BOARD_OVERRIDE AND NOT BOARD STREQUAL ARCS_MINI_PRODUCT_BOARD)
    message(FATAL_ERROR
        "Board lock rejected BOARD='${BOARD}'. This product requires "
        "'${ARCS_MINI_PRODUCT_BOARD}'. Remove the old build directory (or use -C) "
        "and configure again. For an intentional hardware variant only, pass "
        "-DARCS_MINI_ALLOW_BOARD_OVERRIDE=ON explicitly.")
endif()

find_package(listenai-cmake REQUIRED HINTS $ENV{ARCS_BASE})

project(arcs-mini)

option(ARCS_TEMP_UPLINK_SAMPLE "One-shot RAM uplink diagnostic (never release)" OFF)
if(ARCS_TEMP_UPLINK_SAMPLE)
    add_definitions(-DARCS_TEMP_UPLINK_SAMPLE)
endif()

if(NOT ARCS_MINI_ALLOW_BOARD_OVERRIDE AND NOT CONFIG_BOARD_ARCS_MINI_V2_1)
    message(FATAL_ERROR
        "Kconfig board does not match arcs_mini_v2_1. The build cache is stale; "
        "run the build with -C and keep the output directory as build.")
endif()

include(version.cmake)

add_definitions(-DLISTEN_CLOUD -DCONFIG_EXT_RAM)

# Add project root directory to include path for app-config-tls-generic.h and memap.h
listenai_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# Get project root directory (two levels up from apps/aopu-yuba)
get_filename_component(PROJECT_TOP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
message (STATUS "PROJECT_TOP_DIR: ${PROJECT_TOP_DIR}")
# Python 3 is discovered by the SDK; reuse the cached Python3_EXECUTABLE.
set(PYTHON3 ${Python3_EXECUTABLE})

add_subdirectory(${PROJECT_TOP_DIR}/modules ${CMAKE_BINARY_DIR}/modules)
add_subdirectory(${PROJECT_TOP_DIR}/src ${CMAKE_BINARY_DIR}/src)

if(CONFIG_APPLICATION_UI)
    add_subdirectory(${PROJECT_TOP_DIR}/apps-ui ${CMAKE_BINARY_DIR}/apps-ui)
endif()

add_subdirectory(tone)
add_subdirectory(services)
add_subdirectory(mcp-tools)
add_subdirectory(voice_intent)

listenai_add_executable(${PROJECT_NAME})
if(ARCS_TEMP_UPLINK_SAMPLE)
    target_sources(${PROJECT_NAME} PRIVATE uplink_sample.c)
endif()

# V2.1 复用默认分区资源，只保留输出包名里的板型后缀。
if(BOARD STREQUAL "arcs_mini_v2_1")
    set(_VARIANT_SUFFIX "_v2_1")
    set(RES_DIR ${PROJECT_TOP_DIR}/res/arcs-mini)
else()
    # res 目录命名约定：<app-name>[_variant]，BOARD 去掉 "arcs_mini" 前缀后即变体后缀
    string(REGEX REPLACE "^arcs_mini" "" _VARIANT_SUFFIX ${BOARD})
    set(RES_DIR ${PROJECT_TOP_DIR}/res/arcs-mini${_VARIANT_SUFFIX})
endif()
set(COMBINED_HEX ${CMAKE_BINARY_DIR}/${PROJECT_NAME}${_VARIANT_SUFFIX}.combined.hex)
set(OTA_TXZ ${CMAKE_BINARY_DIR}/${PROJECT_NAME}${_VARIANT_SUFFIX}-v${PROJECT_VERSION_STR}.ota.txz)

# Keep the original music bytes in CP XIP flash: tone is limited to 1 MiB.
set(TONE_EXTRA_ARGS)
if(CONFIG_BOARD_ARCS_MINI_V2_1)
    enable_language(ASM)
    set(MUSIC_131_FILE ${RES_DIR}/tone/131_music.mp3)
    configure_file(music_131.S.in ${CMAKE_CURRENT_BINARY_DIR}/music_131.S @ONLY)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/music_131.S
        PROPERTIES OBJECT_DEPENDS ${MUSIC_131_FILE})
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/music_131.S)
    list(APPEND TONE_EXTRA_ARGS --exclude 131_music.mp3 --max-bytes 1048576)
endif()

# tone.bin is a generated resource. Rebuild it on every package invocation so
# replacing an MP3 under res/arcs-mini/tone can never leave a stale tone image.
# The helper stages only root-level NNN_name.mp3 files and atomically replaces
# the previous image, so editor/cache directories such as .omc are excluded.
add_custom_target(
    tone_resource ALL
    COMMAND ${PYTHON3} ${PROJECT_TOP_DIR}/tools/repack_tone.py
        --assets ${RES_DIR}/tone
        --output ${RES_DIR}/tone.bin
        ${TONE_EXTRA_ARGS}
    WORKING_DIRECTORY ${PROJECT_TOP_DIR}
    COMMENT "Repacking all tone MP3 files into ${RES_DIR}/tone.bin"
    VERBATIM
)

add_custom_target(
    combined_hex ALL
    COMMAND ${CMAKE_COMMAND} -E echo "-- Generating combined hex: ${COMBINED_HEX}"
    COMMAND ${PYTHON3} ${PROJECT_TOP_DIR}/tools/make_package.py
        --format hex
        --partition-table ${RES_DIR}/partition_table.json
        --var BUILD_DIR=${CMAKE_BINARY_DIR}
        ${COMBINED_HEX}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_dependencies(combined_hex ${PROJECT_NAME} tone_resource)

add_custom_target(
    ota_txz ALL
    COMMAND ${CMAKE_COMMAND} -E echo "-- Generating ota txz: ${OTA_TXZ}"
    COMMAND ${PYTHON3} ${PROJECT_TOP_DIR}/tools/make_package.py
        --format ota
        --partition-table ${RES_DIR}/partition_table.json
        --var BUILD_DIR=${CMAKE_BINARY_DIR}
        --ota-version ${PROJECT_VERSION_STR}
        ${OTA_TXZ}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_dependencies(ota_txz ${PROJECT_NAME} tone_resource)

# add_subdirectory(msgs)
# add_subdirectory(recognize)

target_sources(
    ${PROJECT_NAME} PRIVATE
    main.c
    voice_player.c
)

if(CONFIG_LISA_CAMERA_DEVICE)
    target_sources(${PROJECT_NAME} PRIVATE button_camera_preview.c)
endif()

target_link_options(${PROJECT_NAME} PRIVATE
    -Wl,--wrap,gettimeofday
    -Wl,--wrap,settimeofday
    -Wl,--wrap,time
    -Wl,--wrap,gmtime_r
)
