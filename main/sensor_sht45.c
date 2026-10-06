#include "sensor_sht45.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t sht45_dev = NULL;

void sensor_sht45_init(void)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = (gpio_num_t)I2C_SDA;
    bus_config.scl_io_num = (gpio_num_t)I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = SHT45_ADDR;
    dev_config.scl_speed_hz = 100000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_config, &sht45_dev));
}

esp_err_t sensor_sht45_read(float *temperature, float *humidity)
{
    uint8_t cmd = 0xFD; // high precision measure
    uint8_t data[6];

    esp_err_t err = i2c_master_transmit(sht45_dev, &cmd, 1, 100);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(10));

    err = i2c_master_receive(sht45_dev, data, 6, 100);
    if (err != ESP_OK) return err;

    uint16_t t_raw = (data[0] << 8) | data[1];
    uint16_t rh_raw = (data[3] << 8) | data[4];

    *temperature = -45.0f + 175.0f * ((float)t_raw / 65535.0f);
    *humidity = -6.0f + 125.0f * ((float)rh_raw / 65535.0f);

    return ESP_OK;
}