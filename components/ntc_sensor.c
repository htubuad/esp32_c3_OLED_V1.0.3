#include "ntc_sensor.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_rom_sys.h"
#include <math.h>
#include <string.h>

static const char *TAG = "NTC";

static adc_oneshot_unit_handle_t s_adc_unit = NULL;
static adc_cali_handle_t s_cali1 = NULL;
static adc_cali_handle_t s_cali2 = NULL;
static bool s_cali1_bad = false;
static bool s_cali2_bad = false;
static adc_channel_t s_chan1 = ADC_CHANNEL_3;
static adc_channel_t s_chan2 = ADC_CHANNEL_4;
static bool s_initialized = false;

static int s_last_raw1 = 0, s_last_raw2 = 0;
static int s_last_mv1 = 0, s_last_mv2 = 0;

static adc_cali_handle_t create_curve_fitting_cali(adc_channel_t chan)
{
    adc_cali_handle_t cali = NULL;
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cfg, &cali);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "curve_fitting cali failed for chan %d: 0x%x", chan, ret);
        return NULL;
    }
    return cali;
}

static int raw_to_voltage_safe(adc_channel_t chan, int raw, adc_cali_handle_t cali, bool *cali_bad_flag)
{
    int fallback_mv = (raw * NTC_VOLTAGE_MV) / 4095;

    if (!cali) return fallback_mv;

    if (*cali_bad_flag) return fallback_mv;

    int cali_mv = 0;
    adc_cali_raw_to_voltage(cali, raw, &cali_mv);

    if (cali_mv <= 0 || cali_mv > 5000) {
        ESP_LOGW(TAG, "chan %d curve_fitting gave %d mV (raw=%d), bad data! fallback", chan, cali_mv, raw);
        *cali_bad_flag = true;
        return fallback_mv;
    }

    if (cali_mv > NTC_VOLTAGE_MV) {
        ESP_LOGW(TAG, "chan %d curve_fitting %d mV exceeds VCC %d, fallback (raw=%d)",
                 chan, cali_mv, NTC_VOLTAGE_MV, raw);
        *cali_bad_flag = true;
        return fallback_mv;
    }

    if (raw > 100 && raw < 4000) {
        int diff = abs(cali_mv - fallback_mv);
        if (diff > fallback_mv / 3) {
            ESP_LOGW(TAG, "chan %d curve_fitting %d mV differs too much from fallback %d mV, bad efuse! fallback",
                     chan, cali_mv, fallback_mv);
            *cali_bad_flag = true;
            return fallback_mv;
        }
    }

    return cali_mv;
}

esp_err_t ntc_sensor_init(void)
{
    if (s_initialized) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_unit), TAG, "ADC unit create failed");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    adc_channel_t ch1 = (adc_channel_t)NTC1_ADC_GPIO;
    adc_channel_t ch2 = (adc_channel_t)NTC2_ADC_GPIO;

    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_unit, ch1, &chan_cfg), TAG, "ADC CH1 config failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_unit, ch2, &chan_cfg), TAG, "ADC CH2 config failed");

    s_chan1 = ch1;
    s_chan2 = ch2;

    s_cali1 = create_curve_fitting_cali(ch1);
    s_cali2 = create_curve_fitting_cali(ch2);

    s_initialized = true;
    ESP_LOGI(TAG, "NTC1 GPIO%d(curve_fit=%s)  NTC2 GPIO%d(curve_fit=%s)  atten=DB12",
             NTC1_ADC_GPIO, s_cali1 ? "OK" : "no",
             NTC2_ADC_GPIO, s_cali2 ? "OK" : "no");
    return ESP_OK;
}

adc_oneshot_unit_handle_t ntc_sensor_get_adc_unit(void)
{
    return s_adc_unit;
}

bool ntc_sensor_is_calibrated(void)
{
    return s_cali1 && s_cali2 && !s_cali1_bad && !s_cali2_bad;
}

static float calc_temp(adc_channel_t chan, adc_cali_handle_t cali, bool *cali_bad_flag)
{
    if (!s_initialized) return -999.0f;

    int raw;
    adc_oneshot_read(s_adc_unit, chan, &raw);
    esp_rom_delay_us(500);

    int sum = 0;
    for (int i = 0; i < 10; i++) {
        adc_oneshot_read(s_adc_unit, chan, &raw);
        sum += raw;
    }
    raw = sum / 10;
    int voltage_mv = raw_to_voltage_safe(chan, raw, cali, cali_bad_flag);

    if (voltage_mv > NTC_VOLTAGE_MV) {
        voltage_mv = NTC_VOLTAGE_MV;
    }
    if (voltage_mv < 0) {
        voltage_mv = 0;
    }

    if (chan == s_chan1) {
        s_last_raw1 = raw;
        s_last_mv1 = voltage_mv;
    } else {
        s_last_raw2 = raw;
        s_last_mv2 = voltage_mv;
    }

    float v_ratio = (float)voltage_mv / (float)NTC_VOLTAGE_MV;

    if (v_ratio >= 0.999f) {
        v_ratio = 0.999f;
    }
    if (v_ratio <= 0.001f) {
        v_ratio = 0.001f;
    }

    float r_ntc = (float)NTC_FIXED_RES_OHM * v_ratio / (1.0f - v_ratio);

    float inv_t = 1.0f / NTC_NOMINAL_TEMP_K + (1.0f / NTC_B_VALUE) * logf(r_ntc / NTC_NOMINAL_RES_OHM);
    float temp_c = (1.0f / inv_t) - 273.15f;

    if (isnan(temp_c) || isinf(temp_c)) {
        ESP_LOGW(TAG, "chan %d raw=%d mv=%d r_ntc=%.1f -> NaN/Inf, fallback %.1fC",
                 chan, raw, voltage_mv, r_ntc, NTC_OPEN_CIRCUIT_TEMP);
        return NTC_OPEN_CIRCUIT_TEMP;
    }

    return temp_c;
}

float ntc_sensor_get1(void)
{
    return calc_temp(s_chan1, s_cali1, &s_cali1_bad);
}

float ntc_sensor_get2(void)
{
    return calc_temp(s_chan2, s_cali2, &s_cali2_bad);
}

void ntc_sensor_get_raw(int *raw1, int *raw2, int *mv1, int *mv2)
{
    if (raw1) *raw1 = s_last_raw1;
    if (raw2) *raw2 = s_last_raw2;
    if (mv1)  *mv1  = s_last_mv1;
    if (mv2)  *mv2  = s_last_mv2;
}