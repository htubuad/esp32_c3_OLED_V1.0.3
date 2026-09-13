#ifndef LED_H
#define LED_H

#include <stdbool.h>

#define LED_STATUS_GPIO     13

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK_SLOW,
    LED_MODE_BLINK_FAST,
    LED_MODE_BLINK_IDLE,
    LED_MODE_BLINK_DOUBLE,
    LED_MODE_BLINK_GOOD,
    LED_MODE_BLINK_BAD,
} led_mode_t;

void led_init(void);

void led_status_mode_set(led_mode_t mode);

void led_notify_wifi_ap(bool active);
void led_notify_wifi_sta(bool connected);
void led_notify_wifi_idle(bool energy_save);
void led_notify_mqtt(bool connected);

void led_notify_ota_start(void);
void led_notify_ota_success(void);
void led_notify_ota_fail(void);

void led_status_rx_notify(void);
void led_status_tx_notify(void);

#endif