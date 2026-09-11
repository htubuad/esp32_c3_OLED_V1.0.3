#include "mqtt_aliyun.h"
#include "wifi_manager.h"
#include "switch.h"
#include "led.h"
#include "temp_sensor.h"
#include "version.h"
#include "oled.h"
#include "ui_dashboard.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "mbedtls/md.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *TAG = "ALIYUN";

#define ALIYUN_PRODUCT_KEY   "k1jrhJxxEiu"
#define ALIYUN_DEVICE_NAME   "esp32_s3_new"
#define ALIYUN_CLIENT_ID     ALIYUN_PRODUCT_KEY "." ALIYUN_DEVICE_NAME
#define ALIYUN_DEVICE_SECRET "f6de83ccbbc1648cd1deaaf4b714af87"

#define ALIYUN_BROKER_HOST   "iot-06z00im62xzpqxb.mqtt.iothub.aliyuncs.com"
#define ALIYUN_BROKER_URI    "mqtt://" ALIYUN_BROKER_HOST ":1883"
#define ALIYUN_USERNAME      ALIYUN_DEVICE_NAME "&" ALIYUN_PRODUCT_KEY

#define ALIYUN_TOPIC_USER        "/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/user/get"
#define ALIYUN_TOPIC_USER_UPDATE "/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/user/update"

#define MQTT_CONNECT_TIMEOUT_MS  15000
#define MQTT_RETRY_DELAY_MS      5000

enum {
    MQTT_STATE_IDLE = 0,
    MQTT_STATE_WAIT_WIFI,
    MQTT_STATE_WAIT_TIME,
    MQTT_STATE_INIT,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR,
};

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static int s_mqtt_state = MQTT_STATE_IDLE;
static int64_t s_start_time_ms = 0;
static SemaphoreHandle_t s_connect_sem = NULL;

#define MQTT_PERIODIC_TX_ENABLED  0

#define MQTT_MSG_Y_START  48
#define MQTT_MSG_FONT     OLED_SIZE_16
#define MQTT_MSG_LINE_H   16
#define MQTT_MSG_MAX_LINES 5

static char s_client_id[128];
static char s_password[72];

static char s_wd_a[128] = {0};
static char s_wd_b[128] = {0};

static int  s_num_val = 0;
static char s_from_src[64] = {0};

static char s_last_payload[512] = {0};

static char s_msg_lines[MQTT_MSG_MAX_LINES][128] = {0};
static int  s_msg_count = 0;
static int  s_msg_idx = 0;

static mqtt_rx_entry_t s_rx_history[MQTT_RX_MAX_ENTRIES];
static int s_rx_head = 0;
static int s_rx_total = 0;

static mqtt_tx_entry_t s_tx_history[MQTT_TX_MAX_ENTRIES];
static int s_tx_head = 0;
static int s_tx_total = 0;
static int s_tx_msg_id = 0;

static float  s_field_a = 0.0f;
static float  s_field_b = 0.0f;
static float  s_set_a   = 0.0f;
static float  s_set_b   = 0.0f;

static char *mqtt_build_tx_frame(void)
{
    float temp = roundf(temp_sensor_get() * 10.0f) / 10.0f;
    int   rssi = wifi_is_connected() ? wifi_get_rssi() : -127;

    uint8_t switches = 0;
    if (sw0_get())       switches |= 1;
    if (sw_bit1_get())   switches |= 2;

    char *out = (char *)malloc(320);
    if (!out) return NULL;

    int n = snprintf(out, 320,
        "{\"DeviceID\":\"%s\",\"Dir\":\"D>C\",\"Temp\":%.1f,\"RSSI\":%d,"
        "\"Switches\":%d,\"light\":%d,\"power\":%d,"
        "\"Field1\":%.2f,\"Field2\":%.2f}",
        DEVICE_ID, temp, rssi, switches,
        switch1_get() ? 1 : 0, power_get() ? 1 : 0,
        s_field_a, s_field_b);

    if (n < 0 || n >= 320) { free(out); return NULL; }

    return out;
}

