#pragma once

#include "workbuddy_model.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool demo_mode;
    bool gateway_online;
    bool audio_ready;
    int battery_percent;
    uint32_t now_ms;
    const char *wifi_label;
    const char *notice;
} wb_ui_runtime_t;

/* All functions must be called while holding the BSP LVGL lock. */
bool wb_ui_init(void);
void wb_ui_render(const wb_model_t *model, const wb_ui_runtime_t *runtime);
