#include "app_trash.h"
#include "esp_log.h"
#include "comms_server.h"
#include "lvgl.h"
#include "esp_camera.h"
#include "esp_timer.h"
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

/* ── UDP stream task (from app_camera.c) ── */
static void stream_task(void *arg)
{
    (void)arg;
    uint8_t pkt[1500];

    while (1) {
        if (!g_android_connected) {
            ESP_LOGI(TAG, "Disconnected, stopping stream");
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
        ESP_LOGE(TAG, "Camera init failed");
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
    s_streaming = true;
    ESP_LOGI(TAG, "Stream started");
}

static void stop_streaming(void)
{
    if (s_stream_task) {
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

/* ── Wait for Android then start stream ── */
static void try_start_stream(lv_timer_t *t)
{
    (void)t;
    if (!g_android_connected) {
        s_wait_timeout++;
        if (s_wait_timeout >= 300) {
            ESP_LOGW(TAG, "Android connect timeout");
            if (s_wait_android_timer) {
                lv_timer_del(s_wait_android_timer);
                s_wait_android_timer = NULL;
            }
            /* Exit app — ui_main handles close via launcher */
            return;
        }
        return;
    }
    if (g_android_port == 0 || g_android_ip == 0) return;

    if (s_wait_android_timer) {
        lv_timer_del(s_wait_android_timer);
        s_wait_android_timer = NULL;
    }

    start_streaming();
    comms_server_send_packet(0x30, NULL, 0); /* TrashMode → Android */
    s_state = TRASH_STREAMING;
    if (s_status_label)
        lv_label_set_text(s_status_label, "Ready for capture");
}

/* ── Command callback (comms_server 0x20-0x4F routing) ── */
static void on_trash_cmd(uint8_t cmd, uint16_t seq,
                          const uint8_t *payload, uint32_t plen)
{
    (void)seq;

    switch (cmd) {
    case 0x20: /* StreamStartUdp from Android — cancel crop or initial start */
        if (s_state != TRASH_STREAMING) {
            start_streaming();
            s_state = TRASH_STREAMING;
            if (s_status_label)
                lv_label_set_text(s_status_label, "Ready for capture");
        }
        break;

    case 0x21: /* StreamStopUdp from Android — "识别" pressed */
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
static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    lv_obj_set_style_bg_color(s_page, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);

    comms_server_set_app_handler(on_trash_cmd);

    /* IP label (top) */
    lv_obj_t *ip = lv_label_create(s_page);
    lv_label_set_text(ip, s_sta_ip);
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
    s_wait_android_timer = lv_timer_create(try_start_stream, 200, NULL);
}

static void on_destroy(void)
{
    stop_streaming();
    if (s_wait_android_timer) {
        lv_timer_del(s_wait_android_timer);
        s_wait_android_timer = NULL;
    }
    if (s_ip_refresh_timer) {
        lv_timer_del(s_ip_refresh_timer);
        s_ip_refresh_timer = NULL;
    }
    comms_server_set_app_handler(NULL);
    s_page = NULL;
    s_status_label = NULL;
    s_detail_label = NULL;
    s_state = TRASH_IDLE;
}

static void on_joystick(joystick_evt_t evt)
{
    if (evt == JOY_EVT_PRESS && s_state == TRASH_RESULT) {
        /* Resume streaming */
        start_streaming();
        comms_server_send_packet(0x31, NULL, 0); /* ResumeCapture → Android */
        s_state = TRASH_STREAMING;
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
