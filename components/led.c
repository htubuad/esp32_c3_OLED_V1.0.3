#include "led.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NVS_NAMESPACE       "led"
#define NVS_KEY_MQTT_ON     "mqtt_on"

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_SPEED_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_RESOLUTION     LEDC_TIMER_10_BIT
#define LEDC_FREQ_HZ        5000

#define BREATH_PERIOD_US    6000000
#define BREATH_STEP_US      10000
#define BREATH_STEPS        (BREATH_PERIOD_US / BREATH_STEP_US)
#define BREATH_MIN_DUTY     10
#define BREATH_MAX_DUTY     1000
#define BREATH_BASE_DUTY    BREATH_MIN_DUTY

#define DOUBLE_STEP_US      150000
#define DOUBLE_PATTERN_LEN  6

static const int s_double_pattern[DOUBLE_PATTERN_LEN] = {
    1, 1, 0, 1, 0, 2
};

#define NOTIFY_ON_US        80000
#define NOTIFY_OFF_US       80000

static bool s_mqtt_led_on = false;

static led_mode_t s_status_mode = LED_MODE_OFF;
static led_mode_t s_saved_mode = LED_MODE_OFF;
static bool s_notify_active = false;
static int s_notify_step = 0;
static int s_notify_total = 0;

static esp_timer_handle_t s_status_timer = NULL;
static int s_breath_step = 0;
static int s_pattern_idx = 0;

static void apply_status_pwm(int duty)
{
    if (duty < 0) duty = 0;
    if (duty > 1023) duty = 1023;
    ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
}

static void apply_status_gpio(bool on)
{
    apply_status_pwm(on ? 1023 : 0);
}

static void resume_status_mode(void)
{
    s_status_mode = s_saved_mode;
    s_saved_mode = LED_MODE_OFF;
    s_notify_active = false;
    s_breath_step = 0;
    s_pattern_idx = 0;

    switch (s_status_mode) {
    case LED_MODE_BREATH:
        apply_status_pwm(BREATH_BASE_DUTY);
        esp_timer_start_once(s_status_timer, BREATH_STEP_US);
        break;
    case LED_MODE_BLINK_SLOW:
        apply_status_pwm(1023);
        esp_timer_start_once(s_status_timer, 500000);
        break;
    case LED_MODE_PATTERN_DOUBLE:
        apply_status_pwm(1023);
        esp_timer_start_once(s_status_timer, DOUBLE_STEP_US);
        break;
    case LED_MODE_ON:
        apply_status_pwm(1023);
        break;
    case LED_MODE_OFF:
    default:
        apply_status_pwm(0);
        break;
    }
}

static void notify_timer_cb(void)
{
    if (!s_notify_active) return;

    bool even = (s_notify_step % 2 == 0);
    apply_status_gpio(even);

    s_notify_step++;
    if (s_notify_step >= s_notify_total * 2) {
        resume_status_mode();
        return;
    }

    esp_timer_start_once(s_status_timer, even ? NOTIFY_OFF_US : NOTIFY_ON_US);
}

static void status_timer_cb(void *arg)
{
    if (s_notify_active) {
        notify_timer_cb();
        return;
    }

    switch (s_status_mode) {
    case LED_MODE_BREATH: {
        float angle = (float)s_breath_step / BREATH_STEPS * 3.14159265f;
        float t = sinf(angle);
        int duty = (int)(BREATH_MIN_DUTY + (BREATH_MAX_DUTY - BREATH_MIN_DUTY) * t);
        if (duty < 0) duty = 0;
        if (duty > 1023) duty = 1023;
        apply_status_pwm(duty);
        s_breath_step++;
        if (s_breath_step >= BREATH_STEPS) s_breath_step = 0;
        esp_timer_start_once(s_status_timer, BREATH_STEP_US);
        break;
    }
    case LED_MODE_BLINK_SLOW: {
        bool on = (s_breath_step % 2 == 0);
        apply_status_gpio(on);
        s_breath_step++;
        esp_timer_start_once(s_status_timer, 500000);
        break;
    }
    case LED_MODE_PATTERN_DOUBLE: {
        int step = s_double_pattern[s_pattern_idx % DOUBLE_PATTERN_LEN];
        apply_status_gpio(step > 0);
        s_pattern_idx++;
        esp_timer_start_once(s_status_timer, (step > 0 ? 1 : 2) * DOUBLE_STEP_US);
        break;
    }
    default:
        break;
    }
}

