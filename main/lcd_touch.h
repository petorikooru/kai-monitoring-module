#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// call once at boot, sets up LGFX (DMA) + LVGL glue
void lcd_touch_init(void);
void display_set_timeout(uint32_t seconds);
uint32_t display_get_timeout(void);

// wrap every lv_* call between these two (replaces esp_lvgl_port's lock/unlock)
bool ui_lock(uint32_t timeout_ms);
void ui_unlock(void);

#ifdef __cplusplus
}
#endif