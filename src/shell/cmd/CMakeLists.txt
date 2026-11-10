listenai_library_named(shell_cmds)

listenai_library_sources(
    # app.c
    ble.c
    # fs.c
    heap.c
    id_cmd.c
    interact.c
    kv.c
    lingxin.c
    # listen_cmd.c
    # log.c
    mic_bias.c
    # player.c
    sys.c
    version.c
    wifi.c
)

listenai_library_sources_ifdef(CONFIG_LISA_SDMMC_DEVICE sd.c)
listenai_library_sources_ifdef(CONFIG_OTA ota.c)
listenai_library_sources_ifdef(CONFIG_LOG_UPLOAD logbuf.c)
