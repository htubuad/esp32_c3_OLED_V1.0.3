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
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>

#include "lwip/netif.h"
#include "lwip/apps/mdns.h"

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "pass"

#define WIFI_TASK_STACK_SIZE   4096
#define WIFI_TASK_PRIORITY     5

#define CMD_CONNECT_NEW  1
#define CMD_CLEAR_AND_AP 2
#define CMD_CLOSE_AP     3
#define CMD_OPEN_AP      4

#define CONNECTED_BIT  BIT0
#define FAIL_BIT       BIT1

static const char *TAG = "WIFI";

static wifi_state_t s_state = WIFI_STATE_IDLE;
static QueueHandle_t s_cmd_queue = NULL;
static EventGroupHandle_t s_event_group = NULL;

static bool s_wifi_connected = false;
static bool s_ap_active = false;
static char s_sta_ip_str[16] = {0};
static char s_ap_ip_str[16] = {0};

#define STATUS_TEXT_MAX 64
static char s_status_text[STATUS_TEXT_MAX] = "初始化中...";
static int s_rssi = 0;

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;
static int s_retry_count = 0;
static bool s_sta_connect_allowed = false;

static bool s_mdns_initialized = false;
static bool s_mdns_sta_registered = false;
static bool s_mdns_ap_registered = false;

// 防重入标记
static bool s_wifi_inited = false;
static bool s_netif_created = false;
static bool s_event_registered = false;
static bool s_wifi_started = false;

#define MDNS_HOSTNAME "esp32c3"
#define MAX_RETRY      3
#define CONNECT_TIMEOUT_SEC  15
#define WIFI_QUEUE_LEN  8

static void start_rssi_timer(void);
static void stop_rssi_timer(void);

typedef struct {
    uint8_t cmd;
    wifi_cred_t cred;
} wifi_msg_t;

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
    if (!s_mdns_initialized) { mdns_resp_init(); s_mdns_initialized = true; }
    struct netif *netif = find_netif_by_name("st");
    if (netif && !s_mdns_sta_registered) {
        if (mdns_resp_add_netif(netif, MDNS_HOSTNAME) == ERR_OK) {
            s_mdns_sta_registered = true;
        }
    }
}

static void mdns_register_ap(void)
{
    if (!s_mdns_initialized) { mdns_resp_init(); s_mdns_initialized = true; }
    struct netif *netif = find_netif_by_name("ap");
    if (netif && !s_mdns_ap_registered) {
        if (mdns_resp_add_netif(netif, MDNS_HOSTNAME) == ERR_OK) {
            s_mdns_ap_registered = true;
        }
    }
}

static void mdns_unregister_sta(void)
{
    struct netif *netif = find_netif_by_name("st");
    if (netif && s_mdns_sta_registered) {
        mdns_resp_remove_netif(netif);
        s_mdns_sta_registered = false;
    }
}

static void mdns_unregister_ap(void)
{
    struct netif *netif = find_netif_by_name("ap");
    if (netif && s_mdns_ap_registered) {
        mdns_resp_remove_netif(netif);
        s_mdns_ap_registered = false;
    }
}

static bool queue_cmd(uint8_t cmd, const wifi_cred_t *cred)
{
    wifi_msg_t msg = {0};
    msg.cmd = cmd;
    if (cred) memcpy(&msg.cred, cred, sizeof(msg.cred));
    return xQueueSend(s_cmd_queue, &msg, 0) == pdPASS;
}

static esp_err_t do_open_ap(void);

static void set_status(const char *text)
{
    if (!text) return;
    strncpy(s_status_text, text, STATUS_TEXT_MAX - 1);
    s_status_text[STATUS_TEXT_MAX - 1] = '\0';
}

