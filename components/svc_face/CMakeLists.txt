idf_component_register(
    SRCS
        "src/svc_face.c"
        "src/face_lbp.c"
        "src/face_locate.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bsp app_core svc_media svc_access esp32-camera esp_timer
)
