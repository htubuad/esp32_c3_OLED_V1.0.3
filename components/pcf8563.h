#ifndef PCF8563_H
#define PCF8563_H

#include <stdbool.h>
#include <time.h>

#define PCF8563_I2C_SCL_GPIO   9
#define PCF8563_I2C_SDA_GPIO   8
#define PCF8563_I2C_ADDR       0x51

bool pcf8563_init(void);
bool pcf8563_read_time(struct tm *out);
bool pcf8563_set_time(const struct tm *in);
bool pcf8563_is_running(void);

#endif