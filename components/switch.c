#include "switch.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "SWITCH";

#define NVS_NAMESPACE       "switch"

static bool s_switch1_on = false;
static bool s_switch2_on = false;
static bool s_power_on = false;
static bool s_light_on = false;

static esp_err_t nvs_save(const char *key, bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_load(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    uint8_t val = 0;
    err = nvs_get_u8(h, key, &val);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return val != 0;
}

esp_err_t switch_init(void)
{
    gpio_reset_pin(SWITCH1_GPIO);
    gpio_reset_pin(SWITCH2_GPIO);
    gpio_reset_pin(POWER_GPIO);
    gpio_reset_pin(LIGHT_GPIO);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SWITCH1_GPIO) | (1ULL << SWITCH2_GPIO) | (1ULL << POWER_GPIO) | (1ULL << LIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) return ret;

    s_switch1_on = nvs_load("sw1_on");
    gpio_set_level(SWITCH1_GPIO, s_switch1_on ? 1 : 0);
    ESP_LOGI(TAG, "SW1 GPIO%d = %d", SWITCH1_GPIO, s_switch1_on);

    s_switch2_on = nvs_load("sw2_on");
    gpio_set_level(SWITCH2_GPIO, s_switch2_on ? 1 : 0);
    ESP_LOGI(TAG, "SW2 GPIO%d = %d", SWITCH2_GPIO, s_switch2_on);

    s_power_on = nvs_load("pwr_on");
    gpio_set_level(POWER_GPIO, s_power_on ? 1 : 0);
    ESP_LOGI(TAG, "POWER GPIO%d = %d", POWER_GPIO, s_power_on);

    s_light_on = nvs_load("light_on");
    gpio_set_level(LIGHT_GPIO, s_light_on ? 1 : 0);
    ESP_LOGI(TAG, "LIGHT GPIO%d = %d", LIGHT_GPIO, s_light_on);

    return ESP_OK;
}

void power_set(bool on)
{
    s_power_on = on;
    gpio_set_level(POWER_GPIO, on ? 1 : 0);
    nvs_save("pwr_on", on);
}

bool power_get(void)
{
    return s_power_on;
}

void switch1_set(bool on)
{
    s_switch1_on = on;
    gpio_set_level(SWITCH1_GPIO, on ? 1 : 0);
    nvs_save("sw1_on", on);
}

bool switch1_get(void)
{
    return s_switch1_on;
}

void switch2_set(bool on)
{
    s_switch2_on = on;
    gpio_set_level(SWITCH2_GPIO, on ? 1 : 0);
    nvs_save("sw2_on", on);
}

bool switch2_get(void)
{
    return s_switch2_on;
}

void light_set(bool on)
{
    s_light_on = on;
    gpio_set_level(LIGHT_GPIO, on ? 1 : 0);
    nvs_save("light_on", on);
}

bool light_get(void)
{
    return s_light_on;
}