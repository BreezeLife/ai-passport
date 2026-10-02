#pragma once
#include "dino_model.h"
/* Called under the BSP LVGL lock; the screen owns every widget. */
void dino_ui_init(void);
void dino_ui_render(const dino_model_t *model, int battery, int audio, int storage);