static void set_status_disconnected(uint8_t reason)
{
    switch (reason) {
    case 2:  set_status("认证过期"); break;
    case 3:
    case 4:  set_status("密码错误"); break;
    case 5:  set_status("AP已禁用"); break;
    case 6:  set_status("密码无效"); break;
    case 7:  set_status("握手超时"); break;
    case 8:  set_status("连接被拒绝"); break;
    case 201: set_status("未找到路由器"); break;
    case 202: set_status("连接超时"); break;
    case 204: set_status("握手超时"); break;
    case 205: set_status("路由器已断开"); break;
    default: {
        char buf[STATUS_TEXT_MAX];
        snprintf(buf, sizeof(buf), "连接失败 (reason=%d)", reason);
        set_status(buf);
        break;
    }
    }
}

bool wifi_is_connected(void) { return s_wifi_connected; }
bool wifi_is_ap_active(void) { return s_ap_active; }
int wifi_get_rssi(void) { return s_rssi; }
const char *wifi_get_status_text(void) { return s_status_text; }
const char *wifi_get_sta_ip(void) { return s_sta_ip_str; }
const char *wifi_get_ap_ip(void) { return s_ap_ip_str; }
const char *wifi_get_ip(void) { return s_sta_ip_str[0] ? s_sta_ip_str : s_ap_ip_str; }

bool wifi_cred_load(wifi_cred_t *cred)
{
    if (!cred) return false;
    memset(cred, 0, sizeof(*cred));
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t ssid_len = sizeof(cred->ssid);
    size_t pass_len = sizeof(cred->password);
    esp_err_t err = nvs_get_str(handle, NVS_KEY_SSID, cred->ssid, &ssid_len);
    if (err == ESP_OK) nvs_get_str(handle, NVS_KEY_PASS, cred->password, &pass_len);
    nvs_close(handle);
    return (err == ESP_OK && strlen(cred->ssid) > 0);
}

bool wifi_cred_save(const wifi_cred_t *cred)
{
    if (!cred || strlen(cred->ssid) == 0) return false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, NVS_KEY_SSID, cred->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, NVS_KEY_PASS, cred->password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

void wifi_cred_clear(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_erase_key(handle, NVS_KEY_SSID);
    nvs_erase_key(handle, NVS_KEY_PASS);
    nvs_commit(handle);
    nvs_close(handle);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_connect_allowed) {
            set_status("正在连接路由器...");
            s_retry_count = 0;
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = event_data;
        s_wifi_connected = false;
        led_notify_wifi_sta(false);
        memset(s_sta_ip_str, 0, sizeof(s_sta_ip_str));
        xEventGroupClearBits(s_event_group, CONNECTED_BIT);
        mdns_unregister_sta();

        if (s_sta_connect_allowed && s_retry_count < MAX_RETRY) {
            set_status_disconnected(disconn->reason);
            esp_wifi_connect();
            s_retry_count++;
        } else if (s_sta_connect_allowed) {
            set_status_disconnected(disconn->reason);
            xEventGroupSetBits(s_event_group, FAIL_BIT);
        } else {
            set_status("WiFi 已断开");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_sta_ip_str, sizeof(s_sta_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        led_notify_wifi_sta(true);
        s_retry_count = 0;
        mdns_register_sta();
        start_rssi_timer();
        xEventGroupSetBits(s_event_group, CONNECTED_BIT);

        wifi_ap_record_t ap_info = {0};
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) rssi = ap_info.rssi;

        char buf[STATUS_TEXT_MAX + 32];
        snprintf(buf, sizeof(buf), "已连接 %s (IP: %s)", ap_info.ssid, s_sta_ip_str);
        set_status(buf);

        ESP_LOGI(TAG, "========================");
        ESP_LOGI(TAG, " WiFi Connected!");
        ESP_LOGI(TAG, " SSID: %s", ap_info.ssid);
        ESP_LOGI(TAG, " IP:   %s", s_sta_ip_str);
        ESP_LOGI(TAG, " RSSI: %d dBm", rssi);
        ESP_LOGI(TAG, "========================");
    }
}

static esp_timer_handle_t s_rssi_timer = NULL;

static void rssi_timer_cb(void *arg)
{
    wifi_update_rssi();
    esp_timer_start_once(s_rssi_timer, 5000000);
}

static void start_rssi_timer(void)
{
    if (!s_rssi_timer) {
        esp_timer_create_args_t args = {
            .callback = rssi_timer_cb, .name = "rssi_update",
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_timer_create(&args, &s_rssi_timer);
    }
    wifi_update_rssi();
    esp_timer_start_once(s_rssi_timer, 5000000);
}

static void stop_rssi_timer(void)
{
    if (s_rssi_timer) esp_timer_stop(s_rssi_timer);
}

// ===== 安全幂等初始化，绝不 panic =====
static esp_err_t do_wifi_init(void)
{
    if (!s_event_group) s_event_group = xEventGroupCreate();

    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NO_MEM) return err;

    if (!s_netif_created) {
        s_netif_sta = esp_netif_create_default_wifi_sta();
        s_netif_ap = esp_netif_create_default_wifi_ap();
        if (!s_netif_sta || !s_netif_ap) return ESP_ERR_NO_MEM;

        {
            esp_netif_ip_info_t info = {0};
            info.ip.addr = htonl(0xC0A80401);
            info.gw.addr = htonl(0xC0A80401);
            info.netmask.addr = htonl(0xFFFFFF00);
            esp_netif_set_ip_info(s_netif_ap, &info);
        }
        s_netif_created = true;
    }

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) return err;
        s_wifi_inited = true;
    }

    if (!s_event_registered) {
        esp_event_handler_instance_t inst1, inst2;
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        &wifi_event_handler, NULL, &inst1);
        if (err == ESP_OK) {
            err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                            &wifi_event_handler, NULL, &inst2);
        }
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_event_registered = true;
    }

    return ESP_OK;
}

