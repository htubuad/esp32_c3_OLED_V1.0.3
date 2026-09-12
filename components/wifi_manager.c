#include "wifi_manager.h"
#include "led.h"
#include "dns_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/task.h"
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>

#include "lwip/netif.h"
#include "lwip/apps/mdns.h"

#define MDNS_HOSTNAME "esp32c3"

static const char *TAG = "WIFI";

static bool s_mdns_initialized = false;
static bool s_mdns_sta_registered = false;

static struct netif *find_netif_by_name(const char *name)
{
    struct netif *n;
    NETIF_FOREACH(n) {
        if (strncmp(n->name, name, 2) == 0) return n;
    }
    return NULL;
}

static void mdns_register_sta(void)
{
    if (!s_mdns_initialized) {
        mdns_resp_init();
        s_mdns_initialized = true;
    }
    struct netif *netif = find_netif_by_name("st");
    if (netif && !s_mdns_sta_registered) {
        err_t ret = mdns_resp_add_netif(netif, MDNS_HOSTNAME);
        if (ret == ERR_OK) {
            s_mdns_sta_registered = true;
            // ESP_LOGI(TAG, "mDNS: STA registered as %s.local", MDNS_HOSTNAME);
        } else {
            // ESP_LOGI(TAG, "mDNS: STA register failed (%d)", ret);
        }
    }
}

static void mdns_register_ap(void)
{
    if (!s_mdns_initialized) {
        mdns_resp_init();
        s_mdns_initialized = true;
    }
    struct netif *netif = find_netif_by_name("ap");
    if (netif) {
        err_t ret = mdns_resp_add_netif(netif, MDNS_HOSTNAME);
        if (ret == ERR_OK) {
            // ESP_LOGI(TAG, "mDNS: AP registered as %s.local", MDNS_HOSTNAME);
        } else {
            // ESP_LOGI(TAG, "mDNS: AP register failed (%d)", ret);
        }
    }
}

static void mdns_unregister_sta(void)
{
    struct netif *netif = find_netif_by_name("st");
    if (netif && s_mdns_sta_registered) {
        mdns_resp_remove_netif(netif);
        s_mdns_sta_registered = false;
        // ESP_LOGI(TAG, "mDNS: STA unregistered");
    }
}

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define AP_DEFAULT_SSID  "DEV-MGT_26001"

#define MAX_RETRY      3
#define CONNECT_TIMEOUT_SEC  15

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static bool s_wifi_connected = false;
static char s_ip_str[16] = {0};
static int s_rssi = 0;
static bool s_ap_active = false;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;
static bool s_sta_connect_allowed = true;

static void stop_ap_task(void *arg);

bool wifi_is_connected(void) { return s_wifi_connected; }
const char *wifi_get_ip(void) { return s_ip_str; }
int wifi_get_rssi(void) { return s_rssi; }
EventGroupHandle_t wifi_get_event_group(void) { return s_wifi_event_group; }
bool wifi_is_ap_active(void) { return s_ap_active; }

static const char *auth_mode_str(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:        return "OPEN";
        case WIFI_AUTH_WEP:         return "WEP";
        case WIFI_AUTH_WPA_PSK:     return "WPA";
        case WIFI_AUTH_WPA2_PSK:    return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:    return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:return "WPA2/WPA3";
        default:                    return "UNKNOWN";
    }
}

static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:        return "Auth expire";
        case WIFI_REASON_AUTH_LEAVE:         return "Auth leave";
        case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: return "Assoc expire";
        case WIFI_REASON_ASSOC_TOOMANY:      return "Assoc too many";
        case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA: return "Not authed";
        case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA: return "Not associated";
        case WIFI_REASON_ASSOC_LEAVE:        return "Assoc leave";
        case WIFI_REASON_ASSOC_NOT_AUTHED:   return "Assoc not authed";
        case WIFI_REASON_UNSPECIFIED:        return "Unspecified";
        case WIFI_REASON_BEACON_TIMEOUT:     return "Beacon timeout";
        case WIFI_REASON_NO_AP_FOUND:        return "AP not found";
        case WIFI_REASON_AUTH_FAIL:          return "Auth FAIL";
        case WIFI_REASON_CONNECTION_FAIL:    return "Connection fail";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "Handshake timeout";
        default:                             return "Unknown";
    }
}

