idf_component_register(
    SRCS "src/svc_access.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp app_core svc_media svc_notify driver esp_timer nvs_flash
)
