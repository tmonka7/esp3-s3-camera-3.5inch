idf_component_register(
    SRCS "src/svc_modbus.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp app_core driver
)
