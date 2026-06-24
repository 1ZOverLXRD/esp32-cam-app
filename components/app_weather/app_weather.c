#include "app_t.h"
#include "esp_log.h"

static const char *TAG = "APP_WEATHER";
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_temp_label = NULL;

static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    lv_obj_set_style_bg_color(s_page, lv_color_make(9, 132, 227), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);

    // 渐变效果（顶部浅蓝底部深蓝）
    lv_obj_set_style_bg_grad_color(s_page, lv_color_make(0, 50, 150), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(s_page, LV_GRAD_DIR_VER, LV_STATE_DEFAULT);

    lv_obj_t *icon = lv_label_create(s_page);
    lv_label_set_text(icon, "☀️");
    lv_obj_set_style_text_font(icon, lv_font_default(), LV_STATE_DEFAULT);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 15);

    s_temp_label = lv_label_create(s_page);
    lv_label_set_text(s_temp_label, "--°C");
    lv_obj_set_style_text_color(s_temp_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *detail = lv_label_create(s_page);
    lv_label_set_text(detail, "等待数据...");
    lv_obj_set_style_text_color(detail, lv_color_make(200, 220, 255), LV_STATE_DEFAULT);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, 30);

    ESP_LOGI(TAG, "Weather app created");
}

static void on_destroy(void)
{
    s_page = NULL;
    s_temp_label = NULL;
}

app_t app_weather = {
    .name = "天气",
    .icon = "🌤️",
    .color = 0x0984E3,
    .on_create = on_create,
    .on_destroy = on_destroy,
    .on_joystick = NULL,
    .page = NULL,
};