#include "temp_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"

static const char *TAG = "TEMP";
static temperature_sensor_handle_t s_tsens = NULL;
static float s_temperature = 0.0f;
static bool s_available = false;

esp_err_t temp_sensor_init(void)
{
    temperature_sensor_config_t temp_sensor = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    esp_err_t ret = temperature_sensor_install(&temp_sensor, &s_tsens);
    if (ret != ESP_OK) {
        // ESP_LOGI(TAG, "temperature_sensor_install failed: %s", esp_err_to_name(ret));
        // ESP_LOGI(TAG, "ESP32-C3 no internal temp sensor, using simulated value");
        s_available = false;
        return ESP_OK;
    }
    ret = temperature_sensor_enable(s_tsens);
    if (ret != ESP_OK) {
        // ESP_LOGI(TAG, "temperature_sensor_enable failed: %s", esp_err_to_name(ret));
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
        s_available = false;
        return ESP_OK;
    }
    s_available = true;
    // ESP_LOGI(TAG, "Chip temperature sensor ready");
    return ESP_OK;
}

float temp_sensor_get(void)
{
    if (!s_available || s_tsens == NULL) {
        static float s_sim = 25.0f;
        s_sim += ((int)(esp_timer_get_time() / 1000) % 200 - 100) * 0.02f;
        if (s_sim < -10.0f) s_sim = -10.0f;
        if (s_sim > 80.0f)  s_sim = 80.0f;
        return s_sim;
    }
    temperature_sensor_get_celsius(s_tsens, &s_temperature);
    return s_temperature;
}