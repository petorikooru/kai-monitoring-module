#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_ERR = 0,
    LED_BOOT,
    LED_TX,
    LED_OFF,
} led_state_t;

void led_init(void);
void led_switch(led_state_t state);

#ifdef __cplusplus
}
#endif