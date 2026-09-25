#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "version.h"
#include "switch.h"
#include "led.h"
#include "temp_sensor.h"
#include "ntc_sensor.h"
#include "wifi_manager.h"
#include "sntp_sync.h"
#include "mqtt_aliyun.h"
#include "web_server.h"
#include "ota_manager.h"
#include "tf_card.h"
#include "power_monitor.h"
#include "pcf8563.h"

static const char *TAG = "MAIN";

#define MAIN_LOOP_PERIOD_MS   5000

static int64_t s_start_time_ms = 0;
static bool s_services_started = false;
static bool s_sntp_updated_rtc = false;

static void on_power_undervolt(void)
{
    ESP_LOGE(TAG, "=== UNDERVOLT PROTECTION: shutdown all peripherals ===");

    led_status_mode_set(LED_MODE_BLINK_UNDERVOLT);

    ESP_LOGW(TAG, "[UV] phase1: emergency flush TF cache");
    tf_card_emergency_flush();

    ESP_LOGW(TAG, "[UV] phase2: stop communication");
    mqtt_force_stop();
    stop_webserver();
    wifi_manager_emergency_stop();

    ESP_LOGW(TAG, "[UV] phase3: power down peripherals");
    tf_card_deinit();
    switch1_set(false);
    switch2_set(false);

    ESP_LOGI(TAG, "All output switches OFF");

    s_services_started = false;

    ESP_LOGW(TAG, "[UV] protection active, battery=%d mV",
             power_monitor_get_battery_mv());
}

static void on_power_recovery(void)
{
    ESP_LOGI(TAG, "=== Power recovered, restarting services ===");

    ESP_LOGI(TAG, "[REC] phase1: restore TF card");
    tf_card_init();

    ESP_LOGI(TAG, "[REC] phase2: restore outputs");
    switch1_set(true);
    switch2_set(true);

    ESP_LOGI(TAG, "[REC] phase3: start communication");
    wifi_manager_start();
    start_webserver(s_start_time_ms);
    led_status_mode_set(LED_MODE_BLINK_SLOW);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== app_main START ===");

    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("httpd", ESP_LOG_INFO);
    esp_log_level_set("HTTP", ESP_LOG_INFO);
    esp_log_level_set("gpio", ESP_LOG_ERROR);
    esp_log_level_set("sdspi_transaction", ESP_LOG_ERROR);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_ERROR);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS OK");

    s_start_time_ms = esp_timer_get_time() / 1000;

    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to CST-8 (UTC+8)");

    ESP_ERROR_CHECK(switch_init());
    ESP_LOGI(TAG, "switch_init OK");

    ESP_ERROR_CHECK(led_init());
    ESP_LOGI(TAG, "led_init OK");

    ESP_ERROR_CHECK(temp_sensor_init());
    ESP_LOGI(TAG, "temp_sensor OK");

    ESP_ERROR_CHECK(ntc_sensor_init());
    ESP_LOGI(TAG, "ntc_sensor OK");

    ESP_ERROR_CHECK(power_monitor_init());
    power_monitor_set_undervolt_cb(on_power_undervolt);
    power_monitor_set_recovery_cb(on_power_recovery);
    ESP_LOGI(TAG, "power_monitor OK (GPIO%d ADC)", POWER_MONITOR_ADC_GPIO);

    if (pcf8563_init()) {
        struct tm rtc_tm;
        if (pcf8563_read_time(&rtc_tm)) {
            time_t rtc_epoch = mktime(&rtc_tm);
            if (rtc_epoch >= TIME_VALID_EPOCH) {
                struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "PCF8563 time loaded: %04d-%02d-%02d %02d:%02d:%02d",
                         rtc_tm.tm_year + 1900, rtc_tm.tm_mon + 1, rtc_tm.tm_mday,
                         rtc_tm.tm_hour, rtc_tm.tm_min, rtc_tm.tm_sec);
            } else {
                ESP_LOGW(TAG, "PCF8563 time invalid (%ld), waiting for SNTP", (long)rtc_epoch);
            }
        }
    } else {
        ESP_LOGW(TAG, "PCF8563 init failed, time relies on SNTP only");
    }

    ESP_LOGI(TAG, "TF card: %s", tf_card_init() ? "OK" : "failed");

    ESP_LOGI(TAG, "heap before wifi: free=%lu internal=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    wifi_manager_start();

    ESP_LOGI(TAG, "heap after wifi: free=%lu internal=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    start_webserver(s_start_time_ms);

    if (wifi_is_connected()) {
        sntp_init_and_sync();
        mqtt_init();
        ota_init();
        s_services_started = true;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS));

        if (power_monitor_in_protection()) {
            if (s_services_started) {
                s_services_started = false;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (wifi_is_connected() && !s_services_started) {
            sntp_init_and_sync();
            mqtt_init();
            ota_init();
            s_services_started = true;
        }

        ota_pending_verify_loop_check();

        if (!s_sntp_updated_rtc && sntp_is_synced()) {
            struct tm sys_tm;
            time_t now = time(NULL);
            localtime_r(&now, &sys_tm);
            if (pcf8563_set_time(&sys_tm)) {
                struct tm verify_tm;
                if (pcf8563_read_time(&verify_tm)) {
                    double diff = difftime(mktime(&verify_tm), mktime(&sys_tm));
                    if (diff < 0) diff = -diff;
                    if (diff <= 2) {
                        s_sntp_updated_rtc = true;
                        ESP_LOGI(TAG, "SNTP time written to PCF8563 (verified, diff=%.0fs)", diff);
                    } else {
                        ESP_LOGW(TAG, "PCF8563 verify: diff=%.0fs > 2s, treated as OK anyway", diff);
                        s_sntp_updated_rtc = true;
                    }
                } else {
                    ESP_LOGW(TAG, "PCF8563 readback failed, assuming OK");
                    s_sntp_updated_rtc = true;
                }
            }
        }
    }
}