static void rx_history_add(const char *topic, const char *data)
{
    mqtt_rx_entry_t *e = &s_rx_history[s_rx_head];
    strncpy(e->topic, topic ? topic : "", MQTT_RX_TOPIC_LEN - 1);
    e->topic[MQTT_RX_TOPIC_LEN - 1] = '\0';
    strncpy(e->data, data ? data : "", MQTT_RX_DATA_LEN - 1);
    e->data[MQTT_RX_DATA_LEN - 1] = '\0';
    e->timestamp_ms = esp_timer_get_time() / 1000;
    e->used = true;
    s_rx_head = (s_rx_head + 1) % MQTT_RX_MAX_ENTRIES;
    if (s_rx_total < MQTT_RX_MAX_ENTRIES) s_rx_total++;
}

static void tx_history_add(const char *topic, const char *data, int msg_id)
{
    mqtt_tx_entry_t *e = &s_tx_history[s_tx_head];
    strncpy(e->topic, topic ? topic : "", MQTT_RX_TOPIC_LEN - 1);
    e->topic[MQTT_RX_TOPIC_LEN - 1] = '\0';
    strncpy(e->data, data ? data : "", MQTT_RX_DATA_LEN - 1);
    e->data[MQTT_RX_DATA_LEN - 1] = '\0';
    e->timestamp_ms = esp_timer_get_time() / 1000;
    e->msg_id = msg_id;
    e->used = true;
    s_tx_head = (s_tx_head + 1) % MQTT_TX_MAX_ENTRIES;
    if (s_tx_total < MQTT_TX_MAX_ENTRIES) s_tx_total++;
}

bool mqtt_is_connected(void) { return s_mqtt_connected; }

const char *mqtt_get_wd_a(void)
{
    return s_wd_a[0] ? s_wd_a : "--";
}

const char *mqtt_get_wd_b(void)
{
    return s_wd_b[0] ? s_wd_b : "--";
}

int mqtt_get_num(void) { return s_num_val; }

const char *mqtt_get_from(void)
{
    return s_from_src[0] ? s_from_src : "--";
}

const char *mqtt_get_last_rx_topic(void)
{
    if (s_rx_total <= 0) return "";
    int idx = (s_rx_head - 1 + MQTT_RX_MAX_ENTRIES) % MQTT_RX_MAX_ENTRIES;
    return s_rx_history[idx].topic;
}

const char *mqtt_get_last_rx_data(void)
{
    return s_last_payload;
}

int mqtt_get_msg_count(void) { return s_msg_count; }

const char *mqtt_get_msg_line(int idx)
{
    if (idx < 0 || idx >= s_msg_count) return "";
    return s_msg_lines[idx];
}

void mqtt_advance_msg_idx(void)
{
    if (s_msg_count <= 0) return;
    s_msg_idx = (s_msg_idx + 1) % s_msg_count;
}

int mqtt_get_msg_idx(void) { return s_msg_idx; }

int mqtt_get_all_msg_lines(char out[][128], int max_lines)
{
    if (!out || max_lines <= 0) return 0;
    int cnt = s_msg_count < max_lines ? s_msg_count : max_lines;
    for (int i = 0; i < cnt; i++) {
        strncpy(out[i], s_msg_lines[i], 127);
        out[i][127] = '\0';
    }
    return cnt;
}

