idf_component_register(
    SRCS "src/svc_webcam.c"
    INCLUDE_DIRS "include"
    REQUIRES app_core svc_media esp_http_server esp_timer
)