static esp_err_t do_wifi_start_once(const char *sta_ssid, const char *sta_pass)
{
    esp_err_t err;

    if (!s_wifi_inited) {
        err = do_wifi_init();
        if (err != ESP_OK) return err;
    }

    wifi_config_t ap_config = {0};
    const char *ap_ssid = WIFI_AP_DEFAULT_SSID;
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    wifi_config_t sta_config = {0};
    if (sta_ssid && strlen(sta_ssid) > 0) {
        strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid) - 1);
        if (sta_pass) strncpy((char *)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        sta_config.sta.pmf_cfg.capable = true;
        sta_config.sta.pmf_cfg.required = false;
        s_sta_connect_allowed = true;
        s_retry_count = 0;
    } else {
        s_sta_connect_allowed = false;
        s_retry_count = MAX_RETRY;
    }
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    esp_wifi_set_protocol(WIFI_IF_AP,
            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_protocol(WIFI_IF_STA,
            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_wifi_started = true;
    }

    {
        uint32_t dns_ip = htonl(0xC0A80401);
        esp_netif_dhcps_stop(s_netif_ap);
        esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET,
                ESP_NETIF_DOMAIN_NAME_SERVER, &dns_ip, sizeof(dns_ip));
        esp_netif_dhcps_start(s_netif_ap);
    }

    s_ap_active = true;
    led_notify_wifi_ap(true);

    vTaskDelay(pdMS_TO_TICKS(300));
    mdns_register_ap();
    dns_server_start();
    snprintf(s_ap_ip_str, sizeof(s_ap_ip_str), WIFI_AP_IP);

    if (!s_sta_connect_allowed) {
        set_status("AP配网模式，等待手机连接...");
    }

    ESP_LOGI(TAG, "========================");
    ESP_LOGI(TAG, " WiFi Init Done");
    ESP_LOGI(TAG, " SSID: %s", ap_ssid);
    ESP_LOGI(TAG, " AP:   %s", s_ap_ip_str);
    ESP_LOGI(TAG, "========================");

    return ESP_OK;
}

