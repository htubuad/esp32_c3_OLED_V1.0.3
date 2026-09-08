#include "sntp_sync.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "SNTP";
static bool s_sntp_initialized = false;

static void sntp_time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synced: %ld", (long)tv->tv_sec);
}

void sntp_init_and_sync(void)
{
    if (s_sntp_initialized) {
        ESP_LOGW(TAG, "SNTP already initialized, skipping");
        return;
    }
    s_sntp_initialized = true;

    ESP_LOGI(TAG, "Syncing time...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(sntp_time_sync_cb);
    esp_sntp_init();

    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now = time(NULL);
    int waited = 0;
    while (now < 1000000000 && waited < 10000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        now = time(NULL);
    }
    ESP_LOGI(TAG, "Time: %s", ctime(&now));
}