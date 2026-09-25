#include "history_query.h"
#include "tf_card.h"
#include "mqtt_aliyun.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static const char *TAG = "HISTORY";

#define QUERY_PAGE_SIZE     300
#define QUERY_JSON_BUF_SIZE 20000

static volatile bool s_query_cancelled = false;

typedef struct {
    char start_date[16];
    char start_time[16];
    char end_date[16];
    char end_time[16];
} query_params_t;

typedef struct {
    char date[16];
    char time[16];
    float f1;
    float f2;
} history_record_t;

static bool is_valid_date_format(const char *s)
{
    if (!s || strlen(s) != 10) return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (s[i] != '-') return false;
        } else {
            if (s[i] < '0' || s[i] > '9') return false;
        }
    }
    return true;
}

static bool is_valid_time_format(const char *s)
{
    if (!s || strlen(s) != 8) return false;
    for (int i = 0; i < 8; i++) {
        if (i == 2 || i == 5) {
            if (s[i] != ':') return false;
        } else {
            if (s[i] < '0' || s[i] > '9') return false;
        }
    }
    return true;
}

static int time_to_sec(const char *hhmmss)
{
    int h, m, s;
    if (sscanf(hhmmss, "%d:%d:%d", &h, &m, &s) != 3) return -1;
    return h * 3600 + m * 60 + s;
}

static void date_add_days(const char *date_str, int days, char *out)
{
    struct tm tm = {0};
    sscanf(date_str, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_mday += days;
    time_t t = mktime(&tm);
    struct tm *r = localtime(&t);
    strftime(out, 16, "%Y-%m-%d", r);
}

static int date_diff_days(const char *d1, const char *d2)
{
    struct tm t1 = {0}, t2 = {0};
    sscanf(d1, "%d-%d-%d", &t1.tm_year, &t1.tm_mon, &t1.tm_mday);
    sscanf(d2, "%d-%d-%d", &t2.tm_year, &t2.tm_mon, &t2.tm_mday);
    t1.tm_year -= 1900; t1.tm_mon -= 1;
    t2.tm_year -= 1900; t2.tm_mon -= 1;
    time_t a = mktime(&t1);
    time_t b = mktime(&t2);
    return (int)((b - a) / 86400);
}

static bool parse_csv_line(const char *line, history_record_t *rec)
{
    const char *p = line;
    const char *end;
    int len;

    end = strchr(p, ',');
    if (!end) return false;
    len = end - p;
    if (len >= (int)sizeof(rec->date)) len = sizeof(rec->date) - 1;
    memcpy(rec->date, p, len);
    rec->date[len] = '\0';
    p = end + 1;

    end = strchr(p, ',');
    if (!end) return false;
    len = end - p;
    if (len >= (int)sizeof(rec->time)) len = sizeof(rec->time) - 1;
    memcpy(rec->time, p, len);
    rec->time[len] = '\0';
    p = end + 1;

    rec->f1 = atof(p);
    while (*p && *p != ',' && *p != '\n') p++;
    if (*p == ',') p++;

    rec->f2 = atof(p);

    return true;
}

static bool record_time_in_range(const char *rec_date, const char *rec_time,
                                 const char *start_date, int start_sec,
                                 const char *end_date, int end_sec)
{
    int d_cmp_start = strcmp(rec_date, start_date);
    int d_cmp_end = strcmp(rec_date, end_date);

    if (d_cmp_start < 0) return false;
    if (d_cmp_end > 0) return false;

    if (d_cmp_start == 0) {
        int t = time_to_sec(rec_time);
        if (t < start_sec) return false;
    }
    if (d_cmp_end == 0) {
        int t = time_to_sec(rec_time);
        if (t > end_sec) return false;
    }

    return true;
}

static void send_query_end(int record_count)
{
    char buf[96];
    snprintf(buf, sizeof(buf),
        "{\"Dir\":\"D>C\",\"Cmd\":\"QUERY_END\",\"RecordCount\":%d}",
        record_count);
    ESP_LOGI(TAG, "TX QUERY_END: count=%d", record_count);
    mqtt_publish_aliyun_params(buf);
}

static void send_query_error(const char *reason)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"Dir\":\"D>C\",\"Cmd\":\"QUERY_ERROR\",\"Reason\":\"%s\"}",
        reason);
    ESP_LOGW(TAG, "TX QUERY_ERROR: %s", reason);
    mqtt_publish_aliyun_params(buf);
}