static esp_err_t nvs_save_mqtt_on(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_MQTT_ON, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_load_mqtt_on(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    uint8_t val = 0;
    err = nvs_get_u8(h, NVS_KEY_MQTT_ON, &val);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return val != 0;
}

void led_init(void)
{
    gpio_reset_pin(LED_MQTT_GPIO);
    gpio_reset_pin(LED_STATUS_GPIO);

    gpio_config_t mqtt_io = {
        .pin_bit_mask = (1ULL << LED_MQTT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&mqtt_io);
    gpio_set_level(LED_MQTT_GPIO, 0);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_SPEED_MODE,
        .duty_resolution = LEDC_RESOLUTION,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t chan_cfg = {
        .gpio_num = LED_STATUS_GPIO,
        .speed_mode = LEDC_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&chan_cfg);
    ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);

    const esp_timer_create_args_t status_args = {
        .callback = status_timer_cb,
        .name = "led_status",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&status_args, &s_status_timer);

    bool saved_on = nvs_load_mqtt_on();
    s_mqtt_led_on = saved_on;
    gpio_set_level(LED_MQTT_GPIO, saved_on ? 1 : 0);
}

void led_mqtt_set(bool on)
{
    s_mqtt_led_on = on;
    gpio_set_level(LED_MQTT_GPIO, on ? 1 : 0);
    nvs_save_mqtt_on(on);
}

bool led_mqtt_get(void)
{
    return s_mqtt_led_on;
}

static void status_notify_blink(int blinks)
{
    if (s_notify_active) return;
    if (s_status_mode == LED_MODE_OFF || s_status_mode == LED_MODE_ON) return;

    s_saved_mode = s_status_mode;
    s_notify_active = true;
    s_notify_step = 0;
    s_notify_total = blinks;

    if (s_status_timer) {
        esp_timer_stop(s_status_timer);
    }

    apply_status_gpio(true);
    s_notify_step = 1;
    esp_timer_start_once(s_status_timer, NOTIFY_OFF_US);
}

void led_status_rx_notify(void)
{
    status_notify_blink(3);
}

void led_status_tx_notify(void)
{
    status_notify_blink(2);
}

void led_status_set(bool on)
{
    led_status_mode_set(on ? LED_MODE_ON : LED_MODE_OFF);
}

bool led_status_get(void)
{
    return (s_status_mode != LED_MODE_OFF);
}

void led_status_mode_set(led_mode_t mode)
{
    if (s_status_mode == mode && !s_notify_active) return;

    if (s_status_timer) {
        esp_timer_stop(s_status_timer);
    }

    s_notify_active = false;
    s_saved_mode = LED_MODE_OFF;
    s_status_mode = mode;
    s_breath_step = 0;
    s_pattern_idx = 0;

    switch (mode) {
    case LED_MODE_OFF:
        apply_status_pwm(0);
        break;
    case LED_MODE_ON:
        apply_status_pwm(1023);
        break;
    case LED_MODE_BREATH:
        apply_status_pwm(BREATH_BASE_DUTY);
        esp_timer_start_once(s_status_timer, BREATH_STEP_US);
        break;
    case LED_MODE_BLINK_SLOW:
        apply_status_pwm(1023);
        esp_timer_start_once(s_status_timer, 500000);
        break;
    case LED_MODE_PATTERN_DOUBLE:
        apply_status_pwm(1023);
        esp_timer_start_once(s_status_timer, DOUBLE_STEP_US);
        break;
    default:
        apply_status_pwm(0);
        break;
    }

}

led_mode_t led_status_mode_get(void)
{
    return s_status_mode;
}