#include "app_t.h"
#include "esp_log.h"
#include "comms_server.h"
#include <string.h>

/* 引入中文字体 */
LV_FONT_DECLARE(cn_font_16);

static const char *TAG = "天气应用";
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_temp_label = NULL;
static lv_obj_t *s_city_label = NULL;
static lv_obj_t *s_cond_label = NULL;
static lv_obj_t *s_extra_label = NULL;
static lv_timer_t *s_weather_timer = NULL;
static int s_retry_count = 0;

/* 天气代码 → 中文描述 */
static const char *wcode_to_desc(int code)
{
    switch (code) {
        case 0:  return "晴";
        case 1:  return "多云";
        case 2:  return "阴";
        case 3:  return "小雨";
        case 4:  return "中雨";
        case 5:  return "大雨";
        case 6:  return "雷阵雨";
        case 7:  return "雪";
        case 8:  return "雾";
        case 9:  return "霾";
        case 51: return "强对流";
        default: return "未知";
    }
}

static void request_weather(void)
{
    s_retry_count++;
    extern volatile int g_android_connected;
    if (g_android_connected) {
        /* 空请求 — Android 自行决定查哪个城市（顶部的） */
        comms_server_send_packet(0x40, NULL, 0);
        ESP_LOGI(TAG, "天气请求已发送到 Android (第%d次)", s_retry_count);
        if (s_temp_label) lv_label_set_text(s_temp_label, "请求中...");
                if (s_cond_label) lv_label_set_text(s_cond_label, "等待回复");
        if (s_extra_label) lv_label_set_text(s_extra_label, "");
        if (s_weather_timer) {
            lv_timer_del(s_weather_timer);
            s_weather_timer = NULL;
        }
    } else {
        ESP_LOGW(TAG, "Android 未连接");
        if (s_cond_label) {
            char buf[48];
            snprintf(buf, sizeof(buf), "等待Android连接... (第%d次)", s_retry_count);
            lv_label_set_text(s_cond_label, buf);
        }
    }
}

static void weather_timer_cb(lv_timer_t *t)
{
    (void)t;
    request_weather();
}

static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    lv_obj_set_style_bg_color(s_page, lv_color_make(9, 132, 227), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(s_page, lv_color_make(0, 50, 150), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(s_page, LV_GRAD_DIR_VER, LV_STATE_DEFAULT);

    s_city_label = lv_label_create(s_page);
    lv_label_set_text(s_city_label, "天气");
    lv_obj_set_style_text_color(s_city_label, lv_color_make(200, 220, 255), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_city_label, &cn_font_16, LV_STATE_DEFAULT);
    lv_obj_align(s_city_label, LV_ALIGN_TOP_MID, 0, 6);

    s_temp_label = lv_label_create(s_page);
    lv_label_set_text(s_temp_label, "请求中...");
    lv_obj_set_style_text_color(s_temp_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_temp_label, &cn_font_16, LV_STATE_DEFAULT);
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, -30);

    s_cond_label = lv_label_create(s_page);
    lv_label_set_text(s_cond_label, "等待连接...");
    lv_obj_set_style_text_color(s_cond_label, lv_color_make(200, 220, 255), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_cond_label, &cn_font_16, LV_STATE_DEFAULT);
    lv_obj_align(s_cond_label, LV_ALIGN_CENTER, 0, 5);

    s_extra_label = lv_label_create(s_page);
    lv_label_set_text(s_extra_label, "");
    lv_obj_set_style_text_color(s_extra_label, lv_color_make(160, 180, 210), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_extra_label, &cn_font_16, LV_STATE_DEFAULT);
    lv_obj_align(s_extra_label, LV_ALIGN_CENTER, 0, 50);

    ESP_LOGI(TAG, "Weather app created");
    s_weather_timer = lv_timer_create(weather_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(s_weather_timer, 20);
}

void app_weather_update(const uint8_t *data, uint16_t len)
{
    if (!s_temp_label || len < 4) {
        ESP_LOGW(TAG, "app_weather_update: no label or short payload (len=%d)", len);
        return;
    }

    int16_t temp_raw = data[0] | (data[1] << 8);
    float temp_c = temp_raw / 100.0f;
    int hum = data[2];
    int wcode = data[3];

    /* 温度（大字） */
    char buf[20];
    snprintf(buf, sizeof(buf), "%.1f°C", temp_c);
    lv_label_set_text(s_temp_label, buf);

    /* 天气状况 + 湿度 */
            const char *desc = wcode_to_desc(wcode);
            char cond[32];
            snprintf(cond, sizeof(cond), "%s  |  湿度 %d%%", desc, hum);
            lv_label_set_text(s_cond_label, cond);

    /* 城市名（来自响应包，如果 len > 4） */
    if (len > 4 && data[4] != 0) {
        /* data[4+] 是 UTF-8 城市名（最多 20 字节） */
        int city_len = (len - 4 > 20) ? 20 : (len - 4);
        char city[24];
        memcpy(city, &data[4], city_len);
        city[city_len] = '\0';
        lv_label_set_text(s_city_label, city);
    }

    /* 额外信息 */
    /* 以后可以扩展：体感温度、风速、风向等 */
    lv_label_set_text(s_extra_label, "");

    ESP_LOGI(TAG, "Weather: %.1f°C %d%% code=%d (%s)", temp_c, hum, wcode, desc);

    if (s_weather_timer) {
        lv_timer_del(s_weather_timer);
        s_weather_timer = NULL;
    }
}

static void on_destroy(void)
{
    if (s_weather_timer) {
        lv_timer_del(s_weather_timer);
        s_weather_timer = NULL;
    }
    s_page = NULL;
    s_temp_label = NULL;
    s_city_label = NULL;
        s_cond_label = NULL;
    s_extra_label = NULL;
    s_retry_count = 0;
    ESP_LOGI(TAG, "天气应用已销毁");
}

app_t app_weather = {
    .name = "天气",
    .icon = "",
    .color = 0x0984E3,
    .on_create = on_create,
    .on_destroy = on_destroy,
    .on_joystick = NULL,
    .page = NULL,
};