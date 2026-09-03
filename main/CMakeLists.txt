idf_component_register(
    SRCS "main.c"
         "workbuddy_app.c"
         "workbuddy_audio.c"
         "workbuddy_model.c"
         "workbuddy_protocol.c"
         "workbuddy_transport.c"
         "workbuddy_ui.c"
         "workbuddy_wifi.c"
         "fonts/lv_font_noto_sans_sc_14.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_hw_support esp_netif esp_timer esp_wifi json mbedtls
)

# ESP32-C3 tasks in this app are intentionally small. Fail the firmware build
# before a large automatic object can silently turn startup into a reset loop.
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=frame-larger-than=3072)
