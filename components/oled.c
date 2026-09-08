#include "oled.h"
#include "oledfont.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "OLED";

static uint8_t s_fb[OLED_W * OLED_PAGES];
static volatile oled_state_t s_state = OLED_STATE_INIT;
static uint8_t s_dirty_page_start = OLED_PAGES;
static uint8_t s_dirty_page_end   = 0;
static bool s_fb_dirty = false;

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;

static const uint8_t s_oled_init_cmds[] = {
    0xAE,
    0x20, 0x10,
    0xB0,
    0xC8,
    0x00,
    0x10,
    0x40,
    0x81, 0xFF,
    0xA1,
    0xA6,
    0xA8, 0x3F,
    0xA4,
    0xD3, 0x00,
    0xD5, 0xF0,
    0xD9, 0x22,
    0xDA, 0x12,
    0xDB, 0x20,
    0x8D, 0x14,
    0xAF,
};

static esp_err_t oled_write_buf(uint8_t ctrl, const uint8_t *data, size_t len)
{
    if (!s_dev_handle) return ESP_ERR_INVALID_STATE;
    uint8_t buf[256];
    if (len + 1 > sizeof(buf)) {
        ESP_LOGW(TAG, "oled_write_buf overflow len=%d", len);
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = ctrl;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s_dev_handle, buf, len + 1, -1);
}

static esp_err_t oled_write_cmds(const uint8_t *cmds, size_t len)
{
    return oled_write_buf(OLED_CMD, cmds, len);
}

static void oled_set_column_page(uint8_t col, uint8_t page)
{
    uint8_t cmds[3];
    cmds[0] = 0xB0 | (page & 0x07);
    cmds[1] = 0x10 | ((col >> 4) & 0x0F);
    cmds[2] = 0x00 | (col & 0x0F);
    oled_write_cmds(cmds, 3);
}

void OLED_Init(void)
{
    ESP_LOGI(TAG, "SSD1306 I2C init (SDA=%d, SCL=%d)", OLED_SDA_GPIO, OLED_SCL_GPIO);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags.disable_ack_check = 0,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle));

    ESP_LOGI(TAG, "Probe 0x%02X: %s", OLED_I2C_ADDR,
             i2c_master_probe(s_bus_handle, OLED_I2C_ADDR, -1) == ESP_OK ? "OK" : "NO ACK");

    oled_write_cmds(s_oled_init_cmds, sizeof(s_oled_init_cmds));

    OLED_Clear();
    s_state = OLED_STATE_IDLE;
    ESP_LOGI(TAG, "SSD1306 init done");
}

void OLED_Clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    for (int page = 0; page < OLED_PAGES; page++) {
        oled_set_column_page(0, page);
        oled_write_buf(OLED_DATA, s_fb + page * OLED_W, OLED_W);
    }
    s_fb_dirty = false;
    s_dirty_page_start = OLED_PAGES;
    s_dirty_page_end   = 0;
}

void OLED_Update(void)
{
    if (s_state != OLED_STATE_IDLE || !s_fb_dirty) return;

    s_state = OLED_STATE_BUSY;

    uint8_t start = s_dirty_page_start;
    uint8_t end   = s_dirty_page_end;
    if (start >= OLED_PAGES || start > end) {
        start = 0;
        end = OLED_PAGES - 1;
    }

    for (uint8_t page = start; page <= end; page++) {
        oled_set_column_page(0, page);
        oled_write_buf(OLED_DATA, s_fb + page * OLED_W, OLED_W);
    }

    s_fb_dirty = false;
    s_dirty_page_start = OLED_PAGES;
    s_dirty_page_end   = 0;
    s_state = OLED_STATE_IDLE;
}

oled_state_t OLED_GetState(void)
{
    return s_state;
}

