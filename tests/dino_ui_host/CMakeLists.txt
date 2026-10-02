cmake_minimum_required(VERSION 3.20)
project(dinobook_ui_host LANGUAGES C)

if(NOT EXISTS "${LVGL_SOURCE}/lvgl.h")
    message(FATAL_ERROR "Pass -DLVGL_SOURCE=<local LVGL 9.5.0 source directory>")
endif()
get_filename_component(DINO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_SOURCE}" "${CMAKE_BINARY_DIR}/lvgl")

add_executable(dino_ui_host
    render.c
    "${DINO_ROOT}/main/dino_ui.c"
    "${DINO_ROOT}/main/dino_model.c"
    "${DINO_ROOT}/main/dino_catalog.c"
    "${DINO_ROOT}/main/dino_animation.c"
    "${DINO_ROOT}/main/dino_animation_assets.c"
    "${DINO_ROOT}/assets/fonts/dino_font_14.c"
    "${DINO_ROOT}/assets/fonts/dino_font_18.c"
    "${DINO_ROOT}/assets/fonts/dino_font_24.c")
target_include_directories(dino_ui_host PRIVATE "${DINO_ROOT}/main")
target_compile_features(dino_ui_host PRIVATE c_std_11)
target_compile_options(dino_ui_host PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dino_ui_host PRIVATE lvgl m)
