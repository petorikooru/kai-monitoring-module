#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void sensor_sht45_init(void);
esp_err_t sensor_sht45_read(float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif