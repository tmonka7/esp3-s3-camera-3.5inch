idf_component_register(
    SRCS "src/svc_notify.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp app_core esp_http_client driver
)
