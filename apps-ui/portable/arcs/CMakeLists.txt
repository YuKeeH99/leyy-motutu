target_sources(
    ${LISA_UI_LIB_NAME}
    PRIVATE
    lisa_ui_mem.c
    lisa_ui_invoke.c
    lisa_ui_workq.c
)

if(CONFIG_LISA_DISPLAY_DEVICE)
    target_sources(${LISA_UI_LIB_NAME} PRIVATE lisa_ui_lvgl.c)
endif()

target_include_directories(${LISA_UI_LIB_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(${LISA_UI_LIB_NAME} PRIVATE LISA_UI_PLATFORM_ARCS=1)

target_link_libraries(${LISA_UI_LIB_NAME} PUBLIC listenai_interface)
listenai_link_libraries(${LISA_UI_LIB_NAME})
