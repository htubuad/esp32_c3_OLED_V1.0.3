#include "ui_dashboard.h"
#include "oled.h"
#include "wifi_manager.h"
#include "mqtt_aliyun.h"
#include "temp_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "UI";

static int64_t s_start_time_ms = 0;
static TaskHandle_t s_ui_task_handle = NULL;

#define FONT_H    16

#define DASH_Y_WIFI   0
#define DASH_Y_INFO   16
#define DASH_Y_MQTT   32

static char s_last_wifi[24] = {0};
static char s_last_info[24] = {0};
static char s_last_mqtt[48] = {0};

static void format_time(char *buf, size_t size)
{
    time_t now = time(NULL);
    if (now < 1000000000) {
        snprintf(buf, size, "--:--:--");
        return;
    }
    struct tm tm_buf;
    struct tm *tm_info = localtime_r(&now, &tm_buf);
    if (!tm_info) {
        snprintf(buf, size, "--:--:--");
        return;
    }
    snprintf(buf, size, "%02d:%02d:%02d",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

static bool render_wifi_line(void)
{
    char buf[24];

    if (wifi_is_connected()) {
        snprintf(buf, sizeof(buf), "%s", wifi_get_ip());
    } else {
        snprintf(buf, sizeof(buf), "WiFi Fail");
    }

    if (strcmp(buf, s_last_wifi) == 0) return false;
    strncpy(s_last_wifi, buf, sizeof(s_last_wifi) - 1);
    s_last_wifi[sizeof(s_last_wifi) - 1] = '\0';

    OLED_ClearArea(0, DASH_Y_WIFI, OLED_W, FONT_H);
    OLED_ShowStringSize(0, DASH_Y_WIFI, buf, OLED_SIZE_16, false);
    return true;
}

static bool render_info_line(void)
{
    char buf[24];
    wifi_update_rssi();

    float temp = temp_sensor_get();
    char temp_sign = (temp != temp || temp < -50.0f) ? '?' : ' ';
    int temp_int = (int)temp;
    int temp_dec = (int)((temp - temp_int) * 10);
    if (temp_dec < 0) temp_dec = -temp_dec;

    int rssi = wifi_get_rssi();

    snprintf(buf, sizeof(buf), "T:%c%d.%d R:%d", temp_sign, temp_int, temp_dec, rssi);

    if (strcmp(buf, s_last_info) == 0) return false;
    strncpy(s_last_info, buf, sizeof(s_last_info) - 1);
    s_last_info[sizeof(s_last_info) - 1] = '\0';

    OLED_ClearArea(0, DASH_Y_INFO, OLED_W, FONT_H);
    OLED_ShowStringSize(0, DASH_Y_INFO, buf, OLED_SIZE_16, false);
    return true;
}

static bool render_mqtt_line(void)
{
    char buf[48];

    char time_str[10];
    format_time(time_str, sizeof(time_str));

    snprintf(buf, sizeof(buf), "%s %s", mqtt_is_connected() ? "MQTT:OK" : "MQTT:--", time_str);

    if (strcmp(buf, s_last_mqtt) == 0) return false;
    strncpy(s_last_mqtt, buf, sizeof(s_last_mqtt) - 1);
    s_last_mqtt[sizeof(s_last_mqtt) - 1] = '\0';

    OLED_ClearArea(0, DASH_Y_MQTT, OLED_W, FONT_H);
    OLED_ShowStringSize(0, DASH_Y_MQTT, buf, OLED_SIZE_16, false);
    return true;
}

#define MSG_Y       48
#define MSG_H       16
#define MSG_BUF_LEN 256
#define MSG_VISIBLE 16

static char s_msg_buf[MSG_BUF_LEN] = {0};
static int  s_msg_scroll = 0;
static bool s_msg_has_content = false;

static void render_msg_line(void)
{
    const char *data = mqtt_get_last_rx_data();

    if (wifi_is_connected()) {
        if (data && data[0] != '\0') {
            if (strcmp(data, s_msg_buf) == 0) {
                int total = (int)strlen(s_msg_buf);
                if (total > MSG_VISIBLE) {
                    s_msg_scroll++;
                    if (s_msg_scroll > total - MSG_VISIBLE) s_msg_scroll = 0;
                    OLED_ClearArea(0, MSG_Y, OLED_W, MSG_H);
                    OLED_ShowStringSize(0, MSG_Y, s_msg_buf + s_msg_scroll, OLED_SIZE_16, false);
                }
                return;
            }
            strncpy(s_msg_buf, data, MSG_BUF_LEN - 1);
            s_msg_buf[MSG_BUF_LEN - 1] = '\0';
            s_msg_scroll = 0;
            s_msg_has_content = true;
            OLED_ClearArea(0, MSG_Y, OLED_W, MSG_H);
            OLED_ShowStringSize(0, MSG_Y, s_msg_buf, OLED_SIZE_16, false);
        } else if (s_msg_has_content) {
            OLED_ClearArea(0, MSG_Y, OLED_W, MSG_H);
            s_msg_buf[0] = '\0';
            s_msg_scroll = 0;
            s_msg_has_content = false;
        }
        return;
    }

    if (data && data[0] != '\0') {
        if (strcmp(data, s_msg_buf) == 0) {
            int total = (int)strlen(s_msg_buf);
            if (total > MSG_VISIBLE) {
                s_msg_scroll++;
                if (s_msg_scroll > total - MSG_VISIBLE) s_msg_scroll = 0;
                OLED_ClearArea(0, MSG_Y, OLED_W, MSG_H);
                OLED_ShowStringSize(0, MSG_Y, s_msg_buf + s_msg_scroll, OLED_SIZE_16, false);
            }
            return;
        }
        strncpy(s_msg_buf, data, MSG_BUF_LEN - 1);
        s_msg_buf[MSG_BUF_LEN - 1] = '\0';
        s_msg_scroll = 0;
        s_msg_has_content = true;
        OLED_ClearArea(0, MSG_Y, OLED_W, MSG_H);
        OLED_ShowStringSize(0, MSG_Y, s_msg_buf, OLED_SIZE_16, false);
        return;
    }

    if (wifi_is_ap_active()) {
        char ap_buf[24];
        snprintf(ap_buf, sizeof(ap_buf), "AP %s", wifi_get_ip());
        if (!s_msg_has_content || strcmp(s_msg_buf, ap_buf) != 0) {
            strncpy(s_msg_buf, ap_buf, MSG_BUF_LEN - 1);
            s_msg_buf[MSG_BUF_LEN - 1] = '\0';
            s_msg_scroll = 0;
            s_msg_has_content = true;
            OLED_ClearArea(0, MSG_Y, OLED_W, MSG_H);
            OLED_ShowStringSize(0, MSG_Y, s_msg_buf, OLED_SIZE_16, false);
        }
        return;
    }

    if (s_msg_has_content) {
        OLED_ClearArea(0, MSG_Y, OLED_W, MSG_H);
        s_msg_buf[0] = '\0';
        s_msg_scroll = 0;
        s_msg_has_content = false;
    }
}

static void ui_task(void *arg)
{
    s_ui_task_handle = xTaskGetCurrentTaskHandle();

    OLED_Clear();

    render_wifi_line();
    render_info_line();
    render_mqtt_line();

    render_msg_line();
    OLED_Update();

    ESP_LOGI(TAG, "UI started (3 lines + scroll, 1608 font)");

    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300));

        render_wifi_line();
        render_info_line();
        render_mqtt_line();

        render_msg_line();
        OLED_Update();
    }
}

void ui_dashboard_start(void)
{
    s_start_time_ms = esp_timer_get_time() / 1000;
    xTaskCreate(ui_task, "ui_dash", 2048, NULL, 4, NULL);
}

void ui_notify_new_msg(void)
{
    if (s_ui_task_handle) {
        BaseType_t higher = pdFALSE;
        if (xPortInIsrContext()) {
            higher = xTaskNotifyFromISR(s_ui_task_handle, 0, eNoAction, NULL);
        } else {
            xTaskNotifyGive(s_ui_task_handle);
        }
        if (higher == pdTRUE) portYIELD_FROM_ISR();
    }
}