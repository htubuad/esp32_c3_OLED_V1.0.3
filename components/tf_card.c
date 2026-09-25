#include "tf_card.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include "version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static const char *TAG = "TF_CARD";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static SemaphoreHandle_t s_mutex = NULL;

#define APPEND_CACHE_SIZE  2048
static char s_append_cache[APPEND_CACHE_SIZE];
static size_t s_append_cache_len = 0;
static char s_append_cache_path[64] = {0};

#define CSV_LOG_DIR          "/logs"
#define CSV_PENDING_MAX_LINES   8
#define CSV_PENDING_LINE_SIZE   64
static char s_csv_pending[CSV_PENDING_MAX_LINES][CSV_PENDING_LINE_SIZE];
static int  s_csv_pending_count = 0;
static int  s_csv_pending_write_idx = 0;
static int  s_csv_pending_read_idx = 0;

static void tf_card_notify_io_fail(void);
static bool tf_card_probe(void);
bool tf_card_reinit(void);
static int s_probe_fail_count;
static int s_io_fail_count;
static bool s_reinit_in_progress = false;
static bool append_cache_flush(void);

#define CACHE_PERIODIC_FLUSH_MS  (5 * 60 * 1000)
static esp_timer_handle_t s_cache_flush_timer = NULL;

static int s_cached_state = 0;

#define TF_LOW_WATERMARK   (1 * 1024 * 1024)   // 1MB，低于此触发清理
#define TF_HIGH_WATERMARK  (5 * 1024 * 1024)   // 5MB，清理到此停止
#define MAX_CSV_FILES      200                  // 最多跟踪 CSV 数量（32字节/条 = 6.4KB）
#define CSV_NAME_SIZE      32                   // YYYY-MM-DD.csv 刚好 14 字节

static uint64_t tf_card_get_free_bytes(void);
static int csv_name_compare(const void *a, const void *b);
static int tf_card_list_log_files(char names[][CSV_NAME_SIZE], int max_count);
static bool tf_card_cleanup_oldest_until(uint64_t target_free);
static bool tf_card_ensure_free_space(void);

static void cache_flush_timer_cb(void *arg)
{
    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));
    if (s_mounted && s_append_cache_len > 0) {
        ESP_LOGI(TAG, "[TIMER] periodic cache flush: %zu bytes", s_append_cache_len);
        append_cache_flush();
    }
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

static uint64_t tf_card_get_free_bytes(void)
{
    uint64_t total = 0, free = 0;
    if (!s_mounted) return 0;
    if (esp_vfs_fat_info(TF_MOUNT_POINT, &total, &free) == ESP_OK) {
        return free;
    }
    ESP_LOGW(TAG, "esp_vfs_fat_info failed");
    return 0;
}

static int csv_name_compare(const void *a, const void *b)
{
    const char *na = (const char *)a;
    const char *nb = (const char *)b;
    return strcmp(na, nb);
}

static int tf_card_list_log_files(char names[][CSV_NAME_SIZE], int max_count)
{
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s%s", TF_MOUNT_POINT, CSV_LOG_DIR);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open dir: %s", dir_path);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (strcmp(name + len - 4, ".csv") != 0) continue;
        if (strlen(name) >= CSV_NAME_SIZE) continue;
        strncpy(names[count], name, CSV_NAME_SIZE - 1);
        names[count][CSV_NAME_SIZE - 1] = '\0';
        count++;
    }
    closedir(dir);

    if (count > 1) {
        qsort(names, count, CSV_NAME_SIZE, csv_name_compare);
    }

    ESP_LOGI(TAG, "Found %d CSV log files", count);
    return count;
}

static bool tf_card_cleanup_oldest_until(uint64_t target_free)
{
    char names[MAX_CSV_FILES][CSV_NAME_SIZE];
    int count = tf_card_list_log_files(names, MAX_CSV_FILES);
    if (count <= 0) {
        ESP_LOGW(TAG, "No CSV files to cleanup");
        return false;
    }

    int deleted = 0;
    for (int i = 0; i < count; i++) {
        uint64_t free = tf_card_get_free_bytes();
        if (free >= target_free) break;

        char full_path[128];
        snprintf(full_path, sizeof(full_path), "%s%s/%s", TF_MOUNT_POINT, CSV_LOG_DIR, names[i]);
        if (unlink(full_path) == 0) {
            deleted++;
            ESP_LOGI(TAG, "Deleted oldest log: %s (free now %llu KB)",
                     names[i], (unsigned long long)(tf_card_get_free_bytes() / 1024));
        } else {
            ESP_LOGW(TAG, "Failed to delete %s: %s", full_path, strerror(errno));
        }
    }

    ESP_LOGI(TAG, "Cleanup done: deleted %d files, free now %llu KB",
             deleted, (unsigned long long)(tf_card_get_free_bytes() / 1024));
    return true;
}

