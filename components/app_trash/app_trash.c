#include "app_trash.h"
#include "esp_log.h"
#include "comms_server.h"
#include "lvgl.h"

static const char *TAG = "APP_TRASH";
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_status_label = NULL;  // 主状态文本
static bool s_streaming = false;

/* ─── 推流握手复用 Camera 模式 ─── */
static void start_streaming(void)
{
    // TODO: Task 3 implements
    s_streaming = true;
}

static void stop_streaming(void)
{
    // TODO: Task 3 implements
    s_streaming = false;
}

/* ─── 命令回调（comms_server 0x30-0x4F 路由） ─── */
static void on_trash_cmd(uint8_t cmd, uint16_t seq,
                          const uint8_t *payload, uint32_t plen)
{
    switch (cmd) {
    case 0x32: { // TrashResult from Android
        // TODO: Task 3 — parse and display on TFT
        if (plen > 0) {
            uint8_t count = payload[0];
            ESP_LOGI(TAG, "TrashResult: %d candidates", count);
        }
        break;
    }
    default:
        break;
    }
}

/* ─── APP 生命周期 ─── */
static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    lv_obj_set_style_bg_color(s_page, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);

    // 注册命令回调
    comms_server_set_app_handler(on_trash_cmd);

    // 等待 Android 连接（同 Camera 流程）
    s_status_label = lv_label_create(s_page);
    lv_label_set_text(s_status_label, "Connect Android...");
    lv_obj_set_style_text_color(s_status_label, lv_color_make(200, 200, 100), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);

    // 显示 STA IP
    extern char s_sta_ip[16];
    lv_obj_t *ip_label = lv_label_create(s_page);
    lv_label_set_text(ip_label, s_sta_ip);
    lv_obj_set_style_text_color(ip_label, lv_color_make(100, 200, 255), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ip_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(ip_label, LV_ALIGN_TOP_MID, 0, 4);

    // 同 Camera 的 try_start_stream 逻辑等待 Android 连接
    // TODO: Task 3 — start streaming when g_android_connected && g_android_port
}

static void on_destroy(void)
{
    stop_streaming();
    comms_server_set_app_handler(NULL);
    s_page = NULL;
    s_status_label = NULL;
}

static void on_joystick(joystick_evt_t evt)
{
    if (evt == JOY_EVT_PRESS) {
        if (/* TODO: Task 3 — in result state */ false) {
            // 恢复推流
            start_streaming();
        }
    }
}

app_t app_trash = {
    .name = "Trash Detect",
    .icon = "T",
    .color = 0xE17055,
    .on_create = on_create,
    .on_destroy = on_destroy,
    .on_joystick = on_joystick,
};