// ===== 运行时热切换凭证 (核心: disconnect → set_config → connect, 不 stop) =====
static esp_err_t do_runtime_switch(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    stop_rssi_timer();
    xEventGroupClearBits(s_event_group, CONNECTED_BIT | FAIL_BIT);
    memset(s_sta_ip_str, 0, sizeof(s_sta_ip_str));
    mdns_unregister_sta();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password ? password : "", sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // 热切换三步曲: 断 → 改 → 连 (AP 全程不动, web_server 不掉线)
    s_sta_connect_allowed = true;
    s_retry_count = 0;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) return err;

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
            CONNECTED_BIT | FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(CONNECT_TIMEOUT_SEC * 1000));

    if (bits & CONNECTED_BIT) return ESP_OK;
    return ESP_FAIL;
}

// 运行时放弃 STA 连接，保持 AP-only (用于密码错误回退配网)
static void do_runtime_standalone_ap(void)
{
    stop_rssi_timer();
    s_sta_connect_allowed = false;
    s_wifi_connected = false;
    led_notify_wifi_sta(false);
    memset(s_sta_ip_str, 0, sizeof(s_sta_ip_str));
    xEventGroupClearBits(s_event_group, CONNECTED_BIT | FAIL_BIT);
    mdns_unregister_sta();

    wifi_config_t empty = {0};
    esp_wifi_set_config(WIFI_IF_STA, &empty);
    esp_wifi_disconnect();

    if (!s_ap_active) {
        do_open_ap();
    }

    set_status("AP配网模式，等待手机连接...");
}

static esp_err_t do_close_ap(void)
{
    if (!s_ap_active) return ESP_OK;

    dns_server_stop();
    mdns_unregister_ap();

    esp_netif_dhcps_stop(s_netif_ap);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set WIFI_MODE_STA failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_active = false;
    led_notify_wifi_ap(false);
    memset(s_ap_ip_str, 0, sizeof(s_ap_ip_str));

    set_status("STA-only 模式");

    return ESP_OK;
}

static esp_err_t do_open_ap(void)
{
    if (s_ap_active) return ESP_OK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set WIFI_MODE_APSTA failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap_config = {0};
    const char *ap_ssid = WIFI_AP_DEFAULT_SSID;
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    esp_wifi_set_protocol(WIFI_IF_AP,
            WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

    {
        uint32_t dns_ip = htonl(0xC0A80401);
        esp_netif_dhcps_start(s_netif_ap);
        esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET,
                ESP_NETIF_DOMAIN_NAME_SERVER, &dns_ip, sizeof(dns_ip));
    }

    s_ap_active = true;
    led_notify_wifi_ap(true);
    snprintf(s_ap_ip_str, sizeof(s_ap_ip_str), WIFI_AP_IP);

    vTaskDelay(pdMS_TO_TICKS(200));
    mdns_register_ap();
    dns_server_start();

    set_status("AP已开启");

    ESP_LOGI(TAG, "AP re-opened, SSID=%s IP=%s", ap_ssid, s_ap_ip_str);
    return ESP_OK;
}

