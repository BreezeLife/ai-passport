idf_component_register(
    SRCS "main.c" "dino_model.c" "dino_catalog.c" "dino_ui.c"
         "dino_animation.c" "dino_animation_assets.c" "dino_adpcm.c" "dino_audio_assets.c"
         "../assets/fonts/dino_font_14.c"
         "../assets/fonts/dino_font_18.c"
         "../assets/fonts/dino_font_24.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash esp_partition
)
