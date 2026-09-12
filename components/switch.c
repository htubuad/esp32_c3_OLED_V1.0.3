#include "switch.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "SWITCH";

#define NVS_NAMESPACE       "switch"
#define NVS_KEY_SW0_ON      "sw0_on"
#define NVS_KEY_SW_BIT1_ON  "sw_bit1"
#define NVS_KEY_SW1_ON      "sw1_on"
#define NVS_KEY_POWER_ON    "pwr_on"

static bool s_sw0_on = false;
static bool s_sw_bit1_on = false;
static bool s_sw1_on = false;
static bool s_power_on = false;

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

static esp_err_t nvs_save_sw0(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_SW0_ON, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_load_sw0(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    uint8_t val = 0;
    err = nvs_get_u8(h, NVS_KEY_SW0_ON, &val);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return val != 0;
}

static esp_err_t nvs_save_sw_bit1(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_SW_BIT1_ON, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_load_sw_bit1(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    uint8_t val = 0;
    err = nvs_get_u8(h, NVS_KEY_SW_BIT1_ON, &val);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return val != 0;
}

static esp_err_t nvs_save_power(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_POWER_ON, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_load_power(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    uint8_t val = 0;
    err = nvs_get_u8(h, NVS_KEY_POWER_ON, &val);
    nvs_close(h);
    if (err != ESP_OK) return false;
    return val != 0;
}

void switch_init(void)
{
    gpio_reset_pin(SWITCH0_GPIO);
    gpio_reset_pin(SWITCH2_GPIO);
    gpio_reset_pin(SWITCH1_GPIO);
    gpio_reset_pin(POWER_GPIO);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SWITCH0_GPIO) | (1ULL << SWITCH2_GPIO) | (1ULL << SWITCH1_GPIO) | (1ULL << POWER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool saved_sw0 = nvs_load_sw0();
    s_sw0_on = saved_sw0;
    gpio_set_level(SWITCH0_GPIO, saved_sw0 ? 1 : 0);
    // ESP_LOGI(TAG, "sw0(bit0) init gpio=%d state=%d", SWITCH0_GPIO, saved_sw0);

    bool saved_bit1 = nvs_load_sw_bit1();
    s_sw_bit1_on = saved_bit1;
    gpio_set_level(SWITCH2_GPIO, saved_bit1 ? 1 : 0);
    // ESP_LOGI(TAG, "sw_bit1(bit1) init gpio=%d state=%d", SWITCH2_GPIO, saved_bit1);

    bool saved_sw1 = nvs_load_sw1();
    s_sw1_on = saved_sw1;
    gpio_set_level(SWITCH1_GPIO, saved_sw1 ? 1 : 0);
    // ESP_LOGI(TAG, "switch1(light) init gpio=%d state=%d", SWITCH1_GPIO, saved_sw1);

    bool saved_power = nvs_load_power();
    s_power_on = saved_power;
    gpio_set_level(POWER_GPIO, saved_power ? 1 : 0);
    // ESP_LOGI(TAG, "power init gpio=%d state=%d", POWER_GPIO, saved_power);
}

void switch1_set(bool on)
{
    s_sw1_on = on;
    gpio_set_level(SWITCH1_GPIO, on ? 1 : 0);
    nvs_save_sw1(on);
}

bool switch1_get(void)
{
    return s_sw1_on;
}

void power_set(bool on)
{
    s_power_on = on;
    gpio_set_level(POWER_GPIO, on ? 1 : 0);
    nvs_save_power(on);
}

bool power_get(void)
{
    return s_power_on;
}

void sw0_set(bool on)
{
    s_sw0_on = on;
    gpio_set_level(SWITCH0_GPIO, on ? 1 : 0);
    nvs_save_sw0(on);
}

bool sw0_get(void)
{
    return s_sw0_on;
}

void sw_bit1_set(bool on)
{
    s_sw_bit1_on = on;
    gpio_set_level(SWITCH2_GPIO, on ? 1 : 0);
    nvs_save_sw_bit1(on);
}

bool sw_bit1_get(void)
{
    return s_sw_bit1_on;
}