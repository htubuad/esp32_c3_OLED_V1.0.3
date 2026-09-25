#ifndef SWITCH_H
#define SWITCH_H

#include <stdbool.h>
#include "esp_err.h"

#define POWER_GPIO          13

#define SWITCH1_GPIO        19
#define SWITCH2_GPIO        1

#define LIGHT_GPIO          18

esp_err_t switch_init(void);

void power_set(bool on);
bool power_get(void);

void switch1_set(bool on);
bool switch1_get(void);

void switch2_set(bool on);
bool switch2_get(void);

void light_set(bool on);
bool light_get(void);

#endif