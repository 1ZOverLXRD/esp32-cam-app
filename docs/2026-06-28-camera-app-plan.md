# Camera APP + OV5640 推流 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task.

**Goal:** ESP32 Camera APP with two modes — 720p UDP streaming to Android, QVGA local TFT display.

**Architecture:** `app_camera` LVGL app + `esp32-camera` managed component. Streaming uses existing binary protocol (comms_server handles CMD), UDP push via separate task.

**Tech Stack:** ESP-IDF v5.5.2, esp32-camera (ESP Registry), LVGL v8.4, LWIP UDP sockets

**Build Verification:** Each task ends with `idf.py build`

## Global Constraints

- esp32-camera component version `^2.0` from ESP Registry
- UDP push task stack = 4096 bytes
- OV5640 QVGA 320×240 RGB565 for TFT mode
- OV5640 HD 1280×720 JPEG for streaming mode
- No NVS storage for camera config
- Camera deinit on LONG_PRESS, return to mode selection screen
- TFT center crop: 320×240 → 240×240 (horizontal -40px each side)

---

### Task 1: Add esp32-camera dependency & sdkconfig defaults

**Files:**
- Modify: `main/idf_component.yml`

- [ ] **Step 1: Add esp32-camera to idf_component.yml**

```yaml
## IDF Component Manager
dependencies:
  espressif/esp32-camera: "^2.0"
  ## Required IDF version
  idf:
    version: ">=5.0"
```

- [ ] **Step 2: Verify dependency resolves**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py reconfigure 2>&1 | grep -E "camera|Downloading"
```
Expected: esp32-camera component downloaded to managed_components/

- [ ] **Step 3: Commit**

```bash
git add main/idf_component.yml managed_components/
git commit -m "feat: add esp32-camera dependency"
```

---

### Task 2: Create app_camera component

**Files:**
- Create: `components/app_camera/app_camera.c`
- Create: `components/app_camera/app_camera.h`
- Create: `components/app_camera/CMakeLists.txt`

- [ ] **Step 1: Create CMakeLists.txt**

```cmake
idf_component_register(SRCS "app_camera.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl__lvgl ui_main esp32-camera)
```

- [ ] **Step 2: Create app_camera.h**

```c
#ifndef APP_CAMERA_H
#define APP_CAMERA_H

#include "app_t.h"

extern app_t app_camera;

#endif
```

- [ ] **Step 3: Create app_camera.c**

```c
#include "app_camera.h"
#include "ui_main.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "APP_CAMERA";
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_stream_btn = NULL;
static lv_obj_t *s_tft_btn = NULL;
static lv_obj_t *s_status = NULL;
static lv_timer_t *s_cam_timer = NULL;
static bool s_streaming = false;
static bool s_tft_mode = false;

static const char *TAG = "APP_CAMERA";

/* 摄像头初始化 */
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
    return esp_camera_init(&cfg);
}

/* TFT 显示模式定时器回调 */
static void cam_timer_cb(lv_timer_t *t)
{
    (void)t;
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;

    if (s_tft_mode) {
        /* QVGA 320×240 RGB565 → 居中裁剪为 240×240 → memcpy 到 PSRAM 缓冲 */
        extern lv_color_t *s_buf1;
        if (s_buf1 && fb->len >= 240 * 240 * 2) {
            for (int y = 0; y < 240; y++) {
                memcpy((uint8_t*)s_buf1 + y * 240 * 2,
                       fb->buf + (y * 320 + 40) * 2,
                       240 * 2);
            }
        }
    }
    esp_camera_fb_return(fb);
}

/* 启动推流（留给后续 Task） */
static void start_streaming(void)
{
    ESP_LOGI(TAG, "Streaming mode - stub");
}

/* 选择模式 */
static void enter_stream_mode(void)
{
    if (s_stream_btn) lv_obj_del(s_stream_btn);
    if (s_tft_btn) lv_obj_del(s_tft_btn);
    s_stream_btn = NULL;
    s_tft_btn = NULL;

    if (init_camera(FRAMESIZE_HD, PIXFORMAT_JPEG) != ESP_OK) {
        if (s_hint) lv_label_set_text(s_hint, "Camera init failed");
        return;
    }
    s_streaming = true;
    s_tft_mode = false;
    if (s_hint) lv_label_set_text(s_hint, "Streaming 720p...");
    start_streaming();
}

static void enter_tft_mode(void)
{
    if (s_stream_btn) lv_obj_del(s_stream_btn);
    if (s_tft_btn) lv_obj_del(s_tft_btn);
    s_stream_btn = NULL;
    s_tft_btn = NULL;

    if (init_camera(FRAMESIZE_QVGA, PIXFORMAT_RGB565) != ESP_OK) {
        if (s_hint) lv_label_set_text(s_hint, "Camera init failed");
        return;
    }
    s_streaming = false;
    s_tft_mode = true;
    if (s_hint) lv_label_set_text(s_hint, "TFT Display");
    s_cam_timer = lv_timer_create(cam_timer_cb, 50, NULL);
}