void OLED_DrawPixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_W || y >= OLED_H) return;
    uint8_t page = y / 8;
    uint8_t bit  = y % 8;
    uint16_t idx  = page * OLED_W + x;
    if (on) {
        s_fb[idx] |= (1 << bit);
    } else {
        s_fb[idx] &= ~(1 << bit);
    }
    s_fb_dirty = true;
    if (page < s_dirty_page_start) s_dirty_page_start = page;
    if (page > s_dirty_page_end)   s_dirty_page_end   = page;
}

void OLED_ClearArea(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    for (uint8_t yy = y; yy < y + h; yy++) {
        for (uint8_t xx = x; xx < x + w; xx++) {
            OLED_DrawPixel(xx, yy, false);
        }
    }
}

static void fb_set_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_W || y >= OLED_H) return;
    uint8_t page = y >> 3;
    uint8_t bit  = y & 0x07;
    uint16_t idx = page * OLED_W + x;
    if (on) s_fb[idx] |= (1 << bit);
    else    s_fb[idx] &= ~(1 << bit);
    s_fb_dirty = true;
    if (page < s_dirty_page_start) s_dirty_page_start = page;
    if (page > s_dirty_page_end)   s_dirty_page_end   = page;
}

static void draw_font_glyph(uint8_t x, uint8_t y, char ch, oled_font_size_t size, bool invert)
{
    uint8_t c = (uint8_t)ch;
    if (c < 32 || c > 126) return;
    uint8_t chr1 = c - ' ';
    uint8_t i, m, temp;
    uint8_t x0 = x, y0 = y;
    uint8_t size1 = 0, size2 = 0;

    if (size == OLED_SIZE_6x8) {
        size1 = 8;
        size2 = 6;
    } else if (size == OLED_SIZE_12) {
        size1 = 12;
        size2 = 12;
    } else if (size == OLED_SIZE_16) {
        size1 = 16;
        size2 = 16;
    } else if (size == OLED_SIZE_24) {
        size1 = 24;
        size2 = 36;
    } else {
        return;
    }

    for (i = 0; i < size2; i++) {
        if (size1 == 8)       temp = asc2_0806[chr1][i];
        else if (size1 == 12) temp = asc2_1206[chr1][i];
        else if (size1 == 16) temp = asc2_1608[chr1][i];
        else if (size1 == 24) temp = asc2_2412[chr1][i];
        else return;

        for (m = 0; m < 8; m++) {
            bool p = (temp & 0x01);
            if (invert) p = !p;
            fb_set_pixel(x, y, p);
            temp >>= 1;
            y++;
        }
        x++;
        if ((size1 != 8) && ((x - x0) == size1 / 2)) {
            x = x0;
            y0 += 8;
        }
        y = y0;
    }
}

void OLED_ShowChar(uint8_t x, uint8_t y, char ch)
{
    draw_font_glyph(x, y, ch, OLED_SIZE_16, false);
}

void OLED_ShowCharSize(uint8_t x, uint8_t y, char ch, oled_font_size_t size, bool invert)
{
    draw_font_glyph(x, y, ch, size, invert);
}

void OLED_ShowString(uint8_t x, uint8_t y, const char *str)
{
    if (!str) return;
    OLED_ShowStringSize(x, y, str, OLED_SIZE_16, false);
}

void OLED_ShowStringSize(uint8_t x, uint8_t y, const char *str, oled_font_size_t size, bool invert)
{
    if (!str) return;
    uint8_t cx = x;
    uint8_t cy = y;
    uint8_t w = 0;
    if (size == OLED_SIZE_6x8) w = 6;
    else if (size == OLED_SIZE_12) w = 12;
    else if (size == OLED_SIZE_16) w = 8;
    else if (size == OLED_SIZE_24) w = 24;
    else return;

    while (*str) {
        uint8_t h = 0;
        if (size == OLED_SIZE_6x8) h = 8;
        else if (size == OLED_SIZE_12) h = 12;
        else if (size == OLED_SIZE_16) h = 16;
        else if (size == OLED_SIZE_24) h = 24;

        if (cx > OLED_W - w) {
            cx = 0;
            cy += h;
        }
        if (cy > OLED_H - h) break;
        draw_font_glyph(cx, cy, *str, size, invert);
        cx += w;
        str++;
    }
}

void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, oled_font_size_t size, bool invert)
{
    uint8_t t, temp, m = 0;
    if (size == OLED_SIZE_6x8) m = 2;
    for (t = 0; t < len; t++) {
        uint32_t divisor = 1;
        for (uint8_t i = 0; i < len - t - 1; i++) divisor *= 10;
        temp = (num / divisor) % 10;
        draw_font_glyph(x + ((uint8_t)(size / 2) + m) * t, y, temp + '0', size, invert);
    }
}

void OLED_DrawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, bool on)
{
    int dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        OLED_DrawPixel(x1, y1, on);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 < dx)  { err += dx; y1 += sy; }
    }
}

void OLED_DrawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on)
{
    OLED_DrawLine(x, y, x + w, y, on);
    OLED_DrawLine(x, y, x, y + h, on);
    OLED_DrawLine(x + w, y, x + w, y + h, on);
    OLED_DrawLine(x, y + h, x + w, y + h, on);
}

void OLED_DrawCircle(uint8_t x, uint8_t y, uint8_t r, bool on)
{
    int32_t f = 1 - r;
    int32_t ddF_x = 0;
    int32_t ddF_y = 2 * r;
    int32_t xx = 0;
    int32_t yy = r;
    OLED_DrawPixel(x, y + r, on);
    OLED_DrawPixel(x, y - r, on);
    OLED_DrawPixel(x + r, y, on);
    OLED_DrawPixel(x - r, y, on);
    while (xx < yy) {
        if (f >= 0) {
            yy--;
            ddF_y -= 2;
            f += ddF_y;
        }
        xx++;
        ddF_x += 2;
        f += ddF_x + 1;
        OLED_DrawPixel(x + xx, y + yy, on);
        OLED_DrawPixel(x - xx, y + yy, on);
        OLED_DrawPixel(x + xx, y - yy, on);
        OLED_DrawPixel(x - xx, y - yy, on);
        OLED_DrawPixel(x + yy, y + xx, on);
        OLED_DrawPixel(x - yy, y + xx, on);
        OLED_DrawPixel(x + yy, y - xx, on);
        OLED_DrawPixel(x - yy, y - xx, on);
    }
}

void OLED_DrawFillCircle(uint8_t cx, uint8_t cy, uint8_t r, bool on)
{
    int32_t f = 1 - r;
    int32_t ddF_x = 0;
    int32_t ddF_y = 2 * r;
    int32_t xx = 0;
    int32_t yy = r;

    for (int32_t i = cx - r; i <= cx + r; i++) {
        OLED_DrawPixel(i, cy, on);
    }
    for (int32_t i = cy - r; i <= cy + r; i++) {
        OLED_DrawPixel(cx, i, on);
    }

    while (xx < yy) {
        if (f >= 0) {
            yy--;
            ddF_y -= 2;
            f += ddF_y;
        }
        xx++;
        ddF_x += 2;
        f += ddF_x + 1;

        for (int32_t i = cx - xx; i <= cx + xx; i++) {
            OLED_DrawPixel(i, cy + yy, on);
            OLED_DrawPixel(i, cy - yy, on);
        }
        for (int32_t i = cx - yy; i <= cx + yy; i++) {
            OLED_DrawPixel(i, cy + xx, on);
            OLED_DrawPixel(i, cy - xx, on);
        }
    }
}

void OLED_DrawImage(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *bmp, bool invert)
{
    if (!bmp) return;
    uint8_t hb = (h + 7) / 8;
    for (uint8_t n = 0; n < hb; n++) {
        for (uint8_t i = 0; i < w; i++) {
            uint8_t temp = bmp[n * w + i];
            for (uint8_t m = 0; m < 8; m++) {
                bool on = (temp & 0x01);
                if (invert) on = !on;
                OLED_DrawPixel(x + i, y + m + n * 8, on);
                temp >>= 1;
            }
        }
    }
}