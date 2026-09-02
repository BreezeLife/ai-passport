#pragma once

#include "bsp_button.h"
#include "esp_err.h"

#include <stdbool.h>

#ifdef CONFIG_WB_DEMO_MODE
#define WB_APP_DEMO_MODE true
#else
#define WB_APP_DEMO_MODE false
#endif

/* Display and LVGL must already be initialized. */
esp_err_t wb_app_start(bool battery_available);

/* Safe to call from the BSP button task; it only enqueues a bounded event. */
void wb_app_button_callback(bsp_btn_t button, bsp_btn_ev_t event, void *context);
