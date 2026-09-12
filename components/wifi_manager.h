#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_wifi_types.h"

#define WIFI_MAX_SSID_LEN    33
#define WIFI_MAX_PASS_LEN    65
#define WIFI_MAX_AP_COUNT    10

#define WIFI_AP_DEFAULT_SSID       "DEV-MGT_26001"
#define WIFI_AP_IP                 "192.168.4.1"

typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_INIT,
    WIFI_STATE_STA_CONNECT,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_AP,
    WIFI_STATE_STA_ONLY,
} wifi_state_t;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char password[WIFI_MAX_PASS_LEN];
} wifi_cred_t;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    int rssi;
    wifi_auth_mode_t auth;
    uint8_t channel;
} wifi_ap_info_t;

wifi_state_t wifi_get_state(void);
bool wifi_is_connected(void);
bool wifi_is_ap_active(void);
const char *wifi_get_ip(void);
const char *wifi_get_sta_ip(void);
const char *wifi_get_ap_ip(void);
int wifi_get_rssi(void);
const char *wifi_get_status_text(void);

void wifi_manager_start(void);
void wifi_manager_request_connect(const char *ssid, const char *password);
void wifi_manager_request_clear_and_ap(void);
void wifi_manager_request_close_ap(void);
void wifi_manager_request_open_ap(void);

esp_err_t wifi_scan_aps(wifi_ap_info_t *out, uint16_t *count);

bool wifi_cred_load(wifi_cred_t *cred);
bool wifi_cred_save(const wifi_cred_t *cred);
void wifi_cred_clear(void);

void wifi_update_rssi(void);

#endif