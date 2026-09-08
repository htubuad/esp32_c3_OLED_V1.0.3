#ifndef __OLED_H
#define __OLED_H

#include <stdint.h>
#include <stdbool.h>

#define OLED_W      128
#define OLED_H       64
#define OLED_PAGES   8

#define OLED_I2C_ADDR  0x3C

#define OLED_SDA_GPIO   3
#define OLED_SCL_GPIO   2

#define OLED_CMD     0x00
#define OLED_DATA    0x40

typedef enum {
    OLED_STATE_INIT,
    OLED_STATE_IDLE,
    OLED_STATE_BUSY,
    OLED_STATE_DONE,
} oled_state_t;

typedef enum {
    OLED_SIZE_6x8  = 0,
    OLED_SIZE_12   = 12,
    OLED_SIZE_16   = 16,
    OLED_SIZE_24   = 24,
} oled_font_size_t;

void OLED_Init(void);
void OLED_Clear(void);
void OLED_Update(void);
oled_state_t OLED_GetState(void);

void OLED_DrawPixel(uint8_t x, uint8_t y, bool on);
void OLED_ClearArea(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

void OLED_ShowChar(uint8_t x, uint8_t y, char ch);
void OLED_ShowCharSize(uint8_t x, uint8_t y, char ch, oled_font_size_t size, bool invert);
void OLED_ShowString(uint8_t x, uint8_t y, const char *str);
void OLED_ShowStringSize(uint8_t x, uint8_t y, const char *str, oled_font_size_t size, bool invert);
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, oled_font_size_t size, bool invert);

void OLED_DrawLine(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2, bool on);
void OLED_DrawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);
void OLED_DrawCircle(uint8_t x, uint8_t y, uint8_t r, bool on);
void OLED_DrawFillCircle(uint8_t cx, uint8_t cy, uint8_t r, bool on);
void OLED_DrawImage(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t *bmp, bool invert);

#endif