static int count_day_records(const char *query_date,
                              int start_sec, const char *global_start_date,
                              int end_sec, const char *global_end_date)
{
    char file_path[64];
    snprintf(file_path, sizeof(file_path), "%s/logs/%s.csv", TF_MOUNT_POINT, query_date);

    FILE *f = fopen(file_path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        tf_card_reinit();
        f = fopen(file_path, "r");
        if (!f) return 0;
    }

    int count = 0;
    char csv_line[128];
    while (fgets(csv_line, sizeof(csv_line), f)) {
        history_record_t rec;
        if (!parse_csv_line(csv_line, &rec)) continue;
        if (record_time_in_range(rec.date, rec.time,
                                 global_start_date, start_sec,
                                 global_end_date, end_sec)) {
            count++;
        }
    }
    fclose(f);
    return count;
}

static int send_day_data(const char *query_date, int day_total_pages,
                          int start_sec, const char *global_start_date,
                          int end_sec, const char *global_end_date)
{
    char file_path[64];
    snprintf(file_path, sizeof(file_path), "%s/logs/%s.csv", TF_MOUNT_POINT, query_date);

    FILE *f = fopen(file_path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        tf_card_reinit();
        f = fopen(file_path, "r");
        if (!f) return 0;
    }

    char json_buf[QUERY_JSON_BUF_SIZE];
    int pos = 0;
    int page_records = 0;
    int current_page = 0;
    int sent_records = 0;

    char csv_line[128];
    while (fgets(csv_line, sizeof(csv_line), f)) {
        if (s_query_cancelled) break;

        history_record_t rec;
        if (!parse_csv_line(csv_line, &rec)) continue;
        if (!record_time_in_range(rec.date, rec.time,
                                   global_start_date, start_sec,
                                   global_end_date, end_sec)) {
            continue;
        }

        if (page_records == 0) {
            pos = snprintf(json_buf, QUERY_JSON_BUF_SIZE,
                "{\"Dir\":\"D>C\",\"Cmd\":\"QUERY_DATA\","
                "\"QueryDate\":\"%s\",\"Page\":%d,\"TotalPages\":%d,"
                "\"Records\":[",
                query_date, current_page, day_total_pages);
        } else {
            pos += snprintf(json_buf + pos, QUERY_JSON_BUF_SIZE - pos, ",");
        }

        pos += snprintf(json_buf + pos, QUERY_JSON_BUF_SIZE - pos,
            "{\"date\":\"%s\",\"t\":\"%s\",\"f1\":%.2f,\"f2\":%.2f}",
            rec.date, rec.time, rec.f1, rec.f2);

        page_records++;
        sent_records++;

        if (page_records >= QUERY_PAGE_SIZE) {
            pos += snprintf(json_buf + pos, QUERY_JSON_BUF_SIZE - pos, "]}");
            json_buf[pos] = '\0';

            ESP_LOGI(TAG, "TX QUERY_DATA %s page %d/%d (%d records)",
                     query_date, current_page, day_total_pages, page_records);
            mqtt_publish_aliyun_params(json_buf);
            vTaskDelay(pdMS_TO_TICKS(30));

            if (s_query_cancelled) break;

            current_page++;
            page_records = 0;
        }
    }

    if (page_records > 0 && !s_query_cancelled) {
        pos += snprintf(json_buf + pos, QUERY_JSON_BUF_SIZE - pos, "]}");
        json_buf[pos] = '\0';

        ESP_LOGI(TAG, "TX QUERY_DATA %s last page %d/%d (%d records)",
                 query_date, current_page, day_total_pages, page_records);
        mqtt_publish_aliyun_params(json_buf);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    fclose(f);
    return sent_records;
}

static void history_query_task(void *arg)
{
    query_params_t *params = (query_params_t *)arg;

    ESP_LOGI(TAG, "===== History query start: %s %s ~ %s %s =====",
             params->start_date, params->start_time,
             params->end_date, params->end_time);

    mqtt_pause_report_ms(30000);
    s_query_cancelled = false;

    int total_days = date_diff_days(params->start_date, params->end_date) + 1;
    if (total_days < 1 || total_days > QUERY_MAX_DAYS) {
        send_query_error("RANGE_TOO_LARGE");
        mqtt_pause_report_ms(0);
        free(params);
        vTaskDelete(NULL);
        return;
    }

    int start_sec = time_to_sec(params->start_time);
    int end_sec = time_to_sec(params->end_time);
    if (start_sec < 0 || end_sec < 0) {
        send_query_error("INVALID_TIME");
        mqtt_pause_report_ms(0);
        free(params);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Phase 1: count per-day records...");
    int per_day_count[QUERY_MAX_DAYS];
    for (int day_offset = 0; day_offset < total_days; day_offset++) {
        if (s_query_cancelled) break;
        char cur_date[16];
        date_add_days(params->start_date, day_offset, cur_date);
        per_day_count[day_offset] = count_day_records(cur_date, start_sec, params->start_date,
                                                      end_sec, params->end_date);
        ESP_LOGI(TAG, "  %s: %d records", cur_date, per_day_count[day_offset]);
    }

    if (s_query_cancelled) {
        ESP_LOGW(TAG, "Count phase cancelled");
        mqtt_pause_report_ms(0);
        free(params);
        vTaskDelete(NULL);
        return;
    }

    int total_records = 0;
    for (int i = 0; i < total_days; i++) total_records += per_day_count[i];

    if (total_records == 0) {
        ESP_LOGI(TAG, "No matching records");
        send_query_end(0);
        mqtt_pause_report_ms(0);
        free(params);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Phase 2: send per-day data...");
    int total_sent = 0;
    for (int day_offset = 0; day_offset < total_days; day_offset++) {
        if (s_query_cancelled) break;
        if (per_day_count[day_offset] == 0) continue;

        char cur_date[16];
        date_add_days(params->start_date, day_offset, cur_date);

        int day_total_pages = (per_day_count[day_offset] + QUERY_PAGE_SIZE - 1) / QUERY_PAGE_SIZE;
        total_sent += send_day_data(cur_date, day_total_pages,
                                     start_sec, params->start_date,
                                     end_sec, params->end_date);
    }

    if (s_query_cancelled) {
        ESP_LOGW(TAG, "===== Query cancelled: %d records sent =====", total_sent);
    } else {
        ESP_LOGI(TAG, "===== Query done: %d records =====", total_sent);
        send_query_end(total_sent);
    }

    mqtt_pause_report_ms(0);
    free(params);
    vTaskDelete(NULL);
}

void history_query_handle(const char *payload, int len)
{
    if (!payload || len <= 0) return;

    cJSON *root = cJSON_ParseWithLength(payload, (size_t)len);
    if (!root) return;

    cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(root, "Cmd");
    if (!(j_cmd && cJSON_IsString(j_cmd))) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(j_cmd->valuestring, "QUERY_CANCEL") == 0) {
        ESP_LOGW(TAG, "RX QUERY_CANCEL");
        s_query_cancelled = true;
        cJSON_Delete(root);
        return;
    }

    if (strcmp(j_cmd->valuestring, "QUERY") != 0) {
        cJSON_Delete(root);
        return;
    }

    query_params_t params;

    cJSON *j_query_date = cJSON_GetObjectItemCaseSensitive(root, "QueryDate");
    cJSON *j_start_date = cJSON_GetObjectItemCaseSensitive(root, "StartDate");
    cJSON *j_end_date   = cJSON_GetObjectItemCaseSensitive(root, "EndDate");
    cJSON *j_start_time = cJSON_GetObjectItemCaseSensitive(root, "StartTime");
    cJSON *j_end_time   = cJSON_GetObjectItemCaseSensitive(root, "EndTime");

    bool have_query_date = (j_query_date && cJSON_IsString(j_query_date));
    bool have_range = (j_start_date && cJSON_IsString(j_start_date)) ||
                      (j_end_date && cJSON_IsString(j_end_date));

    if (have_query_date && !have_range) {
        const char *d = j_query_date->valuestring;
        if (!is_valid_date_format(d)) {
            ESP_LOGW(TAG, "Invalid QueryDate: %s", d ? d : "null");
            send_query_error("INVALID_DATE");
            cJSON_Delete(root);
            return;
        }
        strncpy(params.start_date, d, 15); params.start_date[15] = '\0';
        strncpy(params.end_date, d, 15); params.end_date[15] = '\0';
        strcpy(params.start_time, "00:00:00");
        strcpy(params.end_time, "23:59:59");
    } else if (have_range) {
        const char *sd = (j_start_date && cJSON_IsString(j_start_date)) ? j_start_date->valuestring : NULL;
        const char *ed = (j_end_date && cJSON_IsString(j_end_date)) ? j_end_date->valuestring : NULL;

        if (!sd || !ed || !is_valid_date_format(sd) || !is_valid_date_format(ed)) {
            ESP_LOGW(TAG, "Invalid StartDate/EndDate");
            send_query_error("INVALID_DATE");
            cJSON_Delete(root);
            return;
        }
        strncpy(params.start_date, sd, 15); params.start_date[15] = '\0';
        strncpy(params.end_date, ed, 15); params.end_date[15] = '\0';

        if (strcmp(params.end_date, params.start_date) < 0) {
            ESP_LOGW(TAG, "EndDate before StartDate");
            send_query_error("INVALID_RANGE");
            cJSON_Delete(root);
            return;
        }

        int diff = date_diff_days(params.start_date, params.end_date) + 1;
        if (diff > QUERY_MAX_DAYS) {
            ESP_LOGW(TAG, "Range %d days exceeds max %d", diff, QUERY_MAX_DAYS);
            send_query_error("RANGE_TOO_LARGE");
            cJSON_Delete(root);
            return;
        }

        if (j_start_time && cJSON_IsString(j_start_time)) {
            if (!is_valid_time_format(j_start_time->valuestring)) {
                ESP_LOGW(TAG, "Invalid StartTime: %s", j_start_time->valuestring);
                send_query_error("INVALID_TIME");
                cJSON_Delete(root);
                return;
            }
            strncpy(params.start_time, j_start_time->valuestring, 15); params.start_time[15] = '\0';
        } else {
            strcpy(params.start_time, "00:00:00");
        }

        if (j_end_time && cJSON_IsString(j_end_time)) {
            if (!is_valid_time_format(j_end_time->valuestring)) {
                ESP_LOGW(TAG, "Invalid EndTime: %s", j_end_time->valuestring);
                send_query_error("INVALID_TIME");
                cJSON_Delete(root);
                return;
            }
            strncpy(params.end_time, j_end_time->valuestring, 15); params.end_time[15] = '\0';
        } else {
            strcpy(params.end_time, "23:59:59");
        }

        if (strcmp(params.start_date, params.end_date) == 0) {
            int ss = time_to_sec(params.start_time);
            int es = time_to_sec(params.end_time);
            if (ss > es) {
                ESP_LOGW(TAG, "EndTime before StartTime on same day");
                send_query_error("INVALID_TIME");
                cJSON_Delete(root);
                return;
            }
        }
    } else {
        ESP_LOGW(TAG, "Missing query fields");
        send_query_error("INVALID_FIELD");
        cJSON_Delete(root);
        return;
    }

    query_params_t *dup = malloc(sizeof(query_params_t));
    if (!dup) {
        ESP_LOGE(TAG, "malloc failed");
        send_query_error("NO_MEM");
        cJSON_Delete(root);
        return;
    }
    *dup = params;
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Launch query task: %s %s ~ %s %s",
             dup->start_date, dup->start_time,
             dup->end_date, dup->end_time);

    BaseType_t ret = xTaskCreate(history_query_task, "query", 24576, dup, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        free(dup);
        send_query_error("NO_MEM");
    }
}