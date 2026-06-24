#ifndef LCD_ST7789_H
#define LCD_ST7789_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_WIDTH   240
#define LCD_HEIGHT  240

/* 用SD卡空闲引脚（用户的接线） */
#define PIN_LCD_SCLK   39      /* SD_CLK */
#define PIN_LCD_MOSI   40      /* SD_DATA */
#define PIN_LCD_DC     41      /* MTDI */
#define PIN_LCD_CS     38      /* SD_CMD */
#define PIN_LCD_RST    42      /* MTMS */
#define PIN_LCD_BL     -1      /* 3.3V 常亮 */

/**
 * @brief 初始化 ST7789 显示屏 (IDF LCD API)
 * @return ESP_OK 成功
 */
esp_err_t lcd_st7789_init(void);

/**
 * @brief 获取 LVGL 刷新回调所需的面板句柄
 */
void *lcd_get_panel_handle(void);

#ifdef __cplusplus
}
#endif

#endif