listenai_library_named(app-netcfg-portal)

listenai_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
)

listenai_library_sources(
    app_netcfg_portal.c
    netcfg_portal_http.c
    netcfg_portal_page.c
)
