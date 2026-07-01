#include "app_trash.h"
#include "esp_log.h"
#include "comms_server.h"
#include "lvgl.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "cam_config.h"
#include "string.h"
#include "sys/socket.h"
#include "lwip/inet.h"

static const char *TAG = "APP_TRASH";

/* ── State ── */
typedef enum {
    TRASH_IDLE,
    TRASH_STREAMING,
    TRASH_FETCHING,
    TRASH_RESULT
} trash_state_t;

static trash_state_t s_state = TRASH_IDLE;
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_detail_label = NULL;
static lv_timer_t *s_ip_refresh_timer = NULL;
static lv_timer_t *s_wait_android_timer = NULL;
static int s_wait_timeout = 0;
static bool s_streaming = false;
static bool s_trash_mode_sent = false;  // 是否已发送 0x30 TrashMode
static int s_udp_fd = -1;
static TaskHandle_t s_stream_task = NULL;
static uint32_t s_frame_id = 0;

/* ── Externs ── */
extern volatile int g_android_connected;
extern uint32_t g_android_ip;
extern uint16_t g_android_port;
extern char s_sta_ip[16];

/* ── Camera config (from app_camera.c) ── */
static void apply_cam_config(void)
{
    cam_config_t cfg;
    cam_config_load(&cfg);
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGW(TAG, "sensor_get failed, cannot apply config");
        return;
    }

    s->set_hmirror(s, cfg.mirror);
    s->set_vflip(s, cfg.flip);
    s->set_exposure_ctrl(s, 1);
    s->set_aec_value(s, 300);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    if (cfg.brightness != 0) s->set_brightness(s, cfg.brightness);
    if (cfg.contrast != 0)   s->set_contrast(s, cfg.contrast);
    if (cfg.wb_mode != 0)    s->set_wb_mode(s, cfg.wb_mode);
    if (cfg.ae_level != 0)   s->set_ae_level(s, cfg.ae_level);
    ESP_LOGI(TAG, "Sensor config applied: qual=%u mir=%u flip=%u",
             cfg.quality, cfg.mirror, cfg.flip);
}

/* ── State → name map for logging ── */
static const char *state_name(trash_state_t st) {
    switch (st) {
        case TRASH_IDLE:      return "IDLE";
        case TRASH_STREAMING: return "STREAMING";
        case TRASH_FETCHING:  return "FETCHING";
        case TRASH_RESULT:    return "RESULT";
        default:              return "?";
    }
}

/* ── UDP stream task (from app_camera.c) ── */
static void stream_task(void *arg)
{
    (void)arg;
    uint8_t pkt[1500];

    while (1) {
        if (!g_android_connected) {
            ESP_LOGI(TAG, "Disconnected, stopping stream");
            /* 自清理：让 stop_streaming 不会再尝试删除已退出的任务 */
            s_stream_task = NULL;
            s_streaming = false;
            vTaskDelete(NULL);
            return;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(1); continue; }

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
            dest.sin_port = htons(g_android_port);
            dest.sin_addr.s_addr = g_android_ip;
            sendto(s_udp_fd, pkt, (int)(12 + chunk), 0,
                   (struct sockaddr *)&dest, sizeof(dest));

            frag_id++;
            offset += chunk;
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        s_frame_id++;
        esp_camera_fb_return(fb);
    }
}

/* ── Start / Stop streaming ── */
static void start_streaming(void)
{
    ESP_LOGI(TAG, "start_streaming: state=%s streaming=%d",
             state_name(s_state), s_streaming);
    /* 防止竞态：立即设标志，不等相机初始化完 */
    if (s_streaming) {
        ESP_LOGW(TAG, "start_streaming skipped — already streaming");
        return;
    }
    s_streaming = true;
    s_state = TRASH_STREAMING;

    camera_config_t cfg = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 15,
        .pin_sscb_sda = 4, .pin_sscb_scl = 5,
        .pin_d7 = 16, .pin_d6 = 17, .pin_d5 = 18, .pin_d4 = 12,
        .pin_d3 = 10, .pin_d2 = 8, .pin_d1 = 9, .pin_d0 = 11,
        .pin_vsync = 6, .pin_href = 7, .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_HD,
        .jpeg_quality = 15,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    cam_config_t cam_cfg;
    cam_config_load(&cam_cfg);
    cfg.jpeg_quality = cam_cfg.quality;
    cfg.frame_size = cam_cfg.resolution;

    if (esp_camera_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FAILED (will crash soon)");
        return;
    }
    apply_cam_config();

    s_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp_fd < 0) {
        ESP_LOGE(TAG, "UDP socket failed");
        esp_camera_deinit();
        return;
    }

    xTaskCreate(stream_task, "trash_stream", 4096, NULL, 4, &s_stream_task);
    s_frame_id = 0;
    ESP_LOGI(TAG, "Stream started");
}

