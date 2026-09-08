#ifndef TEMP_SENSOR_H
#define TEMP_SENSOR_H

#include "esp_err.h"

esp_err_t temp_sensor_init(void);
float temp_sensor_get(void);

#endif