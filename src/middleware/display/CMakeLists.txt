listenai_library_named(app_dis)

if(CONFIG_LISA_DISPLAY_DEVICE)
    listenai_library_sources(
        app_display.c
        lv_fs_net.c
        lv_img_net_loader.c
    )
else()
    listenai_library_sources(app_display_headless.c)
endif()

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)
