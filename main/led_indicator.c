#include "led_indicator.h"
#include "config.h"
#include "led_strip.h"
#include "esp_check.h"

static led_strip_handle_t led_strip = NULL;

void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
}

void led_switch(led_state_t state)
{
    switch (state) {
        case LED_ERR:  ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 255, 0, 0)); break;
        case LED_BOOT: ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 0, 0, 255)); break;
        case LED_TX:   ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, 0, 255, 0)); break;
        default:       ESP_ERROR_CHECK(led_strip_clear(led_strip)); break;
    }
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
}