/* 停止摄像头 */
static void stop_camera(void)
{
    if (s_cam_timer) { lv_timer_del(s_cam_timer); s_cam_timer = NULL; }
    esp_camera_deinit();
    s_streaming = false;
    s_tft_mode = false;
}

/* ── APP 生命周期 ── */

static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    s_streaming = false;
    s_tft_mode = false;
    s_cam_timer = NULL;
    s_stream_btn = s_tft_btn = NULL;

    lv_obj_set_style_bg_color(s_page, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Camera");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* 模式选择按钮 */
    s_stream_btn = lv_obj_create(s_page);
    lv_obj_set_size(s_stream_btn, 200, 50);
    lv_obj_set_style_radius(s_stream_btn, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_stream_btn, lv_color_make(60, 60, 140), LV_STATE_DEFAULT);
    lv_obj_align(s_stream_btn, LV_ALIGN_CENTER, 0, -40);
    lv_obj_clear_flag(s_stream_btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *slbl = lv_label_create(s_stream_btn);
    lv_label_set_text(slbl, "Stream to Android");
    lv_obj_center(slbl);

    s_tft_btn = lv_obj_create(s_page);
    lv_obj_set_size(s_tft_btn, 200, 50);
    lv_obj_set_style_radius(s_tft_btn, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_tft_btn, lv_color_make(35, 35, 60), LV_STATE_DEFAULT);
    lv_obj_align(s_tft_btn, LV_ALIGN_CENTER, 0, 30);
    lv_obj_clear_flag(s_tft_btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tlbl = lv_label_create(s_tft_btn);
    lv_label_set_text(tlbl, "Local TFT Display");
    lv_obj_center(tlbl);

    /* 提示 / 状态 */
    s_hint = lv_label_create(s_page);
    lv_label_set_text(s_hint, "PRESS to select mode");
    lv_obj_set_style_text_color(s_hint, lv_color_make(150, 150, 180), LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -16);
}

static void on_destroy(void)
{
    stop_camera();
    s_page = NULL;
}

static void on_joystick(joystick_evt_t evt)
{
    if (!s_page) return;

    /* 正在推流/显示中: 仅 LONG_PRESS 退出 */
    if (s_streaming || s_tft_mode) {
        if (evt == JOY_EVT_LONG_PRESS) {
            stop_camera();
            /* 回到模式选择 = 关闭当前 APP 并重开 */
            /* ui_main 的退出流程会触发 on_destroy */
        }
        return;
    }

    /* 模式选择界面: PRESS 选模式 */
    if (evt == JOY_EVT_PRESS) {
        /* 简单逻辑: 固定选 Streaming（优先级高），后续加焦点切换 */
        enter_stream_mode();
    }
}

app_t app_camera = {
    .name = "Camera",
    .icon = "C",
    .color = 0x0984E3,
    .on_create = on_create,
    .on_destroy = on_destroy,
    .on_joystick = on_joystick,
};
```

- [ ] **Step 4: Verify compilation**

```bash
cd /d/Project/ESP32/esp32-cam-app && cmd.exe //c "F:\Cache\Hermes\scripts\build_check.bat" 2>&1 | tail -5
```
Expected: BUILD_EXIT:0

- [ ] **Step 5: Commit**

```bash
git add components/app_camera/ && git commit -m "feat: camera app with mode selection (stream/TFT stub)"
```

---

### Task 3: Register app_camera in main.c

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Register app_camera**

Add at the extern declarations area:
```c
extern app_t app_camera;
```

Add in `open_current_app()`:
```c
if (real_idx == 0) s_active_app = &app_settings;
else if (real_idx == 1) s_active_app = &app_trash;
else if (real_idx == 2) s_active_app = &app_weather;
else if (real_idx == 3) s_active_app = &app_camera;
```

Wait — the current app registration uses `if (real_idx == 0)`. With 4 apps (Settings, Trash, Weather, Camera), I need to check the current registration.

Let me read the current registration code first.

Actually, looking at the earlier conversation, the main menu has 4 apps by default. `app_t` instances are registered via `extern` and the `real_idx` check in `open_current_app`. The APP_COUNT is 4. The current registration is:

```c
if (real_idx == 0) s_active_app = &app_settings;
```

I need to add camera at the right index. Since Camera is a new app, I need to decide where it goes in the carousel. Let me add it as index 3 (after Settings=0, Trash=1, Weather=2).

- [ ] **Step 2: Verify compilation and commit**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -5
git add main/main.c && git commit -m "feat: register camera app"
```

---

### Task 4: Implement UDP streaming task

**Files:**
- Modify: `components/app_camera/app_camera.c`

- [ ] **Step 1: Add UDP streaming implementation**

Replace the `start_streaming()` stub with actual code:
```c
static int s_udp_fd = -1;
static uint32_t s_android_ip = 0;
static uint16_t s_android_port = 0;
static TaskHandle_t s_stream_task = NULL;

static void stream_task(void *arg)
{
    (void)arg;
    uint32_t frame_id = 0;
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(1); continue; }

        uint8_t *jpeg = fb->buf;
        size_t jpeg_len = fb->len;
        uint32_t ts = esp_timer_get_time() / 1000;

        /* 分片发送 */
        int frag_total = (jpeg_len + 1388 - 1) / 1388;  // 1388 = 1400 - 12 header
        int frag_id = 0;
        size_t offset = 0;

        while (offset < jpeg_len) {
            uint8_t pkt[1500];
            size_t chunk = (jpeg_len - offset > 1388) ? 1388 : (jpeg_len - offset);

            /* UDP 12 字节头 */
            pkt[0] = frame_id & 0xFF;  pkt[1] = (frame_id >> 8) & 0xFF;
            pkt[2] = (frame_id >> 16) & 0xFF; pkt[3] = (frame_id >> 24) & 0xFF;
            pkt[4] = ts & 0xFF;        pkt[5] = (ts >> 8) & 0xFF;
            pkt[6] = (ts >> 16) & 0xFF; pkt[7] = (ts >> 24) & 0xFF;
            pkt[8] = frag_id & 0xFF;   pkt[9] = (frag_id >> 8) & 0xFF;
            pkt[10] = frag_total & 0xFF; pkt[11] = (frag_total >> 8) & 0xFF;

            memcpy(pkt + 12, jpeg + offset, chunk);

            struct sockaddr_in dest;
            dest.sin_family = AF_INET;
            dest.sin_port = htons(s_android_port);
            dest.sin_addr.s_addr = s_android_ip;
            sendto(s_udp_fd, pkt, 12 + chunk, 0,
                   (struct sockaddr*)&dest, sizeof(dest));

            frag_id++;
            offset += chunk;
        }
        frame_id++;
        esp_camera_fb_return(fb);
    }
}

static void start_streaming(void)
{
    s_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_udp_fd < 0) {
        ESP_LOGE(TAG, "UDP socket failed");
        return;
    }
    /* TODO: get Android IP from comms_server */
    s_android_ip = inet_addr("192.168.4.2");  // placeholder
    s_android_port = 9001;  // placeholder — from StreamStartUdp cmd

    xTaskCreate(stream_task, "cam_stream", 4096, NULL, 4, &s_stream_task);
}
```

- [ ] **Step 2: Verify compilation**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -5
```

- [ ] **Step 3: Commit**

```bash
git add components/app_camera/app_camera.c && git commit -m "feat: UDP streaming task with fragment sender"
```

---

### Task 5: Wire comms_server to pass Android IP/port to camera

**Files:**
- Modify: `components/comms_server/comms_server.c`
- Modify: `components/app_camera/app_camera.c`

This task connects the binary protocol's `StreamStartUdp` response to the actual UDP streaming. The comms_server receives the Android port from the CMD=0x20 packet and needs to save it along with the client IP for the camera to use.

- [ ] **Step 1: Add global variables in comms_server.c**

```c
/* Camera streaming params (set by StreamStartUdp handler, read by app_camera) */
uint32_t g_android_ip = 0;
uint16_t g_android_port = 0;
```

- [ ] **Step 2: Update StreamStartUdp handler in comms_server.c**

In `handle_packet`, case `0x20`:
```c
case 0x20: {
    if (plen >= 2) {
        uint16_t android_port = payload[0] | (payload[1] << 8);
        /* 保存 Android IP 和端口 */
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        getpeername(fd, (struct sockaddr*)&client, &client_len);
        g_android_ip = client.sin_addr.s_addr;
        g_android_port = android_port;
        ESP_LOGI(TAG, "StreamStartUdp -> %s:%u",
                 inet_ntoa(client.sin_addr), android_port);
        // ... rest of response
    }
    break;
}
```

- [ ] **Step 3: Update app_camera.c to use these globals**

Replace placeholder IP/port in `start_streaming()`:
```c
extern uint32_t g_android_ip;
extern uint16_t g_android_port;
```

- [ ] **Step 4: Verify compilation and commit**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -5
git add -A && git commit -m "feat: wire StreamStartUdp to camera UDP push"
```
