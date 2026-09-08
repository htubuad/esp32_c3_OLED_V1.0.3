#include "mqtt_aliyun.h"
#include "wifi_manager.h"
#include "led.h"
#include "temp_sensor.h"
#include "version.h"
#include "oled.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "mbedtls/md.h"
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

#define ALIYUN_TOPIC_STATUS      "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/thing/event/property/post"
#define ALIYUN_TOPIC_CMD         "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/thing/service/property/set"
#define ALIYUN_TOPIC_CMD_REPLY   "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/thing/service/property/set_reply"
#define ALIYUN_TOPIC_GET         "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/thing/service/property/get"
#define ALIYUN_TOPIC_GET_REPLY   "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/thing/service/property/get_reply"
#define ALIYUN_TOPIC_REPLY       "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/thing/event/property/post_reply"
#define ALIYUN_TOPIC_USER        "/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/user/get"

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

#define MQTT_MSG_Y_START  48
#define MQTT_MSG_FONT     OLED_SIZE_16
#define MQTT_MSG_LINE_H   16
#define MQTT_MSG_MAX_LINES 5

static char s_client_id[128];
static char s_password[72];

static char s_wd_a[128] = {0};
static char s_wd_b[128] = {0};

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

static void mqtt_send_cmd_reply(const char *id, int code, const char *message)
{
    if (!s_mqtt_connected || !s_mqtt_client) return;

    cJSON *root = cJSON_CreateObject();
    if (id && id[0]) cJSON_AddStringToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message ? message : "");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    esp_mqtt_client_publish(s_mqtt_client, ALIYUN_TOPIC_CMD_REPLY, payload, 0, 0, 0);
    ESP_LOGI(TAG, "CMD reply code=%d msg=%s", code, message ? message : "");
    free(payload);
}

static void mqtt_send_get_reply(const char *id, int code, const char *message, cJSON *data)
{
    if (!s_mqtt_connected || !s_mqtt_client) return;

    cJSON *root = cJSON_CreateObject();
    if (id && id[0]) cJSON_AddStringToObject(root, "id", id);
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    if (data) cJSON_AddItemToObject(root, "data", data);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    esp_mqtt_client_publish(s_mqtt_client, ALIYUN_TOPIC_GET_REPLY, payload, 0, 0, 0);
    ESP_LOGI(TAG, "GET reply: %s", payload);
    free(payload);
}

