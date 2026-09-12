#ifndef MQTT_ALIYUN_H
#define MQTT_ALIYUN_H

#include <stdbool.h>
#include "esp_err.h"
#include "version.h"

#define DEVICE_ID           "001_" APP_VERSION
#define DEVICE_ID_PREFIX    "001"

#define MQTT_RX_MAX_ENTRIES 8
#define MQTT_RX_TOPIC_LEN   64
#define MQTT_RX_DATA_LEN    256

typedef struct {
    char topic[MQTT_RX_TOPIC_LEN];
    char data[MQTT_RX_DATA_LEN];
    int64_t timestamp_ms;
    bool used;
} mqtt_rx_entry_t;

bool mqtt_is_connected(void);
void mqtt_init(void);
const char *mqtt_get_last_rx_topic(void);
const char *mqtt_get_last_rx_data(void);

esp_err_t mqtt_publish_custom(const char *topic, const char *data, int qos);
esp_err_t mqtt_publish_aliyun_params(const char *params_json);
int mqtt_get_rx_entries(mqtt_rx_entry_t *out, int max_count);

#endif