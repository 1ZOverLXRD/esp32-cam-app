#include "app_t.h"
#include "esp_log.h"
#include "comms_server.h"

static const char *TAG = "APP_WEATHER";
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_temp_label = NULL;
static lv_timer_t *s_weather_timer = NULL;

/* 定时器回调：向 Android 请求天气数据 */
static void weather_timer_cb(lv_timer_t *t)
{
    (void)t;
    extern volatile int g_android_connected;
    if (g_android_connected) {
        comms_server_send_packet(0x40, NULL, 0);
        ESP_LOGI(TAG, "WeatherRequest 0x40 sent to Android");
        /* 发了一次就停定时器，等收到数据再开或超时后再发 */
        if (s_weather_timer) {
            lv_timer_del(s_weather_timer);
            s_weather_timer = NULL;
        }
    } else {
        ESP_LOGW(TAG, "Android NOT connected (g_android_connected=0), will retry next tick");
    }
}

static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    lv_obj_set_style_bg_color(s_page, lv_color_make(9, 132, 227), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);

    lv_obj_set_style_bg_grad_color(s_page, lv_color_make(0, 50, 150), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(s_page, LV_GRAD_DIR_VER, LV_STATE_DEFAULT);

    lv_obj_t *icon = lv_label_create(s_page);
    lv_label_set_text(icon, "\u2600\uFE0F");
    lv_obj_set_style_text_font(icon, lv_font_default(), LV_STATE_DEFAULT);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 15);

    s_temp_label = lv_label_create(s_page);
    lv_label_set_text(s_temp_label, "--\u00B0C");
    lv_obj_set_style_text_color(s_temp_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *detail = lv_label_create(s_page);
    lv_label_set_text(detail, "\u7B49\u5F85\u6570\u636E...");
    lv_obj_set_style_text_color(detail, lv_color_make(200, 220, 255), LV_STATE_DEFAULT);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, 30);

    ESP_LOGI(TAG, "Weather app created, starting retry timer");

    /* 每 3 秒重试一次请求数据，最多 20 次（60 秒） */
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
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f\u00B0C  %d%%", temp_c, hum);
    lv_label_set_text(s_temp_label, buf);
    ESP_LOGI(TAG, "Weather updated: %.1f\u00B0C %d%% code=%d", temp_c, hum, wcode);

    /* 收到数据后停止重试定时器 */
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
    ESP_LOGI(TAG, "Weather app destroyed");
}

app_t app_weather = {
    .name = "\u5929\u6C14",
    .icon = "\xE2\x98\x80\xEF\xB8\x8F",  // ☀️
    .color = 0x0984E3,
    .on_create = on_create,
    .on_destroy = on_destroy,
    .on_joystick = NULL,
    .page = NULL,
};