static bool tf_card_ensure_free_space(void)
{
    if (!s_mounted) return false;
    uint64_t free = tf_card_get_free_bytes();
    if (free >= TF_LOW_WATERMARK) return true;

    ESP_LOGW(TAG, "Low space: %llu KB (< %d KB), triggering cleanup...",
             (unsigned long long)(free / 1024), TF_LOW_WATERMARK / 1024);
    return tf_card_cleanup_oldest_until(TF_HIGH_WATERMARK);
}

static bool direct_file_append(const char *path, const char *data, size_t data_len)
{
    if (!s_mounted || !path || !data) return false;

    if (!tf_card_ensure_free_space()) {
        ESP_LOGW(TAG, "direct_append: no free space (cleanup failed)");
        return false;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", TF_MOUNT_POINT, path);

    FILE *f = fopen(full_path, "a");
    if (!f) {
        ESP_LOGE(TAG, "direct_append: failed to open %s", full_path);
        return false;
    }

    size_t written = fwrite(data, 1, data_len, f);
    int err = errno;
    fclose(f);

    if (written != data_len) {
        if (err == ENOSPC) {
            ESP_LOGW(TAG, "direct_append ENOSPC mid-write (%zu/%zu), cleanup + retry",
                     written, data_len);
            tf_card_ensure_free_space();
            f = fopen(full_path, "a");
            if (f) {
                written = fwrite(data, 1, data_len, f);
                fclose(f);
                if (written == data_len) {
                    ESP_LOGI(TAG, "direct_append retry OK after cleanup");
                    return true;
                }
                ESP_LOGE(TAG, "direct_append retry also failed: %zu/%zu", written, data_len);
            }
        } else {
            ESP_LOGE(TAG, "direct_append incomplete: %zu/%zu (errno=%d %s)",
                     written, data_len, err, strerror(err));
        }
        return false;
    }
    return true;
}

static bool append_cache_flush(void)
{
    if (s_append_cache_len == 0 || s_append_cache_path[0] == '\0') return true;

    if (!s_mounted) return false;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", TF_MOUNT_POINT, s_append_cache_path);

    FILE *f = fopen(full_path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Flush: failed to open %s", full_path);
        tf_card_notify_io_fail();
        return false;
    }

    size_t written = fwrite(s_append_cache, 1, s_append_cache_len, f);
    int err = errno;
    fclose(f);

    if (written != s_append_cache_len) {
        if (err == ENOSPC) {
            ESP_LOGW(TAG, "Flush ENOSPC (%zu/%zu), cleanup + retry",
                     written, s_append_cache_len);
            tf_card_ensure_free_space();
            f = fopen(full_path, "a");
            if (f) {
                written = fwrite(s_append_cache, 1, s_append_cache_len, f);
                fclose(f);
                if (written == s_append_cache_len) {
                    s_io_fail_count = 0;
                    ESP_LOGI(TAG, "Flush retry OK after ENOSPC cleanup");
                    s_append_cache_len = 0;
                    s_append_cache_path[0] = '\0';
                    return true;
                }
            }
        }
        ESP_LOGE(TAG, "Flush incomplete: %zu/%zu (errno=%d)",
                 written, s_append_cache_len, err);
        tf_card_notify_io_fail();
        return false;
    }

    s_io_fail_count = 0;
    ESP_LOGI(TAG, "TF flush OK: %zu bytes to %s", s_append_cache_len, s_append_cache_path);
    s_append_cache_len = 0;
    s_append_cache_path[0] = '\0';
    return true;
}

static bool bus_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TF_CMD_GPIO,
        .miso_io_num = TF_D0_GPIO,
        .sclk_io_num = TF_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

void tf_card_deinit(void)
{
    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    append_cache_flush();

    if (s_cache_flush_timer) {
        esp_timer_stop(s_cache_flush_timer);
        esp_timer_delete(s_cache_flush_timer);
        s_cache_flush_timer = NULL;
        ESP_LOGI(TAG, "Cache flush timer stopped");
    }

    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(TF_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
    }
    s_cached_state = 0;
    spi_bus_free(SPI2_HOST);
    ESP_LOGD(TAG, "TF card deinitialized");

    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

bool tf_card_is_mounted(void)
{
    if (!s_mounted) return false;
    return tf_card_probe();
}

int tf_card_get_state(void)
{
    return s_cached_state;
}

bool tf_card_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "Failed to create TF card mutex");
            return false;
        }
    }

    xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    if (!bus_init()) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cs = TF_CS_GPIO;
    slot_config.gpio_cd = GPIO_NUM_NC;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(TF_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TF card mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    s_mounted = true;

    {
        char logdir[32];
        snprintf(logdir, sizeof(logdir), "%s/logs", TF_MOUNT_POINT);
        mkdir(logdir, 0755);

        FILE *marker = fopen("/tf/mock_done", "r");
        if (!marker) {
            char mockpath[64];
            snprintf(mockpath, sizeof(mockpath), "%s/logs/2026-09-20.csv", TF_MOUNT_POINT);
            FILE *mf = fopen(mockpath, "w");
            if (mf) {
                ESP_LOGW(TAG, "Writing mock CSV to %s", mockpath);
                for (int i = 0; i < 50; i++) {
                    fprintf(mf, "2026-09-20,12:%02d:00,25.%02d,60.%02d\n", i, i, i + 10);
                }
                fclose(mf);

                marker = fopen("/tf/mock_done", "w");
                if (marker) { fputs("1", marker); fclose(marker); }
                ESP_LOGI(TAG, "Mock CSV ready: 50 lines");
            }
        } else {
            fclose(marker);
        }
    }

    ESP_LOGI(TAG, "TF card mounted OK");
    s_cached_state = 1;

    if (!s_cache_flush_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = cache_flush_timer_cb,
            .name = "tf_cache_flush",
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_timer_create(&timer_args, &s_cache_flush_timer);
        esp_timer_start_periodic(s_cache_flush_timer, CACHE_PERIODIC_FLUSH_MS);
        ESP_LOGI(TAG, "Cache periodic flush timer started (%d min)",
                 CACHE_PERIODIC_FLUSH_MS / 60000);
    }

    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

bool tf_card_read_file(const char *path, char *buf, size_t buf_size, size_t *bytes_read)
{
    if (!s_mounted || !path || !buf || buf_size == 0) return false;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", TF_MOUNT_POINT, path);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for reading", full_path);
        return false;
    }

    *bytes_read = fread(buf, 1, buf_size - 1, f);
    buf[*bytes_read] = '\0';
    fclose(f);

    ESP_LOGD(TAG, "Read %zu bytes from %s", *bytes_read, full_path);
    return true;
}