static void fsm_task(void *arg)
{
    wifi_msg_t msg;

    esp_err_t err = do_wifi_init();
    if (err != ESP_OK) {
        // init 彻底失败，死循环等待命令（不会重启）
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    wifi_cred_t cred;
    if (wifi_cred_load(&cred)) {
        s_state = WIFI_STATE_STA_CONNECT;
        do_wifi_start_once(cred.ssid, cred.password);

        EventBits_t bits = xEventGroupWaitBits(s_event_group,
                CONNECTED_BIT | FAIL_BIT, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(CONNECT_TIMEOUT_SEC * 1000));

        if (bits & CONNECTED_BIT) {
            s_state = WIFI_STATE_STA_CONNECTED;
        } else {
            s_state = WIFI_STATE_AP;
            do_runtime_standalone_ap();
        }
    } else {
        s_state = WIFI_STATE_AP;
        do_wifi_start_once(NULL, NULL);
    }

    while (1) {
        if (xQueueReceive(s_cmd_queue, &msg, pdMS_TO_TICKS(3000)) == pdPASS) {
            switch (msg.cmd) {
                case CMD_CONNECT_NEW: {
                    esp_err_t ret = do_runtime_switch(msg.cred.ssid, msg.cred.password);

                    if (ret == ESP_OK) {
                        wifi_cred_save(&msg.cred);
                        s_state = WIFI_STATE_STA_CONNECTED;
                    } else {
                        wifi_cred_save(&msg.cred);
                        s_state = WIFI_STATE_AP;
                        do_runtime_standalone_ap();
                    }
                    break;
                }

                case CMD_CLEAR_AND_AP:
                    wifi_cred_clear();
                    s_state = WIFI_STATE_AP;
                    do_runtime_standalone_ap();
                    break;

                case CMD_CLOSE_AP:
                    if (s_state == WIFI_STATE_STA_CONNECTED) {
                        do_close_ap();
                        s_state = WIFI_STATE_STA_ONLY;
                    } else if (s_state == WIFI_STATE_STA_ONLY) {
                        do_close_ap();
                    }
                    break;

                case CMD_OPEN_AP:
                    if (s_state == WIFI_STATE_STA_ONLY) {
                        do_open_ap();
                        s_state = WIFI_STATE_STA_CONNECTED;
                    }
                    break;

                default:
                    break;
            }
        } else {
            EventBits_t bits = xEventGroupGetBits(s_event_group);
            if ((bits & FAIL_BIT) && s_sta_connect_allowed) {
                s_sta_connect_allowed = false;
                if (s_state == WIFI_STATE_STA_CONNECTED || s_state == WIFI_STATE_STA_CONNECT) {
                    s_state = WIFI_STATE_AP;
                    do_runtime_standalone_ap();
                } else if (s_state == WIFI_STATE_STA_ONLY) {
                    ESP_LOGW(TAG, "STA disconnected in STA_ONLY, re-opening AP");
                    do_open_ap();
                    s_state = WIFI_STATE_AP;
                }
            }
        }
    }
}

void wifi_manager_start(void)
{
    if (s_cmd_queue) return;
    s_cmd_queue = xQueueCreate(WIFI_QUEUE_LEN, sizeof(wifi_msg_t));
    xTaskCreatePinnedToCore(fsm_task, "wifi_fsm", WIFI_TASK_STACK_SIZE,
                            NULL, WIFI_TASK_PRIORITY, NULL, 0);
}

void wifi_manager_request_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0 || !s_cmd_queue) return;
    wifi_cred_t cred = {0};
    strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
    if (password) strncpy(cred.password, password, sizeof(cred.password) - 1);
    queue_cmd(CMD_CONNECT_NEW, &cred);
}

void wifi_manager_request_clear_and_ap(void)
{
    if (!s_cmd_queue) return;
    queue_cmd(CMD_CLEAR_AND_AP, NULL);
}

void wifi_manager_request_close_ap(void)
{
    if (!s_cmd_queue) return;
    queue_cmd(CMD_CLOSE_AP, NULL);
}

void wifi_manager_request_open_ap(void)
{
    if (!s_cmd_queue) return;
    queue_cmd(CMD_OPEN_AP, NULL);
}

esp_err_t wifi_scan_aps(wifi_ap_info_t *out, uint16_t *count)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;

    wifi_mode_t cur_mode;
    esp_wifi_get_mode(&cur_mode);
    if (cur_mode != WIFI_MODE_APSTA && cur_mode != WIFI_MODE_STA) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) return err;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    uint16_t max = (ap_count > WIFI_MAX_AP_COUNT) ? WIFI_MAX_AP_COUNT : ap_count;
    wifi_ap_record_t *ap_list = heap_caps_malloc(sizeof(wifi_ap_record_t) * max, MALLOC_CAP_8BIT);
    if (!ap_list) return ESP_ERR_NO_MEM;

    esp_wifi_scan_get_ap_records(&max, ap_list);
    for (uint16_t i = 0; i < max; i++) {
        strncpy(out[i].ssid, (const char *)ap_list[i].ssid, WIFI_MAX_SSID_LEN - 1);
        out[i].rssi = ap_list[i].rssi;
        out[i].auth = ap_list[i].authmode;
        out[i].channel = ap_list[i].primary;
    }
    free(ap_list);
    *count = max;
    return ESP_OK;
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