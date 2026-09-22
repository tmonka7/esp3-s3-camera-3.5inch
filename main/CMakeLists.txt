idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        bsp app_core ui
        svc_uart svc_modbus svc_power svc_switch svc_temp svc_media svc_detect svc_notify
)
