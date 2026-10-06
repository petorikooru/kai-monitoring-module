idf_component_register(SRCS "eepever.c" "led_indicator.c" "net_eth.c" "sensor_sht45.c" "ui_log.c" "ui.c" "mqtt.c" "wifi.c" "lcd_touch.cpp" "main.cpp" "kai_logo.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lvgl_port lvgl driver led_strip esp_eth esp_netif esp_wifi mbedtls esp_event w5500 esp_driver_uart)