static cJSON *build_all_properties(void)
{
    cJSON *data = cJSON_CreateObject();
    bool wifi_ok = wifi_is_connected();
    cJSON_AddBoolToObject(data, "LedSwitch", led_get());
    cJSON_AddNumberToObject(data, "temperature", temp_sensor_get());
    cJSON_AddNumberToObject(data, "WiFiRSSI", wifi_ok ? wifi_get_rssi() : 0);
    cJSON_AddStringToObject(data, "DeviceIP", wifi_ok ? wifi_get_ip() : "");
    cJSON_AddStringToObject(data, "FirmwareVersion", APP_VERSION);
    cJSON_AddNumberToObject(data, "UptimeMs", (double)(esp_timer_get_time() / 1000 - s_start_time_ms));
    cJSON_AddNumberToObject(data, "FreeHeap", (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return data;
}

static cJSON *fill_requested_properties(cJSON *data, cJSON *params)
{
    if (!params || !cJSON_IsArray(params)) return data;

    bool wifi_ok = wifi_is_connected();
    bool matched_any = false;
    cJSON *item = params->child;
    while (item) {
        const char *name = item->valuestring;
        if (!name) { item = item->next; continue; }

        if (strcasecmp(name, "LedSwitch") == 0) {
            cJSON_AddBoolToObject(data, "LedSwitch", led_get());
            matched_any = true;
        } else if (strcasecmp(name, "temperature") == 0) {
            cJSON_AddNumberToObject(data, "temperature", temp_sensor_get());
            matched_any = true;
        } else if (strcasecmp(name, "WiFiRSSI") == 0) {
            cJSON_AddNumberToObject(data, "WiFiRSSI", wifi_ok ? wifi_get_rssi() : 0);
            matched_any = true;
        } else if (strcasecmp(name, "DeviceIP") == 0) {
            cJSON_AddStringToObject(data, "DeviceIP", wifi_ok ? wifi_get_ip() : "");
            matched_any = true;
        } else if (strcasecmp(name, "FirmwareVersion") == 0) {
            cJSON_AddStringToObject(data, "FirmwareVersion", APP_VERSION);
            matched_any = true;
        } else if (strcasecmp(name, "UptimeMs") == 0) {
            cJSON_AddNumberToObject(data, "UptimeMs", (double)(esp_timer_get_time() / 1000 - s_start_time_ms));
            matched_any = true;
        } else if (strcasecmp(name, "FreeHeap") == 0) {
            cJSON_AddNumberToObject(data, "FreeHeap", (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            matched_any = true;
        }
        item = item->next;
    }

    if (!matched_any) {
        cJSON_Delete(data);
        return build_all_properties();
    }
    return data;
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
        ESP_LOGI(TAG, "Connected!");
        esp_mqtt_client_subscribe(s_mqtt_client, ALIYUN_TOPIC_CMD, 0);
        esp_mqtt_client_subscribe(s_mqtt_client, ALIYUN_TOPIC_GET, 0);
        esp_mqtt_client_subscribe(s_mqtt_client, ALIYUN_TOPIC_REPLY, 0);
        esp_mqtt_client_subscribe(s_mqtt_client, ALIYUN_TOPIC_USER, 0);
        ESP_LOGI(TAG, "Subscribed: CMD/GET/REPLY/USER");
        if (s_connect_sem) xSemaphoreGive(s_connect_sem);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        s_mqtt_state = MQTT_STATE_ERROR;
        ESP_LOGW(TAG, "Disconnected");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Rx topic=%.*s data=%.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);

        {
            char topic_buf[MQTT_RX_TOPIC_LEN] = {0};
            char data_buf[MQTT_RX_DATA_LEN] = {0};
            int tlen = event->topic_len < MQTT_RX_TOPIC_LEN - 1 ? event->topic_len : MQTT_RX_TOPIC_LEN - 1;
            int dlen = event->data_len < MQTT_RX_DATA_LEN - 1 ? event->data_len : MQTT_RX_DATA_LEN - 1;
            memcpy(topic_buf, event->topic, tlen);
            memcpy(data_buf, event->data, dlen);
            rx_history_add(topic_buf, data_buf);
        }

        {
            cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
            if (!root) break;

            cJSON *params = cJSON_GetObjectItem(root, "params");
            if (params) {
                cJSON *wd_a = cJSON_GetObjectItem(params, "wd_a");
                if (wd_a) {
                    char tmp[24];
                    if (cJSON_IsString(wd_a)) {
                        snprintf(tmp, sizeof(tmp), "%s", wd_a->valuestring);
                    } else if (cJSON_IsNumber(wd_a)) {
                        snprintf(tmp, sizeof(tmp), "%.1f", wd_a->valuedouble);
                    } else if (cJSON_IsBool(wd_a)) {
                        snprintf(tmp, sizeof(tmp), "%s", cJSON_IsTrue(wd_a) ? "1" : "0");
                    } else {
                        tmp[0] = '\0';
                    }
                    strncpy(s_wd_a, tmp, sizeof(s_wd_a) - 1);
                    s_wd_a[sizeof(s_wd_a) - 1] = '\0';
                }

                cJSON *wd_b = cJSON_GetObjectItem(params, "wd_b");
                if (wd_b) {
                    char tmp[24];
                    if (cJSON_IsString(wd_b)) {
                        snprintf(tmp, sizeof(tmp), "%s", wd_b->valuestring);
                    } else if (cJSON_IsNumber(wd_b)) {
                        snprintf(tmp, sizeof(tmp), "%.1f", wd_b->valuedouble);
                    } else if (cJSON_IsBool(wd_b)) {
                        snprintf(tmp, sizeof(tmp), "%s", cJSON_IsTrue(wd_b) ? "1" : "0");
                    } else {
                        tmp[0] = '\0';
                    }
                    strncpy(s_wd_b, tmp, sizeof(s_wd_b) - 1);
                    s_wd_b[sizeof(s_wd_b) - 1] = '\0';
                }

                cJSON *child = params->child;
                s_msg_count = 0;
                s_msg_idx = 0;
                bool wd_a_in_list = false;

                while (child && s_msg_count < MQTT_MSG_MAX_LINES) {
                    char val_str[16];
                    const char *key_name = child->string;
                    const char *val_text = NULL;

                    if (cJSON_IsBool(child)) {
                        val_text = cJSON_IsTrue(child) ? "ON" : "OFF";
                    } else if (cJSON_IsNumber(child)) {
                        double v = child->valuedouble;
                        if (v == (double)(long long)v) {
                            snprintf(val_str, sizeof(val_str), "%lld", (long long)v);
                        } else {
                            snprintf(val_str, sizeof(val_str), "%.3f", v);
                            int len = strlen(val_str);
                            while (len > 0 && val_str[len-1] == '0') val_str[--len] = '\0';
                            if (len > 0 && val_str[len-1] == '.') val_str[--len] = '\0';
                        }
                    } else if (cJSON_IsString(child)) {
                        snprintf(val_str, sizeof(val_str), "%s", child->valuestring);
                    }

                    if (key_name) {
                        if (strcmp(key_name, "wd_a") == 0) wd_a_in_list = true;
                        char line[32];
                        int n = snprintf(line, sizeof(line), "%s=", key_name);
                        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
                        const char *disp = val_text ? val_text : val_str;
                        snprintf(line + n, sizeof(line) - n, "%s", disp ? disp : "?");
                        strncpy(s_msg_lines[s_msg_count], line, sizeof(s_msg_lines[s_msg_count]) - 1);
                        s_msg_lines[s_msg_count][sizeof(s_msg_lines[s_msg_count]) - 1] = '\0';
                        s_msg_count++;
                    }

                    child = child->next;
                }

                if (!wd_a_in_list && s_wd_a[0] && s_msg_count < MQTT_MSG_MAX_LINES) {
                    snprintf(s_msg_lines[s_msg_count], sizeof(s_msg_lines[s_msg_count]), "wd_a=");
                    strncat(s_msg_lines[s_msg_count], s_wd_a,
                            sizeof(s_msg_lines[s_msg_count]) - strlen(s_msg_lines[s_msg_count]) - 1);
                    s_msg_count++;
                }

                bool led_on = false;
                bool has_led = false;
                cJSON *led = cJSON_GetObjectItem(params, "LedSwitch");
                if (led) {
                    led_on = cJSON_IsTrue(led) || (cJSON_IsNumber(led) && led->valueint != 0);
                    has_led = true;
                }
                cJSON *power = cJSON_GetObjectItem(params, "Power");
                if (power) {
                    led_on = cJSON_IsTrue(power) || (cJSON_IsNumber(power) && power->valueint != 0);
                    has_led = true;
                }
                if (has_led) {
                    led_set(led_on);
                    ESP_LOGI(TAG, "LED %s (cloud cmd)", led_on ? "ON" : "OFF");
                }
            } else {
                s_msg_count = 0;
                s_msg_idx = 0;
                cJSON *child = root->child;
                while (child && s_msg_count < MQTT_MSG_MAX_LINES) {
                    char val_str[24];
                    const char *key_name = child->string;
                    const char *val_text = NULL;

                    if (cJSON_IsBool(child)) {
                        val_text = cJSON_IsTrue(child) ? "ON" : "OFF";
                    } else if (cJSON_IsNumber(child)) {
                        double v = child->valuedouble;
                        if (v == (double)(long long)v) {
                            snprintf(val_str, sizeof(val_str), "%lld", (long long)v);
                        } else {
                            snprintf(val_str, sizeof(val_str), "%.1f", v);
                        }
                    } else if (cJSON_IsString(child)) {
                        snprintf(val_str, sizeof(val_str), "%s", child->valuestring);
                    } else {
                        child = child->next;
                        continue;
                    }

                    if (key_name) {
                        char line[32];
                        int n = snprintf(line, sizeof(line), "%s:", key_name);
                        const char *disp = val_text ? val_text : val_str;
                        snprintf(line + n, sizeof(line) - n, "%s", disp ? disp : "?");
                        strncpy(s_msg_lines[s_msg_count], line, sizeof(s_msg_lines[s_msg_count]) - 1);
                        s_msg_lines[s_msg_count][sizeof(s_msg_lines[s_msg_count]) - 1] = '\0';
                        s_msg_count++;
                    }
                    child = child->next;
                }
                ESP_LOGI(TAG, "Flat JSON parsed, %d msg lines", s_msg_count);
            }

            {
                int tlen = event->topic_len;
                const char *topic = event->topic;
                bool is_cmd = (tlen == (int)strlen(ALIYUN_TOPIC_CMD)) &&
                              (memcmp(topic, ALIYUN_TOPIC_CMD, tlen) == 0);
                if (is_cmd) {
                    const char *msg_id = "";
                    cJSON *id_item = cJSON_GetObjectItem(root, "id");
                    if (id_item && cJSON_IsString(id_item)) msg_id = id_item->valuestring;

                    int reply_code = 200;
                    const char *reply_msg = "success";

                    mqtt_send_cmd_reply(msg_id, reply_code, reply_msg);

                    vTaskDelay(pdMS_TO_TICKS(100));
                    mqtt_publish_status();
                }

                bool is_get = (tlen == (int)strlen(ALIYUN_TOPIC_GET)) &&
                              (memcmp(topic, ALIYUN_TOPIC_GET, tlen) == 0);
                if (is_get) {
                    const char *msg_id = "";
                    cJSON *id_item = cJSON_GetObjectItem(root, "id");
                    if (id_item && cJSON_IsString(id_item)) msg_id = id_item->valuestring;

                    cJSON *params = cJSON_GetObjectItem(root, "params");
                    cJSON *data = cJSON_CreateObject();

                    if (params && cJSON_IsArray(params)) {
                        data = fill_requested_properties(data, params);
                    } else {
                        cJSON_Delete(data);
                        data = build_all_properties();
                    }

                    ESP_LOGI(TAG, "GET request id=%s params=%s",
                             msg_id, params ? "array" : "all");
                    mqtt_send_get_reply(msg_id, 200, "success", data);
                }
            }

            cJSON_Delete(root);
        }
        break;
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

esp_err_t mqtt_publish_status(void)
{
    if (!s_mqtt_connected || !s_mqtt_client) return ESP_ERR_INVALID_STATE;

    char id[24];
    snprintf(id, sizeof(id), "%lu", (unsigned long)(esp_timer_get_time() / 1000));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddStringToObject(root, "method", "thing.event.property.post");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "params", params);

    bool wifi_ok = wifi_is_connected();
    cJSON_AddBoolToObject(params, "LedSwitch", led_get());
    cJSON_AddNumberToObject(params, "temperature", temp_sensor_get());
    cJSON_AddNumberToObject(params, "WiFiRSSI", wifi_ok ? wifi_get_rssi() : 0);
    cJSON_AddStringToObject(params, "DeviceIP", wifi_ok ? wifi_get_ip() : "");
    cJSON_AddStringToObject(params, "FirmwareVersion", APP_VERSION);
    cJSON_AddNumberToObject(params, "UptimeMs", (double)(esp_timer_get_time() / 1000 - s_start_time_ms));
    cJSON_AddNumberToObject(params, "FreeHeap", (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return ESP_FAIL;

    esp_mqtt_client_publish(s_mqtt_client, ALIYUN_TOPIC_STATUS, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Post property (id=%s)", id);
    free(payload);
    return ESP_OK;
}

static void mqtt_manager_task(void *arg)
{
    int retry_count = 0;

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
    if (!s_mqtt_connected || !s_mqtt_client) return ESP_ERR_INVALID_STATE;
    if (!topic || strlen(topic) == 0) return ESP_ERR_INVALID_ARG;
    if (!data) data = "";

    int id = ++s_tx_msg_id;
    int len = strlen(data);
    int rc = esp_mqtt_client_publish(s_mqtt_client, topic, data, len, qos, 0);
    if (rc >= 0) {
        ESP_LOGI(TAG, "TX id=%d topic=%s", rc, topic);
        tx_history_add(topic, data, rc);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "TX failed rc=%d topic=%s", rc, topic);
        return ESP_FAIL;
    }
}

esp_err_t mqtt_publish_aliyun_params(const char *params_json)
{
    if (!s_mqtt_connected || !s_mqtt_client) return ESP_ERR_INVALID_STATE;
    if (!params_json || strlen(params_json) == 0) return ESP_ERR_INVALID_ARG;

    cJSON *params = cJSON_Parse(params_json);
    if (!params) {
        ESP_LOGW(TAG, "params_json invalid, wrap as string");
        params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "raw", params_json);
    }

    char id[24];
    snprintf(id, sizeof(id), "%lu", (unsigned long)(esp_timer_get_time() / 1000));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddStringToObject(root, "method", "thing.event.property.post");
    cJSON_AddItemToObject(root, "params", params);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return ESP_FAIL;

    int rc = esp_mqtt_client_publish(s_mqtt_client, ALIYUN_TOPIC_STATUS, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Aliyun post id=%s rc=%d", id, rc);

    if (rc >= 0) {
        tx_history_add(ALIYUN_TOPIC_STATUS, payload, rc);
    }

    free(payload);
    return (rc >= 0) ? ESP_OK : ESP_FAIL;
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