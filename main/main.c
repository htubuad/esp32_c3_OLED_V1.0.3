#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "version.h"
#include "led.h"
#include "temp_sensor.h"
#include "wifi_manager.h"
#include "sntp_sync.h"
#include "mqtt_aliyun.h"
#include "web_server.h"
#include "oled.h"
#include "ui_dashboard.h"

static const char *TAG = "MAIN";

static int64_t s_start_time_ms = 0;
static bool s_services_started = false;

void app_main(void)
{
    ESP_LOGI(TAG, "==================================");
    ESP_LOGI(TAG, " ESP32-C3 Firmware v%s", APP_VERSION);
    ESP_LOGI(TAG, "==================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_start_time_ms = esp_timer_get_time() / 1000;

    led_init();
    OLED_Init();
    OLED_Clear();
    OLED_ShowStringSize(0, 0, APP_NAME, OLED_SIZE_16, false);
    OLED_ShowStringSize(0, 16, APP_VERSION, OLED_SIZE_16, false);
    OLED_ShowStringSize(0, 32, "Booting...", OLED_SIZE_16, false);
    OLED_Update();

    ESP_ERROR_CHECK(temp_sensor_init());

    wifi_init_sta();

    if (wifi_is_connected()) {
        sntp_init_and_sync();
        mqtt_init(s_start_time_ms);
        s_services_started = true;
    } else if (wifi_is_ap_active()) {
        ESP_LOGI(TAG, "WiFi not connected - AP provisioning mode active");
        OLED_Clear();
        OLED_ShowStringSize(0, 0, "AP Mode", OLED_SIZE_16, false);
        OLED_ShowStringSize(0, 16, "SSID:Setup", OLED_SIZE_16, false);
        OLED_ShowStringSize(0, 32, "No Password", OLED_SIZE_16, false);
        OLED_ShowStringSize(0, 48, wifi_get_ip(), OLED_SIZE_16, false);
        OLED_Update();
    }

    start_webserver(s_start_time_ms);
    ui_dashboard_start();

    ESP_LOGI(TAG, "=== HEAP STATUS ===");
    ESP_LOGI(TAG, "Free heap: %d bytes", (int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Internal free: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Largest free block: %d bytes", (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "==================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (wifi_is_connected() && !s_services_started) {
            ESP_LOGI(TAG, "WiFi connected, starting services...");
            sntp_init_and_sync();
            mqtt_init(s_start_time_ms);
            s_services_started = true;

            OLED_Clear();
            OLED_ShowStringSize(0, 0, "WiFi OK!", OLED_SIZE_16, false);
            OLED_ShowStringSize(0, 16, wifi_get_ip(), OLED_SIZE_16, false);
            OLED_Update();
        }
    }
}