
listenai_library_sources(
    powerup.c
    platform.c
    sys_network_manager.c
    sys_network_diag.c
    remote_logger.c
    sys_network_probe.c
    sys_network_health.c
    sys_wifi.c
    sys_wifi_radio_power.c
    sys_wifi_reconnect_policy.c
)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)
