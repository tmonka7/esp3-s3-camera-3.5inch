idf_component_register(
    SRCS "src/svc_temp.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp app_core svc_modbus
)
