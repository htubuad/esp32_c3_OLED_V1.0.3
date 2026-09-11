#ifndef MQTT_ALIYUN_H
#define MQTT_ALIYUN_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "version.h"

#define DEVICE_ID           "001_" APP_VERSION

#define MQTT_RX_MAX_ENTRIES 32
#define MQTT_RX_TOPIC_LEN   128
#define MQTT_RX_DATA_LEN    256
#define MQTT_TX_MAX_ENTRIES 32

typedef struct {
    char topic[MQTT_RX_TOPIC_LEN];
    char data[MQTT_RX_DATA_LEN];
    int64_t timestamp_ms;
    bool used;
} mqtt_rx_entry_t;

typedef struct {
    char topic[MQTT_RX_TOPIC_LEN];
    char data[MQTT_RX_DATA_LEN];
    int64_t timestamp_ms;
    int msg_id;
    bool used;
} mqtt_tx_entry_t;

bool mqtt_is_connected(void);
void mqtt_init(int64_t start_time_ms);
const char *mqtt_get_wd_a(void);
const char *mqtt_get_wd_b(void);
int mqtt_get_num(void);
const char *mqtt_get_from(void);
const char *mqtt_get_last_rx_topic(void);
const char *mqtt_get_last_rx_data(void);
int mqtt_get_msg_count(void);
const char *mqtt_get_msg_line(int idx);
void mqtt_advance_msg_idx(void);
int mqtt_get_msg_idx(void);
int mqtt_get_all_msg_lines(char out[][128], int max_lines);

esp_err_t mqtt_publish_custom(const char *topic, const char *data, int qos);
esp_err_t mqtt_publish_aliyun_params(const char *params_json);
esp_err_t mqtt_publish_user_update(const char *data);
int mqtt_get_rx_entries(mqtt_rx_entry_t *out, int max_count);
int mqtt_get_tx_entries(mqtt_tx_entry_t *out, int max_count);
void mqtt_clear_rx_history(void);
void mqtt_clear_tx_history(void);

#endif