bool wifi_cred_load(wifi_cred_t *cred)
{
    if (!cred) return false;
    memset(cred, 0, sizeof(*cred));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // ESP_LOGI(TAG, "No WiFi config in NVS (nvs_open: %s)", esp_err_to_name(err));
        return false;
    }

    size_t ssid_len = sizeof(cred->ssid);
    size_t pass_len = sizeof(cred->password);
    err = nvs_get_str(handle, NVS_KEY_SSID, cred->ssid, &ssid_len);
    if (err != ESP_OK) {
        // ESP_LOGI(TAG, "No SSID saved (err: %s)", esp_err_to_name(err));
        nvs_close(handle);
        return false;
    }
    nvs_get_str(handle, NVS_KEY_PASS, cred->password, &pass_len);
    nvs_close(handle);

    if (strlen(cred->ssid) == 0) return false;
    // ESP_LOGI(TAG, "Loaded WiFi config: SSID=%s", cred->ssid);
    return true;
}

bool wifi_cred_save(const wifi_cred_t *cred)
{
    if (!cred || strlen(cred->ssid) == 0) return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        // ESP_LOGI(TAG, "nvs_open write failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, cred->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, cred->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        // ESP_LOGI(TAG, "WiFi config saved to NVS");
        return true;
    }
    // ESP_LOGI(TAG, "Failed to save WiFi config: %s", esp_err_to_name(err));
    return false;
}

void wifi_cred_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return;
    nvs_erase_key(handle, NVS_KEY_SSID);
    nvs_erase_key(handle, NVS_KEY_PASS);
    nvs_commit(handle);
    nvs_close(handle);
    // ESP_LOGI(TAG, "WiFi config cleared from NVS");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_connect_allowed) {
            // ESP_LOGI(TAG, "WiFi STA started, connecting...");
            esp_wifi_connect();
        } else {
            // ESP_LOGI(TAG, "WiFi STA started (AP mode), auto-connect disabled");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;
        led_notify_wifi_sta(false);
        memset(s_ip_str, 0, sizeof(s_ip_str));
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        mdns_unregister_sta();
        // ESP_LOGI(TAG, "Disconnected! reason=%d (%s), rssi=%d", disconn->reason, disconnect_reason_str(disconn->reason), disconn->rssi);
        if (s_sta_connect_allowed && s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            // ESP_LOGI(TAG, "Retry (%d/%d)...", s_retry_count, MAX_RETRY);
        } else if (!s_sta_connect_allowed) {
            // ESP_LOGI(TAG, "Auto-connect disabled (AP mode), skip retry");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            // ESP_LOGI(TAG, "Max retries reached");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        led_notify_wifi_sta(true);
        s_retry_count = 0;
        mdns_register_sta();
        // ESP_LOGI(TAG, "========================================");
        // ESP_LOGI(TAG, "   WiFi CONNECTED");
        // ESP_LOGI(TAG, "   IP   : %s", s_ip_str);
        // ESP_LOGI(TAG, "   URL  : http://%s.local", MDNS_HOSTNAME);
        // ESP_LOGI(TAG, "========================================");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
    }
}

void wifi_init_sta(void)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, &instance_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_sta_connect_allowed = true;

    wifi_cred_t cred;
    if (wifi_cred_load(&cred)) {
        wifi_config_t wifi_config = {0};
        memcpy(wifi_config.sta.ssid, cred.ssid, sizeof(wifi_config.sta.ssid) - 1);
        memcpy(wifi_config.sta.password, cred.password, sizeof(wifi_config.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(CONNECT_TIMEOUT_SEC * 1000));

        if (bits & WIFI_CONNECTED_BIT) {
            // ESP_LOGI(TAG, "Auto-connect succeeded");
            return;
        } else {
            // ESP_LOGI(TAG, "Auto-connect failed, starting AP for provisioning...");
            esp_wifi_stop();
        }
    } else {
        // ESP_LOGI(TAG, "No saved credentials, starting AP for provisioning...");
    }

    wifi_start_ap(AP_DEFAULT_SSID);
}

