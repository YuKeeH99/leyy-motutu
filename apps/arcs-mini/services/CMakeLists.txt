listenai_library_named(app-services)
listenai_library_sources(
    service_led.c
    service_volume.c
    service_button.c
    service_alarm.c
    service_image.c
    servo_bus.c
    servo_backend_pwm.c
    servo_backend_bus.c
    car_motion.c
    motutu_motion.c
    service_motutu_web.c
    voice_motutu_control.c
    voice_car_control.c
    voice_lingxin_action.c
)

if(CONFIG_LISA_DISPLAY_DEVICE)
    listenai_library_sources(service_brightness.c)
else()
    listenai_library_sources(service_brightness_headless.c)
endif()

if(CONFIG_LISA_CAMERA_DEVICE)
    listenai_library_sources(service_camera.c)
else()
    listenai_library_sources(service_camera_disabled.c)
endif()

if(CONFIG_BOARD_ARCS_MINI_V2_1)
    listenai_library_sources(
        service_v2_actuators.c
        service_v2_rgb_effect.c
        service_v2_rgb_effect_policy.c
        service_v2_nfc_peer.c
        service_v2_sensors.c
        service_v2_shell.c
        service_v2_gait.c
        service_v2_gait_preflight_policy.c
        service_v2_idle_action.c
        service_v2_touch_action.c
        service_v2_touch_action_policy.c
        service_v2_shake_detector.c
        service_v2_imu_interaction.c
        service_voice_action.c
        service_offline_action.c
        service_offline_action_policy.c
        service_local_version.c
        service_voice_action_policy.c
        service_voice_action_queue.c
        service_voice_query_policy.c
        service_v2_voice_standby.c
        service_v2_voice_standby_policy.c
        service_v2_low_battery.c
        service_v2_low_battery_policy.c
    )
endif()

listenai_library_sources_ifdef(CONFIG_APP_CODEX_STATUS service_codex_status.c)
listenai_library_sources_ifdef(CONFIG_APP_CODEX_STATUS_DISCOVERY service_codex_discovery.c)
listenai_library_sources_ifdef(CONFIG_APP_CODEX_VOICE_INPUT service_codex_voice_input.c)

listenai_library_sources_ifdef(CONFIG_MIDDLEWARE_SD_MUSIC service_sd_music.c)

listenai_library_compile_options(
    PRIVATE
    -Wno-trigraphs
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_TOP_DIR}/src/middleware/sd_music
    ${PROJECT_TOP_DIR}/arcs-sdk/drivers/lisa_sdmmc
)
