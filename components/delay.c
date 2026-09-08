#include "delay.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

void delay_ms(uint32_t ms)
{
    if (ms >= portTICK_PERIOD_MS) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        esp_rom_delay_us(ms * 1000);
    }
}