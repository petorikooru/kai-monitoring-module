#include "net_eth.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include <cstdio>
#include "esp_event.h"
#include "esp_netif.h"

// Local Files //////////////////////////
#include "config.h"
#include "led_indicator.h"
#include "lcd_touch.h"
#include "sensor_sht45.h"
#include "eepever.h"
#include "ui.h"
#include "wifi.h"
#include "mqtt.h"
#include "ui_log.h"


static const char *TAG_START = "Startup";

static void sensor_task(void *arg)
{
    (void)arg;

    while (1) {
        float temp = 0;
        float hum = 0;
        esp_err_t err = sensor_sht45_read(&temp, &hum);

        if (err == ESP_OK) {
            esp_err_t mqtt_err = mqtt_set_readings(temp, hum);
            if (mqtt_err != ESP_OK) {
                ESP_LOGW(TAG_START, "MQTT sample: %s",
                         esp_err_to_name(mqtt_err));
            }

            if (ui_lock(0)) {
                char buf[32];

                snprintf(buf, sizeof(buf), "%.1f C", temp);
                ui_set_temp(buf);

                snprintf(buf, sizeof(buf), "%.1f %%", hum);
                ui_set_hum(buf);

                ui_push_trend((int)temp, (int)hum);
                ui_unlock();
            }

            led_switch(LED_TX);
        } else {
            if (ui_lock(0)) {
                ui_set_temp("err");
                ui_set_hum("err");
                ui_unlock();
            }

            led_switch(LED_ERR);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void init_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG_START, "Booting...");
    led_init();
    led_switch(LED_BOOT);

    lcd_touch_init();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(eth_init());
    ESP_ERROR_CHECK(wifi_manager_init(eth_get_handle(), eth_get_netif()));
    ESP_ERROR_CHECK(mqtt_publisher_init(NULL));

    esp_err_t ep_err = epever_init();
    if (ep_err != ESP_OK) {
        ESP_LOGE(TAG_START, "EPever init: %s", esp_err_to_name(ep_err));
    }

    sensor_sht45_init();

    if (!ui_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG_START, "UI lock failed");
        abort();
    }

    ui_init();
    ui_set_eth_retry_cb(eth_start_or_retry);
    ui_unlock();

    if (xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG_START, "Sensor task creation failed");
        abort();
    }

    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    xTaskCreate(init_task, "init_task", 8192, NULL, 5, NULL);
}