esp_err_t wifi_try_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        // ESP_LOGI(TAG, "SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password ? password : "", sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_sta_connect_allowed = true;
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_mode_t cur_mode;
    esp_wifi_get_mode(&cur_mode);

    if (cur_mode == WIFI_MODE_AP) {
        // ESP_LOGI(TAG, "Switching AP -> APSTA (keep AP, add STA)...");
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(100));

        if (!s_netif_ap) {
            s_netif_ap = esp_netif_create_default_wifi_ap();
            esp_netif_ip_info_t ap_ip_info = {0};
            ap_ip_info.ip.addr = htonl(0xC0A80401);
            ap_ip_info.gw.addr = htonl(0xC0A80401);
            ap_ip_info.netmask.addr = htonl(0xFFFFFF00);
            esp_netif_set_ip_info(s_netif_ap, &ap_ip_info);
        }
        if (!s_netif_sta) {
            s_netif_sta = esp_netif_create_default_wifi_sta();
        }

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

        wifi_config_t ap_config = {0};
        strncpy((char *)ap_config.ap.ssid, AP_DEFAULT_SSID, sizeof(ap_config.ap.ssid) - 1);
        ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
        ap_config.ap.channel = 1;
        ap_config.ap.max_connection = 4;
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        ESP_ERROR_CHECK(esp_wifi_start());
    } else if (cur_mode == WIFI_MODE_APSTA) {
        // ESP_LOGI(TAG, "APSTA mode, just reconnect STA...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_wifi_connect();
    } else {
        // ESP_LOGI(TAG, "STA mode running, reconnecting with new credentials...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_wifi_connect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(CONNECT_TIMEOUT_SEC * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_cred_t cred;
        strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
        if (password) strncpy(cred.password, password, sizeof(cred.password) - 1);
        wifi_cred_save(&cred);
        // ESP_LOGI(TAG, "Connected to %s, credentials saved. Switching to STA-only...", ssid);
        xTaskCreate(stop_ap_task, "stop_ap", 1024, NULL, 4, NULL);
        return ESP_OK;
    }

    // ESP_LOGI(TAG, "Failed to connect to %s", ssid);
    return ESP_FAIL;
}

static wifi_cred_t s_pending_cred;
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_timer_cb(void *arg)
{
    wifi_try_connect(s_pending_cred.ssid, s_pending_cred.password);
}

void wifi_request_connect(const char *ssid, const char *password)
{
    snprintf(s_pending_cred.ssid, sizeof(s_pending_cred.ssid), "%s", ssid);
    snprintf(s_pending_cred.password, sizeof(s_pending_cred.password), "%s", password ? password : "");

    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    esp_timer_create_args_t args = {
        .callback = reconnect_timer_cb,
        .name = "reconnect",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&args, &s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, 1500000);
}

