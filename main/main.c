#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "version.h"
#include "switch.h"
#include "led.h"
#include "temp_sensor.h"
#include "wifi_manager.h"
#include "sntp_sync.h"
#include "mqtt_aliyun.h"
#include "web_server.h"
#include "ota_manager.h"

static const char *TAG = "MAIN";

static int64_t s_start_time_ms = 0;
static bool s_services_started = false;

void app_main(void)
{
    // ESP_LOGI(TAG, "==================================");
    // ESP_LOGI(TAG, " ESP32-C3 Firmware v%s", APP_VERSION);
    // ESP_LOGI(TAG, "==================================");

    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("httpd", ESP_LOG_WARN);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_start_time_ms = esp_timer_get_time() / 1000;

    switch_init();
    led_init();

    ESP_ERROR_CHECK(temp_sensor_init());

    wifi_manager_start();

    if (wifi_is_connected()) {
        sntp_init_and_sync();
        mqtt_init();
        ota_init();
        s_services_started = true;
    }

    start_webserver(s_start_time_ms);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (wifi_is_connected() && !s_services_started) {
            sntp_init_and_sync();
            mqtt_init();
            ota_init();
            s_services_started = true;
        }
    }
}