static int hmac_sha256_hex(const char *key, size_t key_len,
                           const char *msg, size_t msg_len,
                           char *out, size_t out_size)
{
    if (out_size < 65) return -1;
    unsigned char digest[32];
    int ret = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                              (const unsigned char *)key, key_len,
                              (const unsigned char *)msg, msg_len,
                              digest);
    if (ret != 0) return -1;
    for (int i = 0; i < 32; i++) {
        sprintf(out + i * 2, "%02x", digest[i]);
    }
    out[64] = '\0';
    return 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        s_mqtt_state = MQTT_STATE_CONNECTED;
        led_notify_mqtt(true);
        ESP_LOGI(TAG, "Connected!");
        esp_mqtt_client_subscribe(s_mqtt_client, ALIYUN_TOPIC_USER, 0);
        ESP_LOGI(TAG, "Subscribed: USER");
        if (s_connect_sem) xSemaphoreGive(s_connect_sem);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        s_mqtt_state = MQTT_STATE_ERROR;
        led_notify_mqtt(false);
        ESP_LOGW(TAG, "Disconnected");
        break;
    case MQTT_EVENT_DATA: {
        led_status_rx_notify();

        char topic_buf[MQTT_RX_TOPIC_LEN] = {0};
        char data_buf[MQTT_RX_DATA_LEN] = {0};
        int tlen = event->topic_len < MQTT_RX_TOPIC_LEN - 1 ? event->topic_len : MQTT_RX_TOPIC_LEN - 1;
        int dlen = event->data_len < MQTT_RX_DATA_LEN - 1 ? event->data_len : MQTT_RX_DATA_LEN - 1;
        memcpy(topic_buf, event->topic, tlen);
        memcpy(data_buf, event->data, dlen);
        if (!strstr(topic_buf, "_reply")) {
            rx_history_add(topic_buf, data_buf);
        }

        cJSON *root = cJSON_ParseWithLength(data_buf, (size_t)dlen);
        if (!root) {
            ESP_LOGW(TAG, "RX not JSON, skip parse");
        } else {
            cJSON *j_id     = cJSON_GetObjectItemCaseSensitive(root, "DeviceID");
            cJSON *j_dir    = cJSON_GetObjectItemCaseSensitive(root, "Dir");
            cJSON *j_switch = cJSON_GetObjectItemCaseSensitive(root, "Switches");
            cJSON *j_light  = cJSON_GetObjectItemCaseSensitive(root, "light");
            cJSON *j_power  = cJSON_GetObjectItemCaseSensitive(root, "power");
            cJSON *j_field1  = cJSON_GetObjectItemCaseSensitive(root, "Field1");
            cJSON *j_field2  = cJSON_GetObjectItemCaseSensitive(root, "Field2");
            cJSON *j_set1   = cJSON_GetObjectItemCaseSensitive(root, "Set1");
            if (!j_set1) j_set1 = cJSON_GetObjectItemCaseSensitive(root, "SetValue1");
            cJSON *j_set2   = cJSON_GetObjectItemCaseSensitive(root, "Set2");
            if (!j_set2) j_set2 = cJSON_GetObjectItemCaseSensitive(root, "SetValue2");

            const char *id_str  = j_id  && cJSON_IsString(j_id)  ? j_id->valuestring  : "";
            const char *dir_str = j_dir && cJSON_IsString(j_dir) ? j_dir->valuestring : "";

            if (strncmp(id_str, DEVICE_ID_PREFIX, strlen(DEVICE_ID_PREFIX)) == 0) {
                if (strcmp(dir_str, "C>D") == 0) {
                    ESP_LOGI(TAG, "RX  %s", data_buf);

                    if (j_switch && cJSON_IsNumber(j_switch)) {
                        sw0_set((j_switch->valueint & 1) != 0);
                        sw_bit1_set((j_switch->valueint & 2) != 0);
                    }
                    if (j_light && cJSON_IsNumber(j_light)) {
                        switch1_set(j_light->valueint != 0);
                    }
                    if (j_power && cJSON_IsNumber(j_power)) {
                        power_set(j_power->valueint != 0);
                    }
                    int ack_switches_val = (j_switch && cJSON_IsNumber(j_switch)) ? j_switch->valueint : -1;
                    int ack_light_val    = (j_light  && cJSON_IsNumber(j_light))  ? j_light->valueint  : -1;
                    int ack_power_val    = (j_power  && cJSON_IsNumber(j_power))  ? j_power->valueint  : -1;
                    float ack_set1_val = 0; bool ack_set1_ok = false;
                    float ack_set2_val = 0; bool ack_set2_ok = false;
                    float ack_field1_val = 0; bool ack_field1_ok = false;
                    float ack_field2_val = 0; bool ack_field2_ok = false;
                    if (j_field1 && cJSON_IsNumber(j_field1)) {
                        ack_field1_val = (float)j_field1->valuedouble;
                        s_field_a = ack_field1_val;
                        ack_field1_ok = true;
                    }
                    if (j_field2 && cJSON_IsNumber(j_field2)) {
                        ack_field2_val = (float)j_field2->valuedouble;
                        s_field_b = ack_field2_val;
                        ack_field2_ok = true;
                    }
                    if (j_set1 && cJSON_IsNumber(j_set1)) {
                        ack_set1_val = (float)j_set1->valuedouble;
                        s_set_a = ack_set1_val;
                        snprintf(s_wd_b, sizeof(s_wd_b), "%.2f", s_set_a);
                        ack_set1_ok = true;
                    }
                    if (j_set2 && cJSON_IsNumber(j_set2)) {
                        ack_set2_val = (float)j_set2->valuedouble;
                        s_set_b = ack_set2_val;
                        ack_set2_ok = true;
                    }

                    s_msg_count = 0;
                    s_msg_idx = 0;
                    cJSON *child = root->child;
                    while (child && s_msg_count < MQTT_MSG_MAX_LINES) {
                        const char *key = child->string ? child->string : "";
                        if (cJSON_IsString(child))
                            snprintf(s_msg_lines[s_msg_count], sizeof(s_msg_lines[0]),
                                     "%s=%s", key, child->valuestring);
                        else if (cJSON_IsNumber(child))
                            snprintf(s_msg_lines[s_msg_count], sizeof(s_msg_lines[0]),
                                     "%s=%.4g", key, child->valuedouble);
                        s_msg_count++;
                        child = child->next;
                    }

                    vTaskDelay(pdMS_TO_TICKS(5));

                    {
                        float ack_temp = roundf(temp_sensor_get() * 10.0f) / 10.0f;
                        int   ack_rssi = wifi_is_connected() ? wifi_get_rssi() : -127;
                        char ack_buf[384];
                        int n = snprintf(ack_buf, sizeof(ack_buf),
                            "{\"DeviceID\":\"%s\",\"Dir\":\"ACK\",\"Temp\":%.1f,\"RSSI\":%d",
                            DEVICE_ID, ack_temp, ack_rssi);
                        if (ack_switches_val >= 0) n += snprintf(ack_buf + n, sizeof(ack_buf) - n,
                            ",\"Switches\":%d", ack_switches_val);
                        if (ack_light_val >= 0) n += snprintf(ack_buf + n, sizeof(ack_buf) - n,
                            ",\"light\":%d", ack_light_val);
                        if (ack_power_val >= 0) n += snprintf(ack_buf + n, sizeof(ack_buf) - n,
                            ",\"power\":%d", ack_power_val);
                        if (ack_field1_ok) n += snprintf(ack_buf + n, sizeof(ack_buf) - n,
                            ",\"Field1\":%.2f", ack_field1_val);
                        if (ack_field2_ok) n += snprintf(ack_buf + n, sizeof(ack_buf) - n,
                            ",\"Field2\":%.2f", ack_field2_val);
                        if (ack_set1_ok) n += snprintf(ack_buf + n, sizeof(ack_buf) - n,
                            ",\"Set1\":%.2f", ack_set1_val);
                        if (ack_set2_ok) n += snprintf(ack_buf + n, sizeof(ack_buf) - n,
                            ",\"Set2\":%.2f", ack_set2_val);
                        n += snprintf(ack_buf + n, sizeof(ack_buf) - n, "}");
                        if (n > 0 && n < (int)sizeof(ack_buf)) {
                            mqtt_publish_custom(ALIYUN_TOPIC_USER_UPDATE, ack_buf, 0);
                        }
                    }
                } else if (strcmp(dir_str, "D>C") == 0) {
                }
            } else {
                ESP_LOGW(TAG, "Device ID mismatch: got '%s' expect prefix '%s'", id_str, DEVICE_ID_PREFIX);
            }
            cJSON_Delete(root);
        }

        strncpy(s_last_payload, data_buf, sizeof(s_last_payload) - 1);
        s_last_payload[sizeof(s_last_payload) - 1] = '\0';
        ui_notify_new_msg();
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT ERROR");
        if (event->error_handle) {
            ESP_LOGE(TAG, "  type=%d rc=%d sock_errno=%d",
                     event->error_handle->error_type,
                     event->error_handle->connect_return_code,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;
    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGI(TAG, "Connecting to broker...");
        break;
    default:
        break;
    }
}

static esp_err_t mqtt_create_and_start_client(void)
{
    time_t now_s = time(NULL);
    int64_t ts_ms = (int64_t)now_s * 1000;
    char timestamp[20];
    snprintf(timestamp, sizeof(timestamp), "%lld", (long long)ts_ms);

    snprintf(s_client_id, sizeof(s_client_id),
             "%s|securemode=2,signmethod=hmacsha256,timestamp=%s|",
             ALIYUN_CLIENT_ID, timestamp);

    char sign_content[192];
    snprintf(sign_content, sizeof(sign_content),
             "clientId%sdeviceName%sproductKey%stimestamp%s",
             ALIYUN_CLIENT_ID, ALIYUN_DEVICE_NAME, ALIYUN_PRODUCT_KEY, timestamp);

    if (hmac_sha256_hex(ALIYUN_DEVICE_SECRET, strlen(ALIYUN_DEVICE_SECRET),
                        sign_content, strlen(sign_content),
                        s_password, sizeof(s_password)) != 0) {
        ESP_LOGE(TAG, "HMAC password generate failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "=== Aliyun MQTT Config ===");
    ESP_LOGI(TAG, "Broker   : %s", ALIYUN_BROKER_URI);
    ESP_LOGI(TAG, "ClientID : %s", s_client_id);
    ESP_LOGI(TAG, "Username : %s", ALIYUN_USERNAME);
    ESP_LOGI(TAG, "Password : %s", s_password);
    ESP_LOGI(TAG, "Time     : %s", ctime(&now_s));
    ESP_LOGI(TAG, "========================");

    if (s_mqtt_client) {
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = ALIYUN_BROKER_URI,
        .credentials.client_id = s_client_id,
        .credentials.username = ALIYUN_USERNAME,
        .credentials.authentication.password = s_password,
        .session.keepalive = 120,
        .session.disable_clean_session = false,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .network.disable_auto_reconnect = false,
        .buffer.size = 1024,
        .buffer.out_size = 1024,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init FAILED");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    if (!s_connect_sem) {
        s_connect_sem = xSemaphoreCreateBinary();
    }

    if (esp_mqtt_client_start(s_mqtt_client) != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start FAILED");
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void mqtt_manager_task(void *arg)
{
    int retry_count = 0;
    int temp_tick = 0;

    while (1) {
        switch (s_mqtt_state) {

        case MQTT_STATE_IDLE:
            s_mqtt_state = MQTT_STATE_WAIT_WIFI;
            break;

        case MQTT_STATE_WAIT_WIFI:
            if (wifi_is_connected()) {
                retry_count = 0;
                ESP_LOGI(TAG, "WiFi OK, checking time sync...");
                s_mqtt_state = MQTT_STATE_WAIT_TIME;
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            break;

        case MQTT_STATE_WAIT_TIME: {
            int waited = 0;
            time_t now_s = time(NULL);
            ESP_LOGI(TAG, "Waiting for time sync...");
            while (now_s < 1000000000 && waited < 20000) {
                vTaskDelay(pdMS_TO_TICKS(300));
                waited += 300;
                now_s = time(NULL);
            }
            if (now_s < 1000000000) {
                ESP_LOGW(TAG, "Time still invalid after %dms, proceeding anyway", waited);
            } else {
                ESP_LOGI(TAG, "Time synced after %dms: %s", waited, ctime(&now_s));
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            s_mqtt_state = MQTT_STATE_INIT;
            break;
        }

        case MQTT_STATE_INIT:
            if (mqtt_create_and_start_client() == ESP_OK) {
                s_mqtt_state = MQTT_STATE_CONNECTING;
                ESP_LOGI(TAG, "Client started, waiting for connection...");
            } else {
                ESP_LOGE(TAG, "Client create failed, retry in 5s...");
                s_mqtt_state = MQTT_STATE_ERROR;
            }
            break;

        case MQTT_STATE_CONNECTING: {
            if (!wifi_is_connected()) {
                ESP_LOGW(TAG, "WiFi lost while connecting, back to WAIT_WIFI");
                s_mqtt_state = MQTT_STATE_WAIT_WIFI;
                break;
            }
            if (s_connect_sem && xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS)) == pdTRUE) {
                ESP_LOGI(TAG, "MQTT connected!");
                s_mqtt_state = MQTT_STATE_CONNECTED;
            } else {
                ESP_LOGW(TAG, "Connect timeout, retry...");
                if (s_mqtt_client) {
                    esp_mqtt_client_disconnect(s_mqtt_client);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                s_mqtt_state = MQTT_STATE_ERROR;
            }
            break;
        }

        case MQTT_STATE_CONNECTED:
            if (!wifi_is_connected()) {
                ESP_LOGW(TAG, "WiFi disconnected, waiting...");
                s_mqtt_state = MQTT_STATE_WAIT_WIFI;
                break;
            }
            if (!s_mqtt_connected) {
                ESP_LOGW(TAG, "MQTT lost, recreate...");
                s_mqtt_state = MQTT_STATE_ERROR;
                break;
            }
            temp_tick++;
#if MQTT_PERIODIC_TX_ENABLED
            if (temp_tick >= 15) {
                temp_tick = 0;
                char *frame = mqtt_build_tx_frame();
                if (frame) {
                    mqtt_publish_custom(ALIYUN_TOPIC_USER_UPDATE, frame, 0);
                    free(frame);
                }
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(2000));
            break;

        case MQTT_STATE_ERROR:
            retry_count++;
            ESP_LOGW(TAG, "Retry #%d, delay %dms...", retry_count, MQTT_RETRY_DELAY_MS);
            if (s_mqtt_client) {
                esp_mqtt_client_stop(s_mqtt_client);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_mqtt_client_destroy(s_mqtt_client);
                s_mqtt_client = NULL;
            }
            vTaskDelay(pdMS_TO_TICKS(MQTT_RETRY_DELAY_MS));
            if (!wifi_is_connected()) {
                s_mqtt_state = MQTT_STATE_WAIT_WIFI;
            } else {
                s_mqtt_state = MQTT_STATE_INIT;
            }
            break;

        default:
            s_mqtt_state = MQTT_STATE_WAIT_WIFI;
            break;
        }
    }
}

void mqtt_init(int64_t start_time_ms)
{
    s_start_time_ms = start_time_ms;
    s_mqtt_state = MQTT_STATE_IDLE;

    xTaskCreate(mqtt_manager_task, "mqtt_mgr", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "MQTT manager task started");
}

esp_err_t mqtt_publish_custom(const char *topic, const char *data, int qos)
{
    if (!s_mqtt_connected || !s_mqtt_client) {
        led_status_tx_notify();
        return ESP_ERR_INVALID_STATE;
    }
    if (!topic || strlen(topic) == 0) return ESP_ERR_INVALID_ARG;
    if (!data) data = "";

    int id = ++s_tx_msg_id;
    int len = strlen(data);
    int rc = esp_mqtt_client_publish(s_mqtt_client, topic, data, len, qos, 0);
    if (rc >= 0) {
        ESP_LOGI(TAG, "TX id=%d %s", rc, data);
        led_status_tx_notify();
        tx_history_add(topic, data, rc);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "TX failed rc=%d topic=%s", rc, topic);
        return ESP_FAIL;
    }
}

esp_err_t mqtt_publish_user_update(const char *data)
{
    char *frame = mqtt_build_tx_frame();
    if (!frame) return ESP_FAIL;
    esp_err_t err = mqtt_publish_custom(ALIYUN_TOPIC_USER_UPDATE, frame, 0);
    free(frame);
    return err;
}

esp_err_t mqtt_publish_aliyun_params(const char *params_json)
{
    return mqtt_publish_custom(ALIYUN_TOPIC_USER_UPDATE, params_json, 0);
}

int mqtt_get_rx_entries(mqtt_rx_entry_t *out, int max_count)
{
    if (!out || max_count <= 0) return 0;
    int count = s_rx_total < max_count ? s_rx_total : max_count;
    for (int i = 0; i < count; i++) {
        int idx = (s_rx_head - 1 - i + MQTT_RX_MAX_ENTRIES) % MQTT_RX_MAX_ENTRIES;
        out[i] = s_rx_history[idx];
    }
    return count;
}

int mqtt_get_tx_entries(mqtt_tx_entry_t *out, int max_count)
{
    if (!out || max_count <= 0) return 0;
    int count = s_tx_total < max_count ? s_tx_total : max_count;
    for (int i = 0; i < count; i++) {
        int idx = (s_tx_head - 1 - i + MQTT_TX_MAX_ENTRIES) % MQTT_TX_MAX_ENTRIES;
        out[i] = s_tx_history[idx];
    }
    return count;
}

void mqtt_clear_rx_history(void)
{
    s_rx_head = 0;
    s_rx_total = 0;
    memset(s_rx_history, 0, sizeof(s_rx_history));
}

void mqtt_clear_tx_history(void)
{
    s_tx_head = 0;
    s_tx_total = 0;
    memset(s_tx_history, 0, sizeof(s_tx_history));
}