static void stop_streaming(void)
{
    ESP_LOGI(TAG, "stop_streaming: state=%s streaming=%d",
             state_name(s_state), s_streaming);

    /* 停止流任务 + 关闭UDP + 卸载摄像头（不发网络包，避免lwip锁争用） */
    if (s_stream_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(s_stream_task);
        s_stream_task = NULL;
    }
    if (s_udp_fd >= 0) {
        close(s_udp_fd);
        s_udp_fd = -1;
    }
    esp_camera_deinit();
    s_streaming = false;
    ESP_LOGI(TAG, "Stream stopped");
}

/* ── Wait for Android then send TrashMode and start stream ── */
static void try_start_stream(lv_timer_t *t)
{
    (void)t;
    if (!g_android_connected) {
        s_wait_timeout++;
        if (s_wait_timeout >= 3000) {  // 10 分钟超时后停止
            ESP_LOGW(TAG, "Android connect timeout (10min)");
            if (s_wait_android_timer) {
                lv_timer_del(s_wait_android_timer);
                s_wait_android_timer = NULL;
            }
            return;
        }
        return;
    }

    /* Android 已连接 → 立即发送 TrashMode，不等 StreamStartUdp */
    if (!s_trash_mode_sent) {
        comms_server_send_packet(0x30, NULL, 0);
        s_trash_mode_sent = true;
        ESP_LOGI(TAG, "TrashMode sent to Android");
        if (s_status_label)
            lv_label_set_text(s_status_label, "Trash mode sent, waiting for UDP...");
    }

    /* 等 Android 发来 StreamStartUdp 拿到端口后再开相机推流 */
    if (g_android_port == 0 || g_android_ip == 0) return;

    /* 防止与 on_trash_cmd(0x20) 竞态 — 如果已经在推流则跳过 */
    if (s_streaming) {
        if (s_wait_android_timer) {
            lv_timer_del(s_wait_android_timer);
            s_wait_android_timer = NULL;
        }
        return;
    }

    if (s_wait_android_timer) {
        lv_timer_del(s_wait_android_timer);
        s_wait_android_timer = NULL;
    }

    start_streaming();
    if (s_status_label)
        lv_label_set_text(s_status_label, "Ready for capture");
    ESP_LOGI(TAG, "try_start_stream done -> STREAMING");
}

/* ── Command callback (comms_server 0x20-0x4F routing) ── */
static void on_trash_cmd(uint8_t cmd, uint16_t seq,
                          const uint8_t *payload, uint32_t plen)
{
    (void)seq;

    /* 如果定时器已过期（60秒无连接），Android 连上后补发 TrashMode */
    if (!s_trash_mode_sent) {
        comms_server_send_packet(0x30, NULL, 0);
        s_trash_mode_sent = true;
        ESP_LOGI(TAG, "TrashMode sent (late, on first cmd 0x%02X)", cmd);
    }

    switch (cmd) {
    case 0x20: /* StreamStartUdp from Android — cancel crop or initial start */
        ESP_LOGI(TAG, "CMD 0x20: state=%s streaming=%d",
                 state_name(s_state), s_streaming);
        if (s_state != TRASH_STREAMING) {
            start_streaming();
            if (s_status_label)
                lv_label_set_text(s_status_label, "Ready for capture");
        }
        break;

    case 0x21: /* StreamStopUdp from Android — "识别" pressed */
        ESP_LOGI(TAG, "CMD 0x21: state=%s streaming=%d",
                 state_name(s_state), s_streaming);
        stop_streaming();
        s_state = TRASH_FETCHING;
        if (s_status_label)
            lv_label_set_text(s_status_label, "Fetching...");
        if (s_detail_label)
            lv_label_set_text(s_detail_label, "");
        break;

    case 0x32: { /* TrashResult */
        if (plen < 1) break;
        uint8_t count = payload[0];
        ESP_LOGI(TAG, "CMD 0x32 TrashResult: count=%d", count);
        s_state = TRASH_RESULT;

        if (count == 0) {
            if (s_status_label)
                lv_label_set_text(s_status_label, "Inference failed");
            if (s_detail_label)
                lv_label_set_text(s_detail_label, "");
            break;
        }

        /* Build display text: "1. Label 92%\n2. Label 5%..." */
        char buf[256] = {0};
        int off = 1;
        for (int i = 0; i < count && i < 3; i++) {
            if (off + 1 >= plen) break;
            uint8_t label_len = payload[off++];
            if (off + label_len + 1 > plen) break;
            char label[64];
            memcpy(label, &payload[off], label_len);
            label[label_len] = '\0';
            off += label_len;
            uint8_t conf = payload[off++];

            char line[80];
            snprintf(line, sizeof(line), "%d. %s %d%%\n", i + 1, label, conf);
            strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        }

        if (s_status_label)
            lv_label_set_text(s_status_label, "Trash Detect");
        if (s_detail_label)
            lv_label_set_text(s_detail_label, buf);
        ESP_LOGI(TAG, "TrashResult displayed: %s", buf);
        break;
    }

    default:
        break;
    }
}

/* ── APP lifecycle ── */

/* 从 STA 网口获取 IP（同 Camera app 做法，不依赖 s_sta_ip 全局变量） */
static const char *get_sta_ip_str(void)
{
    static char buf[16] = "0.0.0.0";
    esp_netif_t *sta = NULL;
    esp_netif_t *ap  = NULL;
    for (esp_netif_t *n = esp_netif_next(NULL); n; n = esp_netif_next(n)) {
        const char *key = esp_netif_get_ifkey(n);
        if (key && strstr(key, "STA"))  sta = n;
        if (key && strstr(key, "AP"))   ap  = n;
    }
    if (sta) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            return buf;
        }
    }
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

/* 每 2 秒刷新 IP 标签 */
static void ip_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (s_page) {
        lv_obj_t *ip = lv_obj_get_child(s_page, 0);
        if (ip) {
            const char *ip_str = get_sta_ip_str();
            ESP_LOGI(TAG, "IP refresh: %s", ip_str);
            lv_label_set_text(ip, ip_str);
        }
    }
}

static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    lv_obj_set_style_bg_color(s_page, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);

    comms_server_set_app_handler(on_trash_cmd);

    /* IP label (top) */
    lv_obj_t *ip = lv_label_create(s_page);
    lv_label_set_text(ip, get_sta_ip_str());
    lv_obj_set_style_text_color(ip, lv_color_make(100, 200, 255), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ip, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(ip, LV_ALIGN_TOP_MID, 0, 4);

    /* Status label (center) */
    s_status_label = lv_label_create(s_page);
    lv_label_set_text(s_status_label, "Connecting...");
    lv_obj_set_style_text_color(s_status_label, lv_color_make(200, 200, 200), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -20);

    /* Detail label (below center, multi-line for results) */
    s_detail_label = lv_label_create(s_page);
    lv_label_set_text(s_detail_label, "");
    lv_obj_set_style_text_color(s_detail_label, lv_color_make(220, 220, 255), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_detail_label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_align(s_detail_label, LV_ALIGN_CENTER, 0, 20);

    /* Hint at bottom */
    lv_obj_t *hint = lv_label_create(s_page);
    lv_label_set_text(hint, "Trash Detect");
    lv_obj_set_style_text_color(hint, lv_color_make(150, 150, 180), LV_STATE_DEFAULT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    s_state = TRASH_IDLE;
    s_wait_timeout = 0;
    s_trash_mode_sent = false;
    s_wait_android_timer = lv_timer_create(try_start_stream, 200, NULL);
    s_ip_refresh_timer = lv_timer_create(ip_refresh_cb, 2000, NULL);
    ESP_LOGI(TAG, "on_create done, IP=%s", get_sta_ip_str());
}

static void on_destroy(void)
{
    ESP_LOGI(TAG, "on_destroy: state=%s streaming=%d",
             state_name(s_state), s_streaming);
    /* 先发 AppExit 通知 Android（此时流任务还在，但网络栈正常） */
    comms_server_send_packet(0xF0, NULL, 0);
    stop_streaming();
    if (s_wait_android_timer) {
        lv_timer_del(s_wait_android_timer);
        s_wait_android_timer = NULL;
    }
    if (s_ip_refresh_timer) {
        lv_timer_del(s_ip_refresh_timer);
        s_ip_refresh_timer = NULL;
    }
    s_trash_mode_sent = false;
    comms_server_set_app_handler(NULL);
    s_page = NULL;
    s_status_label = NULL;
    s_detail_label = NULL;
    s_state = TRASH_IDLE;
}

static void on_joystick(joystick_evt_t evt)
{
    if (evt == JOY_EVT_PRESS && s_state == TRASH_RESULT) {
        ESP_LOGI(TAG, "PRESS → resume streaming");
        /* Resume streaming */
        start_streaming();
        comms_server_send_packet(0x31, NULL, 0); /* ResumeCapture → Android */
        if (s_status_label)
            lv_label_set_text(s_status_label, "Ready for capture");
        if (s_detail_label)
            lv_label_set_text(s_detail_label, "");
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
