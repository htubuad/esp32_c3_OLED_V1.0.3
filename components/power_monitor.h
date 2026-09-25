#ifndef POWER_MONITOR_H
#define POWER_MONITOR_H

#include "esp_err.h"
#include <stdbool.h>

#define POWER_MONITOR_ADC_GPIO      2
#define POWER_MONITOR_ADC_CHANNEL   ADC_CHANNEL_2

#define POWER_MONITOR_R5_OHM        150000
#define POWER_MONITOR_R6_OHM        22000

#define POWER_MONITOR_CAL_FACTOR    1.0178f

#define POWER_MONITOR_OVERVOLTAGE_MV    14000
#define POWER_MONITOR_UNDERVOLTAGE_MV    9000
#define POWER_MONITOR_RECOVERY_MV       11000

#define POWER_MONITOR_SAMPLE_INTERVAL_MS    200
#define POWER_MONITOR_UNDERVOLTAGE_COUNT     2
#define POWER_MONITOR_RECOVERY_COUNT        5

typedef void (*power_monitor_cb_t)(void);

esp_err_t power_monitor_init(void);

bool power_monitor_is_low(void);

bool power_monitor_in_protection(void);

int power_monitor_get_battery_mv(void);

void power_monitor_set_undervolt_cb(power_monitor_cb_t cb);
void power_monitor_set_recovery_cb(power_monitor_cb_t cb);

void power_monitor_poll(void);

#endif