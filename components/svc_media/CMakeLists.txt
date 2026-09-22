idf_component_register(
    SRCS
        "src/svc_media.c"
        "src/media_avi.c"
        "src/media_files.c"
        "src/media_player.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp app_core esp_timer
)
