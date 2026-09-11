#ifndef SWITCH_H
#define SWITCH_H

#include <stdbool.h>

#define SWITCH1_GPIO        12

void switch_init(void);

void switch1_set(bool on);
bool switch1_get(void);

#endif