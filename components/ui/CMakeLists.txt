idf_component_register(
    SRCS
        "src/ui_main.c"
        "src/ui_i18n.c"
        "src/ui_theme.c"
        "src/ui_liveview.c"
        "src/ui_splash.c"
        "src/ui_home.c"
        "src/ui_camera.c"
        "src/ui_uart.c"
        "src/ui_modbus.c"
        "src/ui_power.c"
        "src/ui_switches.c"
        "src/ui_temp.c"
        "src/ui_detect.c"
        "src/ui_storage.c"
        "src/ui_playback.c"
        "src/ui_settings.c"
        "src/ui_datetime.c"
        "src/ui_dialog.c"
        "src/ui_access.c"
        "src/ui_cards.c"
        "src/ui_faces.c"
        "src/ui_webcam.c"
        "src/fonts/ui_font_ja_12.c"
        "src/fonts/ui_font_ja_14.c"
        "src/fonts/ui_font_ja_16.c"
        "src/fonts/ui_font_ja_20.c"
        "src/fonts/ui_font_ja_24.c"
        "src/fonts/ui_font_ja_32.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        bsp app_core
        svc_uart svc_modbus svc_power svc_switch svc_temp svc_media svc_detect svc_notify
        svc_access svc_face svc_webcam
        esp_app_format
)
