#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_wifi_types.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_SCAN_DONE_BIT  BIT2

#define WIFI_MAX_SSID_LEN    33
#define WIFI_MAX_PASS_LEN    65
#define WIFI_MAX_AP_COUNT    10

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

bool wifi_is_connected(void);
const char *wifi_get_ip(void);
int wifi_get_rssi(void);
EventGroupHandle_t wifi_get_event_group(void);
void wifi_init_sta(void);
void wifi_update_rssi(void);

bool wifi_cred_load(wifi_cred_t *cred);
bool wifi_cred_save(const wifi_cred_t *cred);
void wifi_cred_clear(void);

esp_err_t wifi_scan_aps(wifi_ap_info_t *out, uint16_t *count);
esp_err_t wifi_try_connect(const char *ssid, const char *password);

void wifi_start_ap(const char *ap_ssid, const char *ap_password);
bool wifi_is_ap_active(void);

#endif