#ifndef NTC_SENSOR_H
#define NTC_SENSOR_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#define NTC1_ADC_GPIO       3
#define NTC2_ADC_GPIO       4

#define NTC_VOLTAGE_MV      3300

#define NTC_FIXED_RES_OHM   47000
#define NTC_NOMINAL_RES_OHM 10700
#define NTC_B_VALUE         3415
#define NTC_NOMINAL_TEMP_K  298.15f

#define NTC_OPEN_CIRCUIT_TEMP   (-40.0f)
#define NTC_SHORT_CIRCUIT_TEMP  (150.0f)
#define NTC_OPEN_RAW_THRESHOLD  (4000)
#define NTC_SHORT_RAW_THRESHOLD (10)

esp_err_t ntc_sensor_init(void);
float ntc_sensor_get1(void);
float ntc_sensor_get2(void);

adc_oneshot_unit_handle_t ntc_sensor_get_adc_unit(void);
adc_cali_handle_t ntc_sensor_get_cali_handle(void);
bool ntc_sensor_is_calibrated(void);

#endif