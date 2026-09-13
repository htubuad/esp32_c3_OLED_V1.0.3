#include "led.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_SPEED_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_RESOLUTION     LEDC_TIMER_10_BIT
#define LEDC_FREQ_HZ        5000

#define BLINK_SLOW_US       300000
#define BLINK_FAST_US       90000
#define BLINK_IDLE_US       2000000
#define BLINK_DOUBLE_SHORT_US  100000
#define BLINK_DOUBLE_GAP_US    600000
#define BLINK_BAD_US            75000
#define BLINK_GOOD_FLASH_US     100000
#define BLINK_GOOD_HOLD_US      2000000
#define LED_ON_DUTY         500

#define NOTIFY_US           80000

static led_mode_t s_status_mode = LED_MODE_OFF;
static led_mode_t s_saved_mode = LED_MODE_OFF;
static bool s_notify_active = false;
static int s_notify_step = 0;

static bool s_ap_active = false;
static bool s_sta_connected = false;
static bool s_mqtt_connected = false;
static bool s_idle_energy_save = false;
static bool s_ota_active = false;

static void resolve_status_mode(void)
{
    if (s_ota_active) return;

    if (s_mqtt_connected) {
        led_status_mode_set(LED_MODE_ON);
    } else if (s_sta_connected) {
        led_status_mode_set(LED_MODE_BLINK_SLOW);
    } else if (s_ap_active) {
        led_status_mode_set(LED_MODE_BLINK_FAST);
    } else if (s_idle_energy_save) {
        led_status_mode_set(LED_MODE_BLINK_IDLE);
    } else {
        led_status_mode_set(LED_MODE_BLINK_SLOW);
    }
}

static esp_timer_handle_t s_status_timer = NULL;
static int s_blink_step = 0;
static int s_good_step = 0;

static void apply_pwm(int duty)
{
    if (duty < 0) duty = 0;
    if (duty > 1023) duty = 1023;
    ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
}

static void apply_gpio(bool on)
{
    apply_pwm(on ? 1023 : 0);
}

static void resume_status_mode(void)
{
    s_status_mode = s_saved_mode;
    s_saved_mode = LED_MODE_OFF;
    s_notify_active = false;
    s_blink_step = 0;
    switch (s_status_mode) {
    case LED_MODE_BLINK_SLOW:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_SLOW_US);
        break;
    case LED_MODE_BLINK_FAST:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_FAST_US);
        break;
    case LED_MODE_BLINK_IDLE:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_IDLE_US);
        break;
    case LED_MODE_ON:
        apply_pwm(LED_ON_DUTY);
        break;
    case LED_MODE_OFF:
    default:
        apply_gpio(false);
        break;
    }
}

static void notify_timer_cb(void)
{
    if (!s_notify_active) return;

    bool even = (s_notify_step % 2 == 0);
    apply_gpio(even);
    s_notify_step++;

    if (s_notify_step >= 2) {
        resume_status_mode();
        return;
    }

    esp_timer_start_once(s_status_timer, NOTIFY_US);
}

static void status_timer_cb(void *arg)
{
    if (s_notify_active) {
        notify_timer_cb();
        return;
    }

    switch (s_status_mode) {
    case LED_MODE_BLINK_SLOW: {
        bool on = (s_blink_step % 2 == 0);
        apply_gpio(on);
        s_blink_step++;
        esp_timer_start_once(s_status_timer, BLINK_SLOW_US);
        break;
    }
    case LED_MODE_BLINK_FAST: {
        bool on = (s_blink_step % 2 == 0);
        apply_gpio(on);
        s_blink_step++;
        esp_timer_start_once(s_status_timer, BLINK_FAST_US);
        break;
    }
    case LED_MODE_BLINK_IDLE: {
        bool on = (s_blink_step % 2 == 0);
        apply_gpio(on);
        s_blink_step++;
        esp_timer_start_once(s_status_timer, BLINK_IDLE_US);
        break;
    }
    case LED_MODE_BLINK_DOUBLE: {
        int phase = s_blink_step % 4;
        if (phase == 0)      apply_gpio(true);
        else if (phase == 1) apply_gpio(false);
        else if (phase == 2) apply_gpio(true);
        else                 apply_gpio(false);
        s_blink_step++;
        uint32_t delay = (phase == 3) ? BLINK_DOUBLE_GAP_US : BLINK_DOUBLE_SHORT_US;
        esp_timer_start_once(s_status_timer, delay);
        break;
    }
    case LED_MODE_BLINK_GOOD: {
        if (s_good_step < 6) {
            bool on = (s_good_step % 2 == 0);
            apply_gpio(on);
            s_good_step++;
            esp_timer_start_once(s_status_timer, BLINK_GOOD_FLASH_US);
        } else {
            apply_pwm(LED_ON_DUTY);
        }
        break;
    }
    case LED_MODE_BLINK_BAD: {
        bool on = (s_blink_step % 2 == 0);
        apply_gpio(on);
        s_blink_step++;
        esp_timer_start_once(s_status_timer, BLINK_BAD_US);
        break;
    }
    default:
        break;
    }
}

void led_init(void)
{
    gpio_reset_pin(LED_STATUS_GPIO);

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
}

static void notify_blink_once(void)
{
    if (s_notify_active) return;

    s_saved_mode = s_status_mode;
    s_notify_active = true;
    s_notify_step = 0;

    if (s_status_timer) {
        esp_timer_stop(s_status_timer);
    }

    apply_gpio(s_saved_mode != LED_MODE_ON);
    s_notify_step = 1;
    esp_timer_start_once(s_status_timer, NOTIFY_US);
}

void led_status_rx_notify(void)
{
    notify_blink_once();
}

void led_status_tx_notify(void)
{
    notify_blink_once();
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
    s_blink_step = 0;
    s_good_step = 0;

    switch (mode) {
    case LED_MODE_OFF:
        apply_gpio(false);
        break;
    case LED_MODE_ON:
        apply_pwm(LED_ON_DUTY);
        break;
    case LED_MODE_BLINK_SLOW:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_SLOW_US);
        break;
    case LED_MODE_BLINK_FAST:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_FAST_US);
        break;
    case LED_MODE_BLINK_IDLE:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_IDLE_US);
        break;
    case LED_MODE_BLINK_DOUBLE:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_DOUBLE_SHORT_US);
        break;
    case LED_MODE_BLINK_GOOD:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_GOOD_FLASH_US);
        break;
    case LED_MODE_BLINK_BAD:
        apply_gpio(true);
        esp_timer_start_once(s_status_timer, BLINK_BAD_US);
        break;
    default:
        apply_gpio(false);
        break;
    }
}

void led_notify_wifi_ap(bool active)
{
    s_ap_active = active;
    resolve_status_mode();
}

void led_notify_wifi_sta(bool connected)
{
    s_sta_connected = connected;
    resolve_status_mode();
}

void led_notify_mqtt(bool connected)
{
    s_mqtt_connected = connected;
    resolve_status_mode();
}

void led_notify_wifi_idle(bool energy_save)
{
    s_idle_energy_save = energy_save;
    resolve_status_mode();
}

void led_notify_ota_start(void)
{
    s_ota_active = true;
    led_status_mode_set(LED_MODE_BLINK_DOUBLE);
}

void led_notify_ota_success(void)
{
    s_ota_active = false;
    led_status_mode_set(LED_MODE_BLINK_GOOD);
}

void led_notify_ota_fail(void)
{
    s_ota_active = false;
    led_status_mode_set(LED_MODE_BLINK_BAD);
}