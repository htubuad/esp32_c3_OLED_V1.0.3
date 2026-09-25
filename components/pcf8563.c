#include "pcf8563.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "PCF8563";

#define REG_VL_SECONDS  0x02
#define REG_MINUTES     0x03
#define REG_HOURS       0x04
#define REG_DAYS        0x05
#define REG_WEEKDAYS    0x06
#define REG_MONTHS      0x07
#define REG_YEARS       0x08

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static bool s_initialized = false;

static uint8_t bcd_to_dec(uint8_t bcd)
{
    return ((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F);
}

static uint8_t dec_to_bcd(uint8_t dec)
{
    return ((dec / 10) << 4) | (dec % 10);
}

static bool reg_read(uint8_t reg, uint8_t *val)
{
    uint8_t addr = reg;
    esp_err_t ret = i2c_master_transmit_receive(s_i2c_dev, &addr, 1, val, 1, -1);
    return ret == ESP_OK;
}

static bool reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    esp_err_t ret = i2c_master_transmit(s_i2c_dev, buf, 2, -1);
    return ret == ESP_OK;
}

bool pcf8563_init(void)
{
    if (s_initialized) return true;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PCF8563_I2C_SDA_GPIO,
        .scl_io_num = PCF8563_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8563_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return false;
    }

    uint8_t ctrl1 = 0;
    if (!reg_read(0x00, &ctrl1)) {
        ESP_LOGE(TAG, "PCF8563 not responding");
        i2c_master_bus_rm_device(s_i2c_dev);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_dev = NULL;
        s_i2c_bus = NULL;
        return false;
    }

    bool stopped = (ctrl1 & 0x20) != 0;
    if (stopped) {
        ESP_LOGW(TAG, "PCF8563 STOP bit set, restarting oscillator");
        reg_write(0x00, ctrl1 & ~0x20);
    }

    uint8_t sec_reg = 0;
    reg_read(REG_VL_SECONDS, &sec_reg);

    bool vl = (sec_reg & 0x80) != 0;
    if (vl) {
        ESP_LOGW(TAG, "PCF8563 VL bit set (low voltage detected), time may be invalid");
        uint8_t ctrl2 = 0;
        reg_read(0x01, &ctrl2);
        reg_write(0x01, ctrl2 & ~0x0C);
        reg_write(REG_VL_SECONDS, 0);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "PCF8563 init OK, VL=%d STOP=%d", vl, stopped);
    return true;
}

bool pcf8563_read_time(struct tm *out)
{
    if (!s_initialized || !out) return false;

    uint8_t regs[7];
    uint8_t addr = REG_VL_SECONDS;
    esp_err_t ret = i2c_master_transmit_receive(s_i2c_dev, &addr, 1, regs, 7, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: %s", esp_err_to_name(ret));
        return false;
    }

    out->tm_sec  = bcd_to_dec(regs[0] & 0x7F);
    out->tm_min  = bcd_to_dec(regs[1] & 0x7F);
    out->tm_hour = bcd_to_dec(regs[2] & 0x3F);
    out->tm_mday = bcd_to_dec(regs[3] & 0x3F);
    out->tm_wday = bcd_to_dec(regs[4] & 0x07);
    out->tm_mon  = bcd_to_dec(regs[5] & 0x1F) - 1;
    out->tm_year = bcd_to_dec(regs[6]) + 100;

    if (out->tm_year < 70) out->tm_year += 100;

    return true;
}

bool pcf8563_set_time(const struct tm *in)
{
    if (!s_initialized || !in) return false;

    uint8_t regs[7];
    regs[0] = dec_to_bcd(in->tm_sec) & 0x7F;
    regs[1] = dec_to_bcd(in->tm_min) & 0x7F;
    regs[2] = dec_to_bcd(in->tm_hour) & 0x3F;
    regs[3] = dec_to_bcd(in->tm_mday) & 0x3F;
    regs[4] = dec_to_bcd(in->tm_wday) & 0x07;
    regs[5] = dec_to_bcd(in->tm_mon + 1) & 0x1F;
    if (in->tm_year >= 100) regs[5] |= 0x80;
    regs[6] = dec_to_bcd(in->tm_year % 100);

    for (int i = 0; i < 7; i++) {
        uint8_t buf[2] = { (uint8_t)(REG_VL_SECONDS + i), regs[i] };
        esp_err_t ret = i2c_master_transmit(s_i2c_dev, buf, 2, -1);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2C write reg %d failed: %s", i, esp_err_to_name(ret));
            return false;
        }
    }

    ESP_LOGI(TAG, "PCF8563 set: %04d-%02d-%02d %02d:%02d:%02d (C=%d)",
             in->tm_year + 1900, in->tm_mon + 1, in->tm_mday,
             in->tm_hour, in->tm_min, in->tm_sec,
             in->tm_year >= 100 ? 1 : 0);
    return true;
}

bool pcf8563_is_running(void)
{
    if (!s_initialized) return false;
    uint8_t sec = 0;
    if (!reg_read(REG_VL_SECONDS, &sec)) return false;
    return (sec & 0x80) == 0;
}