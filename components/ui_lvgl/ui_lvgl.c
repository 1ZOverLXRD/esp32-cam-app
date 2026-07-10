#include "ui_lvgl.h"
#include "lcd_st7789.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "LVGL";
static lv_disp_t *s_disp = NULL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;  // 双核竞态防护

void ui_lvgl_lock(void)
{
    if (s_lvgl_mutex)
        xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
}

void ui_lvgl_unlock(void)
{
    if (s_lvgl_mutex)
        xSemaphoreGive(s_lvgl_mutex);
}

/* 双缓冲 20 行刚好占满 DMA 池，避免 SPI 传输等待渲染 */
#define BUF_ROWS 20
lv_color_t *s_buf1 = NULL;
lv_color_t *s_buf2 = NULL;

/* 引入中文字体 */
LV_FONT_DECLARE(cn_font_16);

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
        ui_lvgl_lock();
        lv_task_handler();
        ui_lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

esp_err_t ui_lvgl_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateMutex();
    assert(s_lvgl_mutex);

    lv_init();

    size_t buf_size = LCD_WIDTH * BUF_ROWS * sizeof(lv_color_t);  // 320×20×2 = 12.8KB
    s_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "Failed to allocate display buffers");
        if (s_buf1) free(s_buf1);
        if (s_buf2) free(s_buf2);
        return ESP_ERR_NO_MEM;
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, s_buf1, s_buf2, LCD_WIDTH * BUF_ROWS);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 0;  // 双缓冲部分刷新，消除 app 切换割裂
    s_disp = lv_disp_drv_register(&disp_drv);

    /* 用 SimHei 中文重新初始化主题 — 替换已烘焙的 font_small/normal/large 样式 */
    lv_theme_t *theme = lv_theme_default_init(s_disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_CYAN),
        LV_THEME_DEFAULT_DARK,
        &cn_font_16);
    if (theme) lv_disp_set_theme(s_disp, theme);

    /* 关键：强制屏幕基础样式继承中文字体
       LVGL 默认 label 不设 font → 查父级 → 查到 lv_scr_act() 上的 font → 生效 */
    lv_obj_set_style_text_font(lv_scr_act(), &cn_font_16, 0);

    xTaskCreatePinnedToCore(lvgl_tick_task, "lv_tick", 2048, NULL, 1, NULL, 1);

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