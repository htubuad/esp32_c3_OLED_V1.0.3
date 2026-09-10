#ifndef LED_H
#define LED_H

#include <stdbool.h>
#include "esp_err.h"

#define LED_MQTT_GPIO       12
#define LED_STATUS_GPIO     13

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK_SLOW,
    LED_MODE_BREATH,
    LED_MODE_PATTERN_DOUBLE,
} led_mode_t;

void led_init(void);

void led_mqtt_set(bool on);
bool led_mqtt_get(void);

void led_status_set(bool on);
bool led_status_get(void);
void led_status_mode_set(led_mode_t mode);
led_mode_t led_status_mode_get(void);
void led_status_rx_notify(void);
void led_status_tx_notify(void);

#endif