#include "app_camera.h"
#include "ui_main.h"
#include "ui_lvgl.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "string.h"
#include "sys/socket.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

static const char *TAG = "APP_CAMERA";

extern volatile bool s_app_handled;

static lv_obj_t *s_page = NULL;
static lv_obj_t *s_hint = NULL; static lv_obj_t *s_ip_label = NULL;
static lv_obj_t *s_stream_btn = NULL;
static lv_obj_t *s_tft_btn = NULL;

#include "esp_netif.h"

/* 只返回 STA/ETH 接口的 IP，跳过 AP（DHCP 服务器模式）避免残留 192.168.4.1 */
static const char *get_sta_ip_str(void)
{
    static char buf[16] = "0.0.0.0";
    esp_netif_t *sta = NULL;
    esp_netif_t *ap  = NULL;
    /* 先分类网口（if_key 全大写：WIFI_STA_DEF / WIFI_AP_DEF） */
    for (esp_netif_t *n = esp_netif_next(NULL); n; n = esp_netif_next(n)) {
        const char *key = esp_netif_get_ifkey(n);
        if (key && strstr(key, "STA"))  sta = n;
        if (key && strstr(key, "AP"))   ap  = n;
    }
    /* 优先返回 STA IP */
    if (sta) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            return buf;
        }
    }
    /* STA 没有 IP，尝试其他非 AP 网口 */
    for (esp_netif_t *n = esp_netif_next(NULL); n; n = esp_netif_next(n)) {
        if (n == ap) continue;
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            break;
        }
    }
    return buf;
}

/* 每 2 秒刷新 IP 标签（STA 连接后自动更新） */
static void ip_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ip_label) {
        lv_label_set_text(s_ip_label, get_sta_ip_str());
    }
}

static lv_timer_t *s_cam_timer = NULL;
static lv_timer_t *s_ip_refresh_timer = NULL;
static bool s_streaming = false;
static bool s_tft_mode = false;

static int s_udp_fd = -1;
static uint32_t s_android_ip = 0;
static uint16_t s_android_port = 0;
static TaskHandle_t s_stream_task = NULL;
static uint32_t s_frame_id = 0;

/* 焦点与去抖 */
static int s_focus_idx = 0;          // 0=stream, 1=tft
static bool s_skip_press = false;     // LONG_PRESS 退出后跳过首次 PRESS

#define FOCUS_STREAM 0
#define FOCUS_TFT    1

/* ===================== 焦点样式 ===================== */

static void update_focus_style(void)
{
    lv_color_t stream_bg = (s_focus_idx == FOCUS_STREAM) ? lv_color_make(80, 80, 180) : lv_color_make(50, 50, 120);
    lv_color_t tft_bg    = (s_focus_idx == FOCUS_TFT)    ? lv_color_make(50, 50, 100) : lv_color_make(30, 30, 55);

    if (s_stream_btn) {
        lv_obj_set_style_bg_color(s_stream_btn, stream_bg, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_stream_btn, (s_focus_idx == FOCUS_STREAM) ? 3 : 0, LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_stream_btn, lv_color_white(), LV_STATE_DEFAULT);
    }
    if (s_tft_btn) {
        lv_obj_set_style_bg_color(s_tft_btn, tft_bg, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_tft_btn, (s_focus_idx == FOCUS_TFT) ? 3 : 0, LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_tft_btn, lv_color_white(), LV_STATE_DEFAULT);
    }
}

/* ===================== 摄像头初始化 ===================== */

static esp_err_t init_camera(framesize_t size, pixformat_t fmt)
{
    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 15,
        .pin_sscb_sda = 4,
        .pin_sscb_scl = 5,
        .pin_d7 = 16,
        .pin_d6 = 17,
        .pin_d5 = 18,
        .pin_d4 = 12,
        .pin_d3 = 10,
        .pin_d2 = 8,
        .pin_d1 = 9,
        .pin_d0 = 11,
        .pin_vsync = 6,
        .pin_href = 7,
        .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = fmt,
        .frame_size = size,
        .jpeg_quality = 10,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    esp_err_t ret = esp_camera_init(&cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera init OK: size=%dx%d fmt=%d",
                 (size == FRAMESIZE_HD) ? 1280 : 320,
                 (size == FRAMESIZE_HD) ? 720  : 240,
                 fmt);
    } else {
        ESP_LOGE(TAG, "Camera init FAILED: %d", ret);
    }
    return ret;
}

/* ===================== TFT 模式定时器回调 ===================== */

static void cam_timer_cb(lv_timer_t *t)
{
    (void)t;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;

    if (s_buf1 && fb->len >= 320 * 240 * 2) {
        for (int y = 0; y < 240; y++) {
            memcpy((uint8_t *)s_buf1 + y * 240 * 2,
                   fb->buf + (y * 320 + 40) * 2,
                   240 * 2);
        }
    }
    esp_camera_fb_return(fb);
}

/* ===================== UDP 推流任务 ===================== */

static void stream_task(void *arg)
{
    (void)arg;
    uint8_t pkt[1500];
    bool first_frame = true;

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(1); continue; }

        if (first_frame) {
            ESP_LOGI(TAG, "First frame sent: %dx%d, %d bytes, frag=%d",
                     fb->width, fb->height, fb->len,
                     (int)((fb->len + 1388 - 1) / 1388));
            first_frame = false;
        }

        uint8_t *jpeg = fb->buf;
        size_t jpeg_len = fb->len;
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);

        int frag_total = (int)((jpeg_len + 1388 - 1) / 1388);
        int frag_id = 0;
        size_t offset = 0;

        while (offset < jpeg_len) {
            size_t chunk = jpeg_len - offset;
            if (chunk > 1388) chunk = 1388;

            pkt[0]  = s_frame_id & 0xFF;
            pkt[1]  = (s_frame_id >> 8) & 0xFF;
            pkt[2]  = (s_frame_id >> 16) & 0xFF;
            pkt[3]  = (s_frame_id >> 24) & 0xFF;
            pkt[4]  = ts & 0xFF;
            pkt[5]  = (ts >> 8) & 0xFF;
            pkt[6]  = (ts >> 16) & 0xFF;
            pkt[7]  = (ts >> 24) & 0xFF;
            pkt[8]  = frag_id & 0xFF;
            pkt[9]  = (frag_id >> 8) & 0xFF;
            pkt[10] = frag_total & 0xFF;
            pkt[11] = (frag_total >> 8) & 0xFF;

            memcpy(pkt + 12, jpeg + offset, chunk);

            struct sockaddr_in dest;
            dest.sin_family = AF_INET;
            dest.sin_port = htons(s_android_port);
            dest.sin_addr.s_addr = s_android_ip;
            sendto(s_udp_fd, pkt, (int)(12 + chunk), 0,
                   (struct sockaddr *)&dest, sizeof(dest));

            frag_id++;
            offset += chunk;
        }

        s_frame_id++;
        esp_camera_fb_return(fb);
    }
}

/* ===================== 进入推流 / TFT 模式 ===================== */

static void enter_stream_mode(void)
{
    ESP_LOGI(TAG, "enter_stream_mode: ip=%s port=%u",
             inet_ntoa(*((struct in_addr *)&s_android_ip)), s_android_port);

    /* 清空选择界面（含 Camera 标题），避免重叠 */
    lv_obj_clean(s_page);
    s_hint = NULL;
    s_stream_btn = s_tft_btn = NULL;

    if (init_camera(FRAMESIZE_HD, PIXFORMAT_JPEG) != ESP_OK) {
        s_hint = lv_label_create(s_page);
        lv_label_set_text(s_hint, "Camera init failed");
        lv_obj_set_style_text_color(s_hint, lv_color_make(255, 80, 80), LV_STATE_DEFAULT);
        lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -16);
        ESP_LOGE(TAG, "Camera init failed");
        return;
    }

    s_streaming = true;
    s_tft_mode  = false;

    /* 创建底部提示 */
    s_hint = lv_label_create(s_page);
    lv_label_set_text(s_hint, "Long PRESS exit");
    lv_obj_set_style_text_color(s_hint, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    /* 创建 IP 标签（顶部居中，y=0 无标题遮挡） */
    if (s_ip_label) { lv_obj_del(s_ip_label); s_ip_label = NULL; }
    s_ip_label = lv_label_create(s_page);
    lv_label_set_text(s_ip_label, get_sta_ip_str());
    lv_obj_set_style_text_color(s_ip_label, lv_color_make(100, 200, 255), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_ip_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_width(s_ip_label, 220);
    lv_obj_align(s_ip_label, LV_ALIGN_TOP_MID, 0, 4);
    ESP_LOGI(TAG, "STA IP: %s", get_sta_ip_str());

    /* 每 2 秒刷新 IP（STA 连上热点获 DHCP IP 后自动更新） */
    s_ip_refresh_timer = lv_timer_create(ip_refresh_cb, 2000, NULL);

    s_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp_fd < 0) {
        ESP_LOGE(TAG, "UDP socket failed");
        if (s_hint) lv_label_set_text(s_hint, "UDP socket failed");
        return;
    }

    xTaskCreate(stream_task, "cam_stream", 4096, NULL, 4, &s_stream_task);
    ESP_LOGI(TAG, "Stream task started, frame_id=%u", s_frame_id);
}

static void enter_tft_mode(void)
{
    ESP_LOGI(TAG, "enter_tft_mode");

    if (s_stream_btn) { lv_obj_del(s_stream_btn); s_stream_btn = NULL; }
    if (s_tft_btn)   { lv_obj_del(s_tft_btn);   s_tft_btn   = NULL; }

    if (init_camera(FRAMESIZE_QVGA, PIXFORMAT_RGB565) != ESP_OK) {
        if (s_hint) lv_label_set_text(s_hint, "Camera init failed");
        return;
    }

    s_streaming = false;
    s_tft_mode  = true;

    if (s_hint) lv_label_set_text(s_hint, "TFT Display");
    s_cam_timer = lv_timer_create(cam_timer_cb, 50, NULL);
    ESP_LOGI(TAG, "TFT mode started");
}

/* ===================== 停止摄像头 ===================== */

static void stop_camera(void)
{
    ESP_LOGI(TAG, "stop_camera: streaming=%d tft=%d", s_streaming, s_tft_mode);

    if (s_cam_timer)  { lv_timer_del(s_cam_timer);  s_cam_timer  = NULL; }
    if (s_ip_refresh_timer) { lv_timer_del(s_ip_refresh_timer); s_ip_refresh_timer = NULL; }
    if (s_stream_task) { vTaskDelete(s_stream_task); s_stream_task = NULL; }
    if (s_udp_fd >= 0) { close(s_udp_fd); s_udp_fd = -1; }
    esp_camera_deinit();
    s_streaming = false;
    s_tft_mode  = false;
    s_frame_id  = 0;
    ESP_LOGI(TAG, "Camera stopped");
}

/* ===================== 重建模式选择 UI ===================== */

static void rebuild_mode_ui(void)
{
    lv_obj_clean(s_page);
    s_stream_btn = s_tft_btn = NULL;
    s_focus_idx = FOCUS_STREAM;

    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Camera");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* 推流按钮 */
    s_stream_btn = lv_obj_create(s_page);
    lv_obj_set_size(s_stream_btn, 200, 50);
    lv_obj_set_style_radius(s_stream_btn, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_stream_btn, lv_color_make(80, 80, 180), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_stream_btn, 3, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_stream_btn, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(s_stream_btn, LV_ALIGN_CENTER, 0, -40);
    lv_obj_clear_flag(s_stream_btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *slbl = lv_label_create(s_stream_btn);
    lv_label_set_text(slbl, "Stream to Android");
    lv_obj_center(slbl);

    /* TFT 按钮 */
    s_tft_btn = lv_obj_create(s_page);
    lv_obj_set_size(s_tft_btn, 200, 50);
    lv_obj_set_style_radius(s_tft_btn, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_tft_btn, lv_color_make(30, 30, 55), LV_STATE_DEFAULT);
    lv_obj_align(s_tft_btn, LV_ALIGN_CENTER, 0, 30);
    lv_obj_clear_flag(s_tft_btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tlbl = lv_label_create(s_tft_btn);
    lv_label_set_text(tlbl, "Local TFT Display");
    lv_obj_center(tlbl);

    s_hint = lv_label_create(s_page);
    char hint[64];
    snprintf(hint, sizeof(hint), "STA: %s | UP/DOWN select  PRESS confirm",
             s_sta_ip);
    lv_label_set_text(s_hint, hint);
    lv_obj_set_style_text_color(s_hint, lv_color_make(150, 150, 180), LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -16);
}

/* ===================== APP 生命周期 ===================== */



static void on_create(lv_obj_t *parent)
{
    s_page = parent;

    lv_obj_set_style_bg_color(s_page, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);

    s_streaming = false;
    s_tft_mode  = false;
    s_cam_timer = NULL;
    s_stream_btn = s_tft_btn = NULL;
    s_frame_id = 0;
    s_focus_idx = FOCUS_STREAM;
    s_skip_press = false;

    /* 确保 STA 网口的 DHCP 客户端在运行 */
    for (esp_netif_t *n = esp_netif_next(NULL); n; n = esp_netif_next(n)) {
        const char *key = esp_netif_get_ifkey(n);
        if (!key || !strstr(key, "STA")) continue;
        esp_netif_dhcp_status_t s;
        if (esp_netif_dhcpc_get_status(n, &s) == ESP_OK && s == ESP_NETIF_DHCP_STOPPED) {
            esp_netif_dhcpc_start(n);
            ESP_LOGI(TAG, "DHCP client started on %s", key);
        }
        break;
    }

    rebuild_mode_ui();
    ESP_LOGI(TAG, "Camera app created, STA IP: %s", get_sta_ip_str());
}

static void on_destroy(void)
{
    stop_camera();
    if (s_stream_btn) { lv_obj_del(s_stream_btn); s_stream_btn = NULL; }
    if (s_tft_btn)   { lv_obj_del(s_tft_btn);   s_tft_btn   = NULL; }
    s_hint = NULL;
    s_page = NULL;
    ESP_LOGI(TAG, "Camera app destroyed");
}

static void on_joystick(joystick_evt_t evt)
{
    if (!s_page) return;

    /* 推流 / TFT 模式下：不消费任何事件，退出由 ui_main 处理动画 */
    if (s_streaming || s_tft_mode) {
        return;
    }

    /* ── 模式选择界面 ── */
    switch (evt) {
        case JOY_EVT_UP:
            s_focus_idx = FOCUS_TFT;
            update_focus_style();
            ESP_LOGD(TAG, "focus -> TFT");
            break;
        case JOY_EVT_DOWN:
            s_focus_idx = FOCUS_STREAM;
            update_focus_style();
            ESP_LOGD(TAG, "focus -> Stream");
            break;
        case JOY_EVT_PRESS:
            if (s_focus_idx == FOCUS_STREAM) {
                ESP_LOGI(TAG, "Selected: Stream mode");
                enter_stream_mode();
            } else {
                ESP_LOGI(TAG, "Selected: TFT mode");
                enter_tft_mode();
            }
            break;
        default:
            break;
    }
}

app_t app_camera = {
    .name        = "Camera",
    .icon        = "C",
    .color       = 0x6C5CE7,
    .on_create   = on_create,
    .on_destroy  = on_destroy,
    .on_joystick = on_joystick,
    .page        = NULL,
};