bool tf_card_write_file(const char *path, const char *data, size_t data_len)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex not initialized");
        return false;
    }
    xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    if (!s_mounted || !path || !data) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", TF_MOUNT_POINT, path);

    FILE *f = fopen(full_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", full_path);
        tf_card_notify_io_fail();
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);

    if (written != data_len) {
        ESP_LOGE(TAG, "Write incomplete: %zu/%zu", written, data_len);
        tf_card_notify_io_fail();
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    s_io_fail_count = 0;
    ESP_LOGD(TAG, "Wrote %zu bytes to %s", data_len, full_path);
    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

bool tf_card_append_file(const char *path, const char *data, size_t data_len)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex not initialized");
        return false;
    }
    xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    if (!s_mounted || !path || !data) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    if (s_append_cache_path[0] != '\0' && strcmp(s_append_cache_path, path) != 0) {
        append_cache_flush();
    }

    if (s_append_cache_len + data_len >= APPEND_CACHE_SIZE) {
        append_cache_flush();
    }

    if (s_append_cache_path[0] == '\0') {
        strncpy(s_append_cache_path, path, sizeof(s_append_cache_path) - 1);
        s_append_cache_path[sizeof(s_append_cache_path) - 1] = '\0';
    }

    memcpy(s_append_cache + s_append_cache_len, data, data_len);
    s_append_cache_len += data_len;

    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

bool tf_card_flush(void)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex not initialized");
        return false;
    }
    xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));
    bool ret = append_cache_flush();
    xSemaphoreGiveRecursive(s_mutex);
    return ret;
}

bool tf_card_emergency_flush(void)
{
    ESP_LOGW(TAG, "[EMERGENCY] === Undervolt emergency flush ===");

    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    if (!s_mounted) {
        ESP_LOGW(TAG, "[EMERGENCY] TF not mounted, trying init...");
        if (!tf_card_init()) {
            ESP_LOGE(TAG, "[EMERGENCY] init failed, cache lost! append=%zu csv_pending=%d",
                     s_append_cache_len, s_csv_pending_count);
            if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
            return false;
        }
    }

    bool ok = tf_card_flush_all();
    ESP_LOGI(TAG, "[EMERGENCY] flush_all: %s (append=%zu csv_pending=%d)",
             ok ? "OK" : "FAIL", s_append_cache_len, s_csv_pending_count);

    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
    return ok;
}

bool tf_card_list_dir(const char *dir_path)
{
    if (!s_mounted) return false;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", TF_MOUNT_POINT, dir_path);

    DIR *dir = opendir(full_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s", full_path);
        return false;
    }

    ESP_LOGD(TAG, "Directory listing of %s:", full_path);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGD(TAG, "  %s", entry->d_name);
    }
    closedir(dir);
    return true;
}

bool tf_card_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted || !total_bytes || !free_bytes) return false;

    esp_err_t ret = esp_vfs_fat_info(TF_MOUNT_POINT, total_bytes, free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_info failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

static bool csv_time_valid(void)
{
    time_t now = time(NULL);
    return now >= TIME_VALID_EPOCH;
}

bool tf_card_flush_all(void)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex not initialized");
        return false;
    }
    xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    if (!s_mounted) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    if (s_csv_pending_count > 0) {
        ESP_LOGW(TAG, "Force flushing %d pending CSV lines (time may be invalid)", s_csv_pending_count);
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);

        char date_path[64];
        snprintf(date_path, sizeof(date_path), "%s/%04d-%02d-%02d.csv",
                 CSV_LOG_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        while (s_csv_pending_count > 0) {
            const char *line = s_csv_pending[s_csv_pending_read_idx];
            direct_file_append(date_path, line, strlen(line));
            s_csv_pending_read_idx = (s_csv_pending_read_idx + 1) % CSV_PENDING_MAX_LINES;
            s_csv_pending_count--;
        }
    }

    bool ret = append_cache_flush();
    xSemaphoreGiveRecursive(s_mutex);
    return ret;
}

static void csv_flush_pending_to_disk(void)
{
    while (s_csv_pending_count > 0 && csv_time_valid()) {
        const char *line = s_csv_pending[s_csv_pending_read_idx];
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);

        char date_path[64];
        snprintf(date_path, sizeof(date_path), "%s/%04d-%02d-%02d.csv",
                 CSV_LOG_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        direct_file_append(date_path, line, strlen(line));

        s_csv_pending_read_idx = (s_csv_pending_read_idx + 1) % CSV_PENDING_MAX_LINES;
        s_csv_pending_count--;
    }
}