esp_err_t wifi_scan_aps(wifi_ap_info_t *out, uint16_t *count)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;

    wifi_mode_t cur_mode;
    esp_wifi_get_mode(&cur_mode);

    wifi_scan_config_t scan_cfg = { .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false };

    if (cur_mode != WIFI_MODE_APSTA && cur_mode != WIFI_MODE_STA) {
        // ESP_LOGI(TAG, "Scan: wrong mode %d, need APSTA or STA", cur_mode);
        return ESP_ERR_INVALID_STATE;
    }

    // ESP_LOGI(TAG, "Scan: starting (mode=%d)...", cur_mode);
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        // ESP_LOGI(TAG, "Scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    // ESP_LOGI(TAG, "Scan found %d APs", ap_count);

    uint16_t max = (ap_count > WIFI_MAX_AP_COUNT) ? WIFI_MAX_AP_COUNT : ap_count;
    wifi_ap_record_t *ap_list = heap_caps_malloc(sizeof(wifi_ap_record_t) * max, MALLOC_CAP_8BIT);
    if (!ap_list) {
        // ESP_LOGI(TAG, "Scan: no heap for ap_list (%d bytes)", (int)(sizeof(wifi_ap_record_t) * max));
        return ESP_ERR_NO_MEM;
    }
    esp_wifi_scan_get_ap_records(&max, ap_list);

    for (uint16_t i = 0; i < max; i++) {
        strncpy(out[i].ssid, (const char *)ap_list[i].ssid, WIFI_MAX_SSID_LEN - 1);
        out[i].rssi = ap_list[i].rssi;
        out[i].auth = ap_list[i].authmode;
        out[i].channel = ap_list[i].primary;
        // ESP_LOGI(TAG, "  [%2d] SSID: %-32s RSSI: %4d Auth: %s Ch: %d", i + 1, out[i].ssid, out[i].rssi, auth_mode_str(out[i].auth), out[i].channel);
    }

    free(ap_list);
    *count = max;
    return ESP_OK;
}

static void stop_ap_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (!s_wifi_connected) {
        vTaskDelete(NULL);
        return;
    }

    // ESP_LOGI(TAG, "Switching APSTA -> STA only");

    dns_server_stop();

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        // ESP_LOGI(TAG, "Set STA mode failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    s_ap_active = false;
    led_notify_wifi_ap(false);
    // ESP_LOGI(TAG, "AP stopped, now STA-only. IP: %s", s_ip_str);
    vTaskDelete(NULL);
}

void wifi_start_ap(const char *ap_ssid)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    s_sta_connect_allowed = false;
    s_wifi_connected = false;
    memset(s_ip_str, 0, sizeof(s_ip_str));
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    led_notify_wifi_sta(false);

    if (!s_netif_ap) {
        s_netif_ap = esp_netif_create_default_wifi_ap();

        esp_netif_ip_info_t ap_ip_info = {0};
        ap_ip_info.ip.addr = htonl(0xC0A80401);
        ap_ip_info.gw.addr = htonl(0xC0A80401);
        ap_ip_info.netmask.addr = htonl(0xFFFFFF00);
        esp_netif_set_ip_info(s_netif_ap, &ap_ip_info);
    }

    {
        esp_netif_dhcps_stop(s_netif_ap);
        uint32_t dns_ip = htonl(0xC0A80401);
        esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dns_ip, sizeof(dns_ip));
        esp_netif_dhcps_start(s_netif_ap);
    }

    // ESP_LOGI(TAG, "Stopping WiFi before APSTA switch...");
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t sta_clear = {0};
    esp_wifi_set_config(WIFI_IF_STA, &sta_clear);

    wifi_config_t ap_config = {0};
    const char *ssid = ap_ssid ? ap_ssid : AP_DEFAULT_SSID;
    strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    s_ap_active = true;
    s_retry_count = MAX_RETRY;
    led_notify_wifi_ap(true);

    vTaskDelay(pdMS_TO_TICKS(300));
    mdns_register_ap();

    dns_server_start();

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_netif_ap, &ip_info) == ESP_OK) {
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip_info.ip));
        // ESP_LOGI(TAG, "========================================");
        // ESP_LOGI(TAG, "   AP Mode Active (OPEN)");
        // ESP_LOGI(TAG, "   SSID : %s", ssid);
        // ESP_LOGI(TAG, "   IP   : %s", s_ip_str);
        // ESP_LOGI(TAG, "   URL  : http://%s.local", MDNS_HOSTNAME);
        // ESP_LOGI(TAG, "========================================");
    } else {
        memset(s_ip_str, 0, sizeof(s_ip_str));
    }
}

void wifi_update_rssi(void)
{
    if (s_wifi_connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_rssi = ap_info.rssi;
        }
    }
}