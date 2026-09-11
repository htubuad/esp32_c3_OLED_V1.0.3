#include "switch.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "SWITCH";

#define NVS_NAMESPACE       "switch"
#define NVS_KEY_SW1_ON      "sw1_on"

static bool s_sw1_on = false;

static esp_err_t nvs_save_sw1(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_SW1_ON, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_load_sw1(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    uint8_t val = 0;
    err = nvs_get_u8(h, NVS_KEY_SW1_ON, &val);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return val != 0;
}

void switch_init(void)
{
    gpio_reset_pin(SWITCH1_GPIO);
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SWITCH1_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool saved = nvs_load_sw1();
    s_sw1_on = saved;
    gpio_set_level(SWITCH1_GPIO, saved ? 1 : 0);
    ESP_LOGI(TAG, "switch1 init gpio=%d state=%d", SWITCH1_GPIO, saved);
}

void switch1_set(bool on)
{
    s_sw1_on = on;
    gpio_set_level(SWITCH1_GPIO, on ? 1 : 0);
    nvs_save_sw1(on);
    ESP_LOGI(TAG, "switch1(light) -> %d", on);
}

bool switch1_get(void)
{
    return s_sw1_on;
}