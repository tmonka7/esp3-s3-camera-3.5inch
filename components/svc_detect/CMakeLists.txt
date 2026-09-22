idf_component_register(
    SRCS "src/svc_detect.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp app_core svc_media svc_notify
)
