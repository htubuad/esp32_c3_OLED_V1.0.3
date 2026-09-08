#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include "esp_err.h"

#define STATUS_LED_GPIO     8

void led_init(void);
void led_set(bool on);
bool led_get(void);

#endif