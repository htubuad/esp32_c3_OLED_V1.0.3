#include "sntp_sync.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "version.h"
#include <time.h>

static const char *TAG = "SNTP";
static bool s_sntp_initialized = false;
static bool s_sntp_synced = false;

static void sntp_time_sync_cb(struct timeval *tv)
{
    s_sntp_synced = true;
}

void sntp_init_and_sync(void)
{
    if (s_sntp_initialized) return;
    s_sntp_initialized = true;

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(sntp_time_sync_cb);
    esp_sntp_init();

    time_t now = time(NULL);
    int waited = 0;
    while (now < TIME_VALID_EPOCH && waited < 10000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        now = time(NULL);
    }
}

bool sntp_is_synced(void)
{
    return s_sntp_synced;
}