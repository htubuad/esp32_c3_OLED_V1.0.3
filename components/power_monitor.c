#include "power_monitor.h"
#include "ntc_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "PWR_MON";

#define COOLDOWN_AFTER_RECOVERY_MS  10000

static power_monitor_cb_t s_undervolt_cb = NULL;
static power_monitor_cb_t s_recovery_cb = NULL;
static bool s_initialized = false;
static bool s_in_protection = false;

static int64_t s_cooldown_until_us = 0;

static adc_oneshot_unit_handle_t s_adc_unit = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_cali_ok = false;

static esp_timer_handle_t s_poll_timer = NULL;

static int s_battery_mv = -1;
static int s_low_count = 0;
static int s_recovery_count = 0;

static void poll_timer_cb(void *arg)
{
    power_monitor_poll();
}

esp_err_t power_monitor_init(void)
{
    if (s_initialized) return ESP_OK;

    s_adc_unit = ntc_sensor_get_adc_unit();
    if (!s_adc_unit) {
        ESP_LOGE(TAG, "ADC unit not available (init ntc_sensor first!)");
        return ESP_ERR_INVALID_STATE;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(
        adc_oneshot_config_channel(s_adc_unit, POWER_MONITOR_ADC_CHANNEL, &chan_cfg),
        TAG, "adc channel config failed");

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = POWER_MONITOR_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t cali_ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (cali_ret == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "power_monitor curve_fitting cali failed: 0x%x, fallback", cali_ret);
    }

    const esp_timer_create_args_t timer_args = {
        .callback = poll_timer_cb,
        .name = "pwr_mon_poll",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_poll_timer,
        POWER_MONITOR_SAMPLE_INTERVAL_MS * 1000));

    ESP_LOGI(TAG, "Power monitor GPIO%d (ADC1_CH%d)  atten=DB12  cali=%s  poll=%dms",
             POWER_MONITOR_ADC_GPIO, POWER_MONITOR_ADC_CHANNEL,
             s_cali_ok ? "yes" : "no", POWER_MONITOR_SAMPLE_INTERVAL_MS);

    s_initialized = true;
    return ESP_OK;
}

int power_monitor_get_battery_mv(void)
{
    return s_battery_mv;
}

bool power_monitor_is_low(void)
{
    return s_low_count >= POWER_MONITOR_UNDERVOLTAGE_COUNT;
}

bool power_monitor_in_protection(void)
{
    return s_in_protection;
}

void power_monitor_set_undervolt_cb(power_monitor_cb_t cb)
{
    s_undervolt_cb = cb;
}

void power_monitor_set_recovery_cb(power_monitor_cb_t cb)
{
    s_recovery_cb = cb;
}

static int adc_to_battery_mv(int adc_mv)
{
    int ratio = POWER_MONITOR_R5_OHM + POWER_MONITOR_R6_OHM;
    float raw_bat = (float)(adc_mv * ratio) / POWER_MONITOR_R6_OHM;
    return (int)(raw_bat * POWER_MONITOR_CAL_FACTOR);
}

static int read_adc_mv(void)
{
    int raw_sum = 0;
    for (int i = 0; i < 10; i++) {
        int raw;
        adc_oneshot_read(s_adc_unit, POWER_MONITOR_ADC_CHANNEL, &raw);
        raw_sum += raw;
    }
    int raw_avg = raw_sum / 10;

    int voltage_mv = (raw_avg * 4400) / 4095;

    if (s_cali_ok && s_cali_handle) {
        int cali_mv = 0;
        adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &cali_mv);
        if (cali_mv > 0 && cali_mv <= 5000) {
            int diff = abs(cali_mv - voltage_mv);
            if (diff <= voltage_mv / 3) {
                voltage_mv = cali_mv;
            } else {
                ESP_LOGW(TAG, "curve_fitting %d mV differs too much from fallback %d mV, bad efuse! fallback", cali_mv, voltage_mv);
                s_cali_ok = false;
            }
        }
    }
    return voltage_mv;
}

void power_monitor_poll(void)
{
    if (!s_initialized) return;

    int adc_mv = read_adc_mv();
    int bat_mv = adc_to_battery_mv(adc_mv);
    s_battery_mv = bat_mv;

    if (!s_in_protection) {
        int64_t now = esp_timer_get_time();
        if (now < s_cooldown_until_us) {
            int remaining_ms = (int)((s_cooldown_until_us - now) / 1000);
            ESP_LOGD(TAG, "In cooldown %dms left, skip undervolt check", remaining_ms);
            return;
        }

        if (bat_mv < POWER_MONITOR_UNDERVOLTAGE_MV) {
            s_low_count++;
            if (s_low_count == 1 || s_low_count == POWER_MONITOR_UNDERVOLTAGE_COUNT) {
                ESP_LOGW(TAG, "Battery low: %d mV  (count %d/%d)",
                         bat_mv, s_low_count, POWER_MONITOR_UNDERVOLTAGE_COUNT);
            }
            if (s_low_count >= POWER_MONITOR_UNDERVOLTAGE_COUNT) {
                s_in_protection = true;
                ESP_LOGE(TAG, "=== UNDERVOLTAGE PROTECTION TRIGGERED! %d mV (threshold %d mV) ===",
                         bat_mv, POWER_MONITOR_UNDERVOLTAGE_MV);
                if (s_undervolt_cb) s_undervolt_cb();
            }
        } else if (bat_mv > POWER_MONITOR_UNDERVOLTAGE_MV + 500) {
            if (s_low_count > 0) {
                ESP_LOGI(TAG, "Battery recovered: %d mV  (was low %d times)", bat_mv, s_low_count);
            }
            s_low_count = 0;
        }
    } else {
        if (bat_mv >= POWER_MONITOR_RECOVERY_MV) {
            s_recovery_count++;
            if (s_recovery_count == 1 || s_recovery_count == POWER_MONITOR_RECOVERY_COUNT) {
                ESP_LOGI(TAG, "Battery recovering: %d mV  (count %d/%d)",
                         bat_mv, s_recovery_count, POWER_MONITOR_RECOVERY_COUNT);
            }
            if (s_recovery_count >= POWER_MONITOR_RECOVERY_COUNT) {
                s_in_protection = false;
                s_low_count = 0;
                s_recovery_count = 0;
                s_cooldown_until_us = esp_timer_get_time() + COOLDOWN_AFTER_RECOVERY_MS * 1000;
                ESP_LOGI(TAG, "=== Battery recovered to %d mV, exiting protection ===", bat_mv);
                ESP_LOGI(TAG, "Cooldown %dms before next undervolt trigger", COOLDOWN_AFTER_RECOVERY_MS);
                if (s_recovery_cb) s_recovery_cb();
            }
        } else if (bat_mv < POWER_MONITOR_RECOVERY_MV - 500) {
            if (s_recovery_count > 0) {
                ESP_LOGW(TAG, "Recovery interrupted: %d mV  (was %d/%d)",
                         bat_mv, s_recovery_count, POWER_MONITOR_RECOVERY_COUNT);
            }
            s_recovery_count = 0;
        }
    }
}