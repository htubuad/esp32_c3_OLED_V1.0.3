#ifndef SWITCH_H
#define SWITCH_H

#include <stdbool.h>

#define SWITCH0_GPIO        5
#define SWITCH1_GPIO        12
#define SWITCH2_GPIO        8
#define POWER_GPIO          4

void switch_init(void);

void sw0_set(bool on);
bool sw0_get(void);

void sw_bit1_set(bool on);
bool sw_bit1_get(void);

void switch1_set(bool on);
bool switch1_get(void);

void power_set(bool on);
bool power_get(void);

#endif