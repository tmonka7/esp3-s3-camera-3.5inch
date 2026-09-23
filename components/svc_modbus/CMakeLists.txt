idf_component_register(
    SRCS
        "src/svc_modbus.c"
        "src/mb_proto.c"
        "src/mb_rtu.c"
        "src/mb_tcp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bsp app_core driver lwip
)