bool tf_card_append_csv(float ntc1, float ntc2)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex not initialized");
        return false;
    }
    xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    if (!s_mounted) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    if (!tf_card_ensure_free_space()) {
        ESP_LOGW(TAG, "CSV write skipped: no free space");
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    csv_flush_pending_to_disk();

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    char csv_line[64];
    int cl = snprintf(csv_line, sizeof(csv_line),
                      "%04d-%02d-%02d,%02d:%02d:%02d,%.2f,%.2f\n",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec, ntc1, ntc2);
    if (cl <= 0) {
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    if (!csv_time_valid()) {
        if (s_csv_pending_count == CSV_PENDING_MAX_LINES) {
            s_csv_pending_read_idx = (s_csv_pending_read_idx + 1) % CSV_PENDING_MAX_LINES;
            s_csv_pending_count--;
        }
        strncpy(s_csv_pending[s_csv_pending_write_idx], csv_line, CSV_PENDING_LINE_SIZE - 1);
        s_csv_pending[s_csv_pending_write_idx][CSV_PENDING_LINE_SIZE - 1] = '\0';
        s_csv_pending_write_idx = (s_csv_pending_write_idx + 1) % CSV_PENDING_MAX_LINES;
        s_csv_pending_count++;
        ESP_LOGW(TAG, "SNTP not synced, CSV line buffered (%d/%d)", s_csv_pending_count, CSV_PENDING_MAX_LINES);
        xSemaphoreGiveRecursive(s_mutex);
        return true;
    }

    char date_path[64];
    snprintf(date_path, sizeof(date_path), "%s/%04d-%02d-%02d.csv",
             CSV_LOG_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", TF_MOUNT_POINT, date_path);

    FILE *f = fopen(full_path, "a");
    if (!f) {
        ESP_LOGE(TAG, "CSV: failed to open %s", full_path);
        tf_card_notify_io_fail();
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    size_t written = fwrite(csv_line, 1, cl, f);
    int err = errno;
    fclose(f);

    if (written != (size_t)cl) {
        if (err == ENOSPC) {
            ESP_LOGW(TAG, "CSV ENOSPC (%zu/%d), cleanup + retry", written, cl);
            tf_card_ensure_free_space();
            f = fopen(full_path, "a");
            if (f) {
                written = fwrite(csv_line, 1, cl, f);
                fclose(f);
                if (written == (size_t)cl) {
                    s_io_fail_count = 0;
                    ESP_LOGI(TAG, "CSV retry OK after cleanup");
                    xSemaphoreGiveRecursive(s_mutex);
                    return true;
                }
            }
        }
        ESP_LOGE(TAG, "CSV write incomplete: %zu/%d (errno=%d)", written, cl, err);
        tf_card_notify_io_fail();
        xSemaphoreGiveRecursive(s_mutex);
        return false;
    }

    s_io_fail_count = 0;
    xSemaphoreGiveRecursive(s_mutex);
    return true;
}

int tf_card_read_csv_by_date(const char *date_str, char *buf, size_t buf_size)
{
    if (!s_mounted || !date_str || !buf || buf_size == 0) return -1;

    char file_path[128];
    snprintf(file_path, sizeof(file_path), "%s%s/%s.csv", TF_MOUNT_POINT, CSV_LOG_DIR, date_str);

    FILE *f = fopen(file_path, "r");
    if (!f) {
        ESP_LOGD(TAG, "CSV file not found: %s", file_path);
        return 0;
    }

    size_t total_read = fread(buf, 1, buf_size - 1, f);
    buf[total_read] = '\0';
    fclose(f);

    int lines = 0;
    for (size_t i = 0; i < total_read; i++) {
        if (buf[i] == '\n') lines++;
    }

    ESP_LOGD(TAG, "Read %zu bytes, %d lines from %s", total_read, lines, file_path);
    return lines;
}

static void tf_card_notify_io_fail(void)
{
    s_io_fail_count++;
    ESP_LOGW(TAG, "TF card I/O fail (%d/2)", s_io_fail_count);
    if ((s_io_fail_count >= 2 || s_probe_fail_count >= 2) && !s_reinit_in_progress) {
        tf_card_reinit();
    }
}

static bool tf_card_probe(void)
{
    if (!s_card) return false;
    return (sdmmc_get_status(s_card) == ESP_OK);
}

bool tf_card_reinit(void)
{
    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, pdMS_TO_TICKS(500));

    s_reinit_in_progress = true;
    ESP_LOGW(TAG, "TF card lost, flushing cache and reinitializing...");
    append_cache_flush();

    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(TF_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
    }
    spi_bus_free(SPI2_HOST);

    bool ok = tf_card_init();
    if (ok) {
        s_probe_fail_count = 0;
        s_io_fail_count = 0;
        ESP_LOGI(TAG, "TF card re-mounted OK");
    } else {
        s_cached_state = 0;
        ESP_LOGW(TAG, "TF card re-init failed (card still absent?)");
    }
    s_reinit_in_progress = false;

    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
    return ok;
}

void tf_card_periodic_check(void)
{
    if (s_mounted) {
        if (tf_card_probe()) {
            s_probe_fail_count = 0;
            int old = s_cached_state;
            s_cached_state = 1;
            if (old != 1) ESP_LOGW(TAG, "TF state changed: %d -> 1 (probe OK)", old);
            return;
        }

        s_probe_fail_count++;
        s_cached_state = 0;
        ESP_LOGW(TAG, "TF card probe fail (%d/2)", s_probe_fail_count);

        if (s_probe_fail_count >= 2 || s_io_fail_count >= 2) {
            tf_card_reinit();
        }
    } else {
        s_cached_state = 0;
        tf_card_init();
    }
}