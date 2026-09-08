#include "led.h"
#include "driver/gpio.h"

static bool s_led_on = false;

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(STATUS_LED_GPIO, 0);
}

void led_set(bool on)
{
    s_led_on = on;
    gpio_set_level(STATUS_LED_GPIO, on ? 1 : 0);
}

bool led_get(void)
{
    return s_led_on;
}