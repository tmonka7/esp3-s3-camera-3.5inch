idf_component_register(
    SRCS "src/svc_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp app_core driver
)
