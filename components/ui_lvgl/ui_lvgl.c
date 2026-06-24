#include "ui_lvgl.h"
#include "lcd_st7789.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LVGL";
static lv_disp_t *s_disp = NULL;

/* 缓冲行数：双缓冲 20 行刚好占满 DMA 池 */
static lv_color_t *s_buf1 = NULL;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_handle_t panel = lcd_get_panel_handle();
    if (panel) {
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    }
    lv_disp_flush_ready(drv);
}

static void lvgl_tick_task(void *arg)
{
    while (1) {
        lv_tick_inc(10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started on core %d", xPortGetCoreID());
    while (1) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(5));  // 给IDLE任务留时间，防wdt
    }
}

esp_err_t ui_lvgl_init(void)
{
    lv_init();

    size_t buf_size = LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t);  // 全屏缓冲
    s_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);  // PSRAM

    if (!s_buf1) {
        ESP_LOGE(TAG, "Failed to allocate full-screen buffer");
        return ESP_ERR_NO_MEM;
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_buf1, NULL, LCD_WIDTH * LCD_HEIGHT);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1;  // 整帧刷新，消除淡入时带状闪烁
    s_disp = lv_disp_drv_register(&disp_drv);

    xTaskCreatePinnedToCore(lvgl_tick_task, "lv_tick", 2048, NULL, 1, NULL, 1);
    // lv_task 启动移到 ui_main_menu_init 之后（防止刷新访问未创建完的卡片）

    ESP_LOGI(TAG, "LVGL initialized: %dx%d, full_refresh PSRAM buffer",
                 LCD_WIDTH, LCD_HEIGHT);
        return ESP_OK;
}

lv_disp_t *ui_lvgl_get_display(void)
{
    return s_disp;
}

void ui_lvgl_start_task(void)
{
    xTaskCreatePinnedToCore(lvgl_task, "lv_task", 4096, NULL, 3, NULL, 0);
}