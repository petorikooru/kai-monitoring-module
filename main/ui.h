#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);
void ui_show_splash(void);

// called from sensor task
void ui_set_temp(const char *text);
void ui_set_hum(const char *text);

void ui_push_trend(int temp_c, int hum_pct);

// called from eth event handlers
void ui_set_eth_status(const char *text);
void ui_set_eth_ip(const char *text);

// wire this up to ur "Retry" button's real action (called from ui.cpp on click)
typedef void (*eth_retry_cb_t)(void);
void ui_set_eth_retry_cb(eth_retry_cb_t cb);

#ifdef __cplusplus
}
#endif