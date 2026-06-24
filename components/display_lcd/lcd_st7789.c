#include "lcd_st7789.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "LCD";
esp_lcd_panel_handle_t s_panel = NULL;

esp_err_t lcd_st7789_init(void)
{
#if PIN_LCD_BL >= 0
    // 背光初始化
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_conf);
    gpio_set_level(PIN_LCD_BL, 1);
#endif  // 背光亮

    ESP_LOGI(TAG, "Init SPI bus: SCLK=%d, MOSI=%d", PIN_LCD_SCLK, PIN_LCD_MOSI);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t) / 6, // 全帧分6次发，防DMA池溢出
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO: DC=%d, CS=%d", PIN_LCD_DC, PIN_LCD_CS);

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 60 * 1000 * 1000,       // 60MHz（稳定）
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 3,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = 42,           // RST=GPIO42，软件控制复位
        .rgb_ele_order = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // 屏幕复位后默认黑色，跳过首次全屏填充（NULL buffer 会导致 SPI 崩溃）
    ESP_LOGI(TAG, "ST7789 240x240 initialized");
    return ESP_OK;
}

void *lcd_get_panel_handle(void)
{
    return s_panel;
}