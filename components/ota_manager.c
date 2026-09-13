#include "ota_manager.h"
#include "mqtt_aliyun.h"
#include "led.h"
#include "wifi_manager.h"
#include "version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "mbedtls/md.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "OTA";

#define OTA_BUFFER_SIZE                 4096
#define OTA_URL_MAX_LEN                 256
#define OTA_VERSION_MAX_LEN             32
#define OTA_SIGN_MAX_LEN                128
#define OTA_REBOOT_DELAY_MS             3000
#define OTA_HTTP_TIMEOUT_MS             30000
#define OTA_PROGRESS_STEP               5
#define OTA_HTTP_RETRY_MAX              3
#define OTA_HTTP_RETRY_INTERVAL_MS      1500
#define OTA_READ_RETRY_MAX              2
#define OTA_READ_RETRY_INTERVAL_MS      200
#define OTA_TASK_STACK_SIZE             12288
#define OTA_TASK_PRIORITY               5

#define ALIYUN_OTA_TOPIC_UPGRADE_REPLY "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/ota/upgrade_reply"
#define ALIYUN_OTA_TOPIC_POST          "/sys/" ALIYUN_PRODUCT_KEY "/" ALIYUN_DEVICE_NAME "/ota/post"

typedef struct {
    char fw_id[64];
    char fw_url[OTA_URL_MAX_LEN];
    char fw_version[OTA_VERSION_MAX_LEN];
    char fw_sign[OTA_SIGN_MAX_LEN];
    char sign_method[16];
    bool force_upgrade;
    char msg_id[32];
} ota_notify_t;

static ota_state_t  s_state = OTA_STATE_IDLE;
static int          s_progress = 0;
static char         s_target_version[OTA_VERSION_MAX_LEN] = {0};
static char         s_error_reason[128] = {0};
static SemaphoreHandle_t s_ota_sem = NULL;
static TaskHandle_t  s_ota_task_handle = NULL;
static volatile bool  s_ota_in_progress = false;
static volatile bool  s_cancel_requested = false;
static bool          s_md5_ctx_ready = false;
static mbedtls_md_context_t s_md5_ctx;
static bool          s_initialized = false;

static void set_state(ota_state_t st, const char *err)
{
    s_state = st;
    if (err) {
        strncpy(s_error_reason, err, sizeof(s_error_reason) - 1);
        s_error_reason[sizeof(s_error_reason) - 1] = '\0';
    }
}

static void md5_cleanup(void)
{
    if (s_md5_ctx_ready) {
        mbedtls_md_free(&s_md5_ctx);
        s_md5_ctx_ready = false;
    }
}

static void report_progress(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == s_progress && percent != 0) return;
    s_progress = percent;

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"id\":\"ota\",\"version\":\"1.0\",\"params\":{\"step\":\"2\",\"desc\":\"%d\"}}",
        percent);
    mqtt_publish_custom(ALIYUN_OTA_TOPIC_POST, payload, 0);
    ESP_LOGI(TAG, "Progress: %d%%", percent);
}

static void report_result(const char *step, const char *desc)
{
    char payload[300];
    snprintf(payload, sizeof(payload),
        "{\"id\":\"ota\",\"version\":\"1.0\",\"params\":{\"step\":\"%s\",\"desc\":\"%s\"}}",
        step, desc ? desc : "");
    mqtt_publish_custom(ALIYUN_OTA_TOPIC_POST, payload, 0);
}

static bool parse_notify(const char *json, ota_notify_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    out->fw_id[0] = '\0';
    out->fw_url[0] = '\0';
    out->fw_version[0] = '\0';
    out->fw_sign[0] = '\0';
    out->sign_method[0] = '\0';
    out->force_upgrade = false;
    out->msg_id[0] = '\0';

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (id && id->valuestring) {
        strncpy(out->msg_id, id->valuestring, sizeof(out->msg_id) - 1);
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!params) { cJSON_Delete(root); return false; }

    cJSON *item = cJSON_GetObjectItem(params, "fwId");
    if (item && item->valuestring)
        strncpy(out->fw_id, item->valuestring, sizeof(out->fw_id) - 1);

    item = cJSON_GetObjectItem(params, "fwUrl");
    if (item && item->valuestring)
        strncpy(out->fw_url, item->valuestring, sizeof(out->fw_url) - 1);

    item = cJSON_GetObjectItem(params, "fwVersion");
    if (item && item->valuestring)
        strncpy(out->fw_version, item->valuestring, sizeof(out->fw_version) - 1);

    item = cJSON_GetObjectItem(params, "fwSign");
    if (item && item->valuestring)
        strncpy(out->fw_sign, item->valuestring, sizeof(out->fw_sign) - 1);

    item = cJSON_GetObjectItem(params, "signMethod");
    if (item && item->valuestring)
        strncpy(out->sign_method, item->valuestring, sizeof(out->sign_method) - 1);

    item = cJSON_GetObjectItem(params, "forceUpgrade");
    if (item) out->force_upgrade = item->valueint ? true : false;

    cJSON_Delete(root);

    return out->fw_url[0] != '\0';
}

static void reply_upgrade(const char *msg_id, int code, const char *desc)
{
    char payload[300];
    snprintf(payload, sizeof(payload),
        "{\"id\":\"%s\",\"code\":%d,\"desc\":\"%s\",\"version\":\"1.0\"}",
        msg_id ? msg_id : "ota", code, desc ? desc : "");
    mqtt_publish_custom(ALIYUN_OTA_TOPIC_UPGRADE_REPLY, payload, 0);
    ESP_LOGI(TAG, "Upgrade reply: code=%d %s", code, desc ? desc : "");
}

static esp_err_t md5_init_ctx(void)
{
    md5_cleanup();
    mbedtls_md_init(&s_md5_ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info) {
        mbedtls_md_free(&s_md5_ctx);
        return ESP_FAIL;
    }
    if (mbedtls_md_setup(&s_md5_ctx, info, 0) != 0) {
        mbedtls_md_free(&s_md5_ctx);
        return ESP_FAIL;
    }
    if (mbedtls_md_starts(&s_md5_ctx) != 0) {
        mbedtls_md_free(&s_md5_ctx);
        return ESP_FAIL;
    }
    s_md5_ctx_ready = true;
    return ESP_OK;
}

static esp_err_t md5_update(const uint8_t *data, size_t len)
{
    if (!s_md5_ctx_ready) return ESP_FAIL;
    if (mbedtls_md_update(&s_md5_ctx, data, len) != 0) {
        md5_cleanup();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t md5_final_hex(char *hex_out, size_t hex_size)
{
    if (!s_md5_ctx_ready) return ESP_FAIL;
    unsigned char digest[16];
    if (mbedtls_md_finish(&s_md5_ctx, digest) != 0) {
        md5_cleanup();
        return ESP_FAIL;
    }
    md5_cleanup();
    if (hex_size < 33) return ESP_FAIL;
    for (int i = 0; i < 16; i++) {
        snprintf(hex_out + i * 2, 3, "%02x", digest[i]);
    }
    hex_out[32] = '\0';
    return ESP_OK;
}

static bool hex_equal_ignore_case(const char *a, const char *b)
{
    if (!a || !b) return false;
    return strcasecmp(a, b) == 0;
}

typedef enum {
    OTA_DOWNLOAD_OK = 0,
    OTA_DOWNLOAD_FAIL,
    OTA_DOWNLOAD_WIFI_LOST,
    OTA_DOWNLOAD_CANCELLED,
} ota_download_result_t;

static ota_download_result_t do_http_download_once(const char *url, uint8_t *buf,
                                                   esp_ota_handle_t ota_handle,
                                                   int64_t *out_total_read,
                                                   int64_t *out_content_len,
                                                   int *out_last_reported)
{
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .buffer_size = OTA_BUFFER_SIZE,
        .buffer_size_tx = OTA_BUFFER_SIZE,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (!http) {
        ESP_LOGE(TAG, "http client init failed");
        return OTA_DOWNLOAD_FAIL;
    }

    esp_err_t err = esp_http_client_open(http, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http);
        return OTA_DOWNLOAD_FAIL;
    }

    int status = esp_http_client_get_status_code(http);
    int64_t content_len = esp_http_client_get_content_length(http);
    ESP_LOGI(TAG, "HTTP status=%d content_len=%" PRId64 " (url=%s)", status, content_len, url);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        esp_http_client_cleanup(http);
        return OTA_DOWNLOAD_FAIL;
    }

    *out_content_len = content_len;
    int64_t total_read = 0;

    while (1) {
        if (s_cancel_requested) {
            ESP_LOGW(TAG, "Download cancelled by user");
            esp_http_client_cleanup(http);
            return OTA_DOWNLOAD_CANCELLED;
        }

        if (!wifi_is_connected()) {
            ESP_LOGE(TAG, "WiFi disconnected during download");
            esp_http_client_cleanup(http);
            return OTA_DOWNLOAD_WIFI_LOST;
        }

        int read_retry = 0;
        int len;
        do {
            len = esp_http_client_read(http, (char *)buf, OTA_BUFFER_SIZE);
            if (len < 0 && read_retry < OTA_READ_RETRY_MAX) {
                ESP_LOGW(TAG, "http read error (attempt %d/%d): %d, retrying...",
                         read_retry + 1, OTA_READ_RETRY_MAX, len);
                vTaskDelay(pdMS_TO_TICKS(OTA_READ_RETRY_INTERVAL_MS));
                read_retry++;
            }
        } while (len < 0 && read_retry < OTA_READ_RETRY_MAX);

        if (len == 0) break;
        if (len < 0) {
            ESP_LOGE(TAG, "http read failed after %d retries", OTA_READ_RETRY_MAX);
            esp_http_client_cleanup(http);
            return OTA_DOWNLOAD_FAIL;
        }

        if (s_md5_ctx_ready) {
            md5_update(buf, len);
        }

        err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(http);
            return OTA_DOWNLOAD_FAIL;
        }

        total_read += len;

        if (content_len > 0) {
            int percent = (int)((total_read * 100) / content_len);
            int bucket = percent / OTA_PROGRESS_STEP;
            if (bucket != *out_last_reported) {
                *out_last_reported = bucket;
                report_progress(bucket * OTA_PROGRESS_STEP);
            }
        }
    }

    *out_total_read = total_read;
    esp_http_client_cleanup(http);
    return OTA_DOWNLOAD_OK;
}

static void ota_task(void *arg)
{
    ota_notify_t *notify = (ota_notify_t *)arg;
    uint8_t *buf = NULL;
    esp_ota_handle_t ota_handle = 0;

    ESP_LOGI(TAG, "Starting OTA: %s -> %s", notify->fw_url, notify->fw_version);

    strncpy(s_target_version, notify->fw_version, sizeof(s_target_version) - 1);
    s_target_version[sizeof(s_target_version) - 1] = '\0';
    s_progress = 0;
    s_error_reason[0] = '\0';
    s_ota_in_progress = true;
    s_cancel_requested = false;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        set_state(OTA_STATE_FAILED, "no running partition");
        reply_upgrade(notify->msg_id, -1, "no running partition");
        goto cleanup;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (!target) {
        set_state(OTA_STATE_FAILED, "no target partition");
        reply_upgrade(notify->msg_id, -1, "no target partition");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Target partition: offset=0x%" PRIx32 " size=0x%" PRIx32,
             target->address, target->size);

    buf = (uint8_t *)malloc(OTA_BUFFER_SIZE);
    if (!buf) {
        set_state(OTA_STATE_FAILED, "malloc failed");
        reply_upgrade(notify->msg_id, -1, "malloc failed");
        goto cleanup;
    }

    int64_t total_read = 0;
    int64_t content_len = 0;
    int retry_count = 0;
    int last_reported_percent = -1;
    esp_err_t err = ESP_OK;

    set_state(OTA_STATE_DOWNLOADING, NULL);
    led_notify_ota_start();
    reply_upgrade(notify->msg_id, 200, "downloading");
    report_progress(0);

    while (1) {
        if (s_cancel_requested) {
            set_state(OTA_STATE_FAILED, "cancelled by user");
            report_result("3", "cancelled");
            reply_upgrade(notify->msg_id, -1, "cancelled");
            goto cleanup;
        }

        if (ota_handle) {
            esp_ota_abort(ota_handle);
            ota_handle = 0;
            md5_cleanup();
        }

        err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
            set_state(OTA_STATE_FAILED, "esp_ota_begin failed");
            reply_upgrade(notify->msg_id, -1, "ota begin failed");
            goto cleanup;
        }

        if (md5_init_ctx() != ESP_OK) {
            ESP_LOGW(TAG, "MD5 init failed, continue without signature check");
        }

        last_reported_percent = -1;
        retry_count++;
        ESP_LOGI(TAG, "OTA download attempt %d/%d", retry_count, OTA_HTTP_RETRY_MAX + 1);

        ota_download_result_t dl_result = do_http_download_once(
            notify->fw_url, buf, ota_handle,
            &total_read, &content_len, &last_reported_percent);

        if (dl_result == OTA_DOWNLOAD_OK) {
            break;
        }

        if (dl_result == OTA_DOWNLOAD_CANCELLED) {
            set_state(OTA_STATE_FAILED, "cancelled by user");
            report_result("3", "cancelled");
            reply_upgrade(notify->msg_id, -1, "cancelled");
            goto cleanup;
        }

        if (dl_result == OTA_DOWNLOAD_WIFI_LOST) {
            ESP_LOGW(TAG, "WiFi lost during download, attempt %d/%d",
                     retry_count, OTA_HTTP_RETRY_MAX + 1);
        }

        if (retry_count > OTA_HTTP_RETRY_MAX) {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg),
                     "http download failed after %d attempts", OTA_HTTP_RETRY_MAX + 1);
            set_state(OTA_STATE_FAILED, err_msg);
            report_result("3", err_msg);
            reply_upgrade(notify->msg_id, -1, err_msg);
            goto cleanup;
        }

        ESP_LOGW(TAG, "Retrying in %d ms...", OTA_HTTP_RETRY_INTERVAL_MS);
        vTaskDelay(pdMS_TO_TICKS(OTA_HTTP_RETRY_INTERVAL_MS));
    }

    if (content_len > 0 && total_read != content_len) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "incomplete download %" PRId64 "/%" PRId64, total_read, content_len);
        set_state(OTA_STATE_FAILED, err_msg);
        report_result("3", err_msg);
        reply_upgrade(notify->msg_id, -1, err_msg);
        goto cleanup;
    }

    if (content_len == 0) {
        ESP_LOGW(TAG, "No Content-Length header, downloaded %" PRId64 " bytes", total_read);
    }

    if (target->size < (uint32_t)total_read) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "firmware too large: %" PRId64 " > partition 0x%" PRIx32,
                 total_read, target->size);
        set_state(OTA_STATE_FAILED, err_msg);
        report_result("3", err_msg);
        reply_upgrade(notify->msg_id, -1, err_msg);
        goto cleanup;
    }

    report_progress(100);
    set_state(OTA_STATE_VERIFYING, NULL);

    bool md5_present = (notify->fw_sign[0] != '\0'
                       && strcasecmp(notify->sign_method, "MD5") == 0);

    if (md5_present) {
        if (!s_md5_ctx_ready) {
            ESP_LOGW(TAG, "MD5 ctx not ready but sign provided, skip verify");
        } else {
            char md5_hex[33] = {0};
            if (md5_final_hex(md5_hex, sizeof(md5_hex)) == ESP_OK) {
                ESP_LOGI(TAG, "MD5 local: %s", md5_hex);
                ESP_LOGI(TAG, "MD5 expect: %s", notify->fw_sign);
                if (!hex_equal_ignore_case(md5_hex, notify->fw_sign)) {
                    set_state(OTA_STATE_FAILED, "MD5 mismatch");
                    report_result("3", "MD5 mismatch");
                    reply_upgrade(notify->msg_id, -1, "MD5 mismatch");
                    goto cleanup;
                }
                ESP_LOGI(TAG, "MD5 verified OK");
            }
        }
    } else {
        md5_cleanup();
        ESP_LOGI(TAG, "No signature provided, skip MD5 verification");
    }

    err = esp_ota_end(ota_handle);
    ota_handle = 0;
    if (err != ESP_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "esp_ota_end failed: %s", esp_err_to_name(err));
        set_state(OTA_STATE_FAILED, err_msg);
        report_result("3", err_msg);
        reply_upgrade(notify->msg_id, -1, err_msg);
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "set boot failed: %s", esp_err_to_name(err));
        set_state(OTA_STATE_FAILED, err_msg);
        report_result("3", err_msg);
        reply_upgrade(notify->msg_id, -1, err_msg);
        goto cleanup;
    }

    set_state(OTA_STATE_SWITCHING, NULL);
    reply_upgrade(notify->msg_id, 200, "success");
    report_result("0", notify->fw_version);

    set_state(OTA_STATE_REBOOT_WAIT, NULL);
    led_notify_ota_success();
    ESP_LOGI(TAG, "OTA success, rebooting in %d ms...", OTA_REBOOT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();

cleanup:
    if (ota_handle) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }
    md5_cleanup();
    if (buf) {
        free(buf);
        buf = NULL;
    }
    if (s_state == OTA_STATE_FAILED) {
        led_notify_ota_fail();
    }
    s_ota_in_progress = false;
    s_cancel_requested = false;
    if (s_ota_sem) {
        xSemaphoreGive(s_ota_sem);
    }
    if (notify) {
        free(notify);
    }
    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

static bool is_newer_version(const char *target_ver)
{
    if (!target_ver || target_ver[0] == '\0') return false;
    if (strcmp(target_ver, APP_VERSION) == 0) return false;

    int cur[3] = {0}, tgt[3] = {0};
    sscanf(APP_VERSION, "%d.%d.%d", &cur[0], &cur[1], &cur[2]);
    sscanf(target_ver, "%d.%d.%d", &tgt[0], &tgt[1], &tgt[2]);

    if (tgt[0] != cur[0]) return tgt[0] > cur[0];
    if (tgt[1] != cur[1]) return tgt[1] > cur[1];
    return tgt[2] > cur[2];
}

void ota_handle_mqtt_msg(const char *topic, int topic_len,
                         const char *data, int data_len)
{
    if (!topic || !data) return;
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring new notify");
        return;
    }

    char topic_buf[64] = {0};
    int tlen = topic_len < (int)sizeof(topic_buf) - 1 ? topic_len : (int)sizeof(topic_buf) - 1;
    memcpy(topic_buf, topic, tlen);
    topic_buf[tlen] = '\0';

    if (strstr(topic_buf, "/ota/upgrade") == NULL) return;

    char data_buf[512] = {0};
    int dlen = data_len < (int)sizeof(data_buf) - 1 ? data_len : (int)sizeof(data_buf) - 1;
    memcpy(data_buf, data, dlen);
    data_buf[dlen] = '\0';

    ESP_LOGI(TAG, "OTA notify received: %s", data_buf);

    ota_notify_t *notify = (ota_notify_t *)calloc(1, sizeof(ota_notify_t));
    if (!notify) {
        ESP_LOGE(TAG, "calloc failed");
        return;
    }

    if (!parse_notify(data_buf, notify)) {
        ESP_LOGE(TAG, "parse OTA notify failed");
        reply_upgrade(notify->msg_id, -1, "parse failed");
        free(notify);
        return;
    }

    if (!notify->force_upgrade && !is_newer_version(notify->fw_version)) {
        ESP_LOGW(TAG, "Version not newer: current=%s target=%s", APP_VERSION, notify->fw_version);
        reply_upgrade(notify->msg_id, 200, "not newer");
        free(notify);
        return;
    }

    if (!mqtt_is_connected()) {
        ESP_LOGE(TAG, "MQTT not connected, defer OTA");
        reply_upgrade(notify->msg_id, -1, "mqtt not connected");
        free(notify);
        return;
    }

    s_ota_in_progress = true;
    if (xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
                    notify, OTA_TASK_PRIORITY, &s_ota_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate ota_task failed");
        s_ota_in_progress = false;
        reply_upgrade(notify->msg_id, -1, "task create failed");
        free(notify);
        return;
    }

    set_state(OTA_STATE_IDLE, NULL);
}

bool ota_is_in_progress(void)
{
    return s_ota_in_progress;
}

void ota_get_status(ota_status_t *out)
{
    if (!out) return;
    out->state = s_state;
    out->progress = s_progress;
    strncpy(out->version, s_target_version, sizeof(out->version) - 1);
    out->version[sizeof(out->version) - 1] = '\0';
    strncpy(out->error, s_error_reason, sizeof(out->error) - 1);
    out->error[sizeof(out->error) - 1] = '\0';
}

void ota_abort(void)
{
    if (!s_ota_in_progress) return;
    ESP_LOGW(TAG, "OTA abort requested");
    s_cancel_requested = true;
}

void ota_init(void)
{
    if (s_initialized) return;
    s_initialized = true;

    if (!s_ota_sem) {
        s_ota_sem = xSemaphoreCreateBinary();
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        esp_ota_img_states_t img_state;
        if (esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
            if (img_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGW(TAG, "Previous OTA pending verify -> mark valid");
                esp_ota_mark_app_valid_cancel_rollback();
            } else if (img_state == ESP_OTA_IMG_ABORTED) {
                ESP_LOGW(TAG, "Previous OTA aborted, device may have rolled back");
            }
        }
    }

    ESP_LOGI(TAG, "OTA manager initialized, current version: %s", APP_VERSION);
}