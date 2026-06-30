# Trash App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Trash Detect app — ESP32 streams to Android, user captures+crops+infers garbage type, result displays on both sides.

**Architecture:** ESP32 `app_trash` component + comms_server callback routing (CMD 0x30-0x32). Android `TrashScreen` + `CropOverlay` composable. Both reuse existing stream/client/inference infrastructure.

**Tech Stack:** ESP-IDF, LVGL 8.3, Android Compose, TFLite

## Global Constraints

- ESP32: no new WiFi/network init logic; reuse `comms_server`, `g_android_connected/ip/port`
- Android: reuse `Esp32Client`, `UdpFrameReceiver`, `TFLiteInferenceEngine` as-is
- TFLite inference has known issues — implement the pipeline, bugs deferred
- 0x32 TrashResult payload: [N:1B] + N×[labelLen:1B][labelUTF8][confidence:1B], label max 63B
- ESP32 TFT shows English only (no CJK support), Android shows Chinese
- "Trash" is already in APP_NAMES[1] in ui_main.c — only missing `app_trash` singleton + extern

---

### Batch 1 / Task 1: comms_server callback routing

**Files:**
- Modify: `components/comms_server/comms_server.h`
- Modify: `components/comms_server/comms_server.c`

**Interfaces:**
- Produces: `void comms_server_set_app_handler(void (*handler)(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint32_t plen))`
  — registers a callback for CMD 0x30-0x4F routing; `NULL` to unregister.

- [ ] **Step 1: Add callback type and setter to comms_server.h**

After the existing function declarations, add:
```c
/** 注册APP命令回调（CMD 0x30-0x4F 路由到此回调）。传入NULL取消注册。 */
void comms_server_set_app_handler(void (*handler)(uint8_t cmd, uint16_t seq,
                                                    const uint8_t *payload, uint32_t plen));
```

- [ ] **Step 2: Implement in comms_server.c**

Add a static function pointer:
```c
static void (*s_app_handler)(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint32_t plen) = NULL;

void comms_server_set_app_handler(void (*handler)(uint8_t cmd, uint16_t seq,
                                                    const uint8_t *payload, uint32_t plen))
{
    s_app_handler = handler;
}
```

In `handle_packet()`, add routing before the `default:` case:
```c
    case 0x30: case 0x31: case 0x32:
    case 0x33: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x38:
    case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        if (s_app_handler) {
            s_app_handler(cmd, seq, payload, plen);
            /* 0x30-0x4F commands: app handler decides whether to send response.
             * For 0x32 (TrashResult from Android), no response needed. */
        } else {
            send_error(fd, seq, cmd, 0x01); // not handled
        }
        break;
```

- [ ] **Step 3: Build verify**

```bash
cd /d/Project/ESP32/esp32-cam-app
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add components/comms_server/comms_server.c components/comms_server/comms_server.h
git commit -m "feat(comms): add app handler callback for 0x30-0x4F routing"
```

---

### Batch 1 / Task 2: app_trash scaffold + registration

**Files:**
- Create: `components/app_trash/CMakeLists.txt`
- Create: `components/app_trash/app_trash.h`
- Create: `components/app_trash/app_trash.c`
- Modify: `components/ui_system/ui_main.c`

**Interfaces:**
- Consumes: `app_t` struct from `app_t.h`, `launcher` from `app_launcher`
- Produces: `app_t app_trash` — the singleton for ui_main to reference
- Implements: `void comms_server_set_app_handler(...)` from Task 1

- [ ] **Step 1: Create `components/app_trash/CMakeLists.txt`**

```cmake
idf_component_register(SRCS "app_trash.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl app_launcher comms_server cam_config esp32-camera)
```

- [ ] **Step 2: Create `components/app_trash/app_trash.h`**

```c
#ifndef APP_TRASH_H
#define APP_TRASH_H

#include "app_t.h"

extern app_t app_trash;

#endif
```

- [ ] **Step 3: Create `components/app_trash/app_trash.c`** — stub with lifecycle but no real logic yet

```c
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
```

- [ ] **Step 4: Register in `components/ui_system/ui_main.c`**

Add extern at top (after existing externs):
```c
extern app_t app_trash;
```

In `open_current_app()` add the routing for index 1:
```c
    if (real_idx == 0) s_active_app = &app_settings;
    else if (real_idx == 1) s_active_app = &app_trash;    // ← ADD
    else if (real_idx == 2) s_active_app = &app_weather;
    else if (real_idx == 3) s_active_app = &app_camera;
```

- [ ] **Step 5: Build verify**

```bash
cd /d/Project/ESP32/esp32-cam-app
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete.`

- [ ] **Step 6: Commit**

```bash
git add components/app_trash/ components/ui_system/ui_main.c
git commit -m "feat(trash): app_trash scaffold + ui_main registration"
```

---

### Batch 1 / Task 3 (parallel with Task 1 & 2): Android protocol + CropOverlay

**Files:**
- Modify: `app/src/main/java/.../esp32/Esp32Protocol.kt`
- Create: `app/src/main/java/.../ui/camera/CropOverlay.kt`

**Interfaces:**
- Produces: `Cmd.TrashMode(0x30)`, `Cmd.ResumeCapture(0x31)`, `Cmd.TrashResult(0x32)` in Esp32Protocol
- Produces: `@Composable fun CropOverlay(bitmap: Bitmap, onCropConfirm: (Rect) -> Unit, onCancel: () -> Unit)` reusable composable

- [ ] **Step 1: Add new CMDs to Esp32Protocol.kt**

Add to the `Cmd` enum (after `StreamStopUdp(0x21)`):
```kotlin
TrashMode(0x30),
ResumeCapture(0x31),
TrashResult(0x32),
```

Add a helper to pack TrashResult payload:
```kotlin
/** 打包 TrashResult payload: [N:1B] + N×[labelLen:1B][labelUTF8:L B][confidence:1B] */
fun packTrashResult(topK: List<Pair<String, Float>>): ByteArray {
    val bos = java.io.ByteArrayOutputStream()
    bos.write(topK.size.coerceAtMost(0xFF))
    for ((label, conf) in topK) {
        val labelBytes = label.toByteArray(StandardCharsets.UTF_8)
        val len = labelBytes.size.coerceAtMost(63)
        bos.write(len)
        bos.write(labelBytes, 0, len)
        bos.write((conf * 100).toInt().coerceIn(0, 255))
    }
    return bos.toByteArray()
}
```

- [ ] **Step 2: Create CropOverlay.kt**

```kotlin
package com.coldfish.garbagedetection.ui.camera

import android.graphics.Rect
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * 裁剪覆盖层 — 在半透明遮罩上显示一个可拖拽的矩形框。
 * @param bitmap 冻结的推流帧（全屏显示）
 * @param onCropConfirm 确认裁剪，返回裁剪区域（相对于bitmap坐标归一化）
 * @param onCancel 取消裁剪
 */
@Composable
fun CropOverlay(
    bitmap: android.graphics.Bitmap,
    onCropConfirm: (Rect) -> Unit,
    onCancel: () -> Unit
) {
    // 裁剪区域比例 (0~1 相对于bitmap宽高)
    var cropLeft by remember { mutableFloatStateOf(0.1f) }
    var cropTop by remember { mutableFloatStateOf(0.2f) }
    var cropRight by remember { mutableFloatStateOf(0.9f) }
    var cropBottom by remember { mutableFloatStateOf(0.8f) }

    Box(modifier = Modifier.fillMaxSize()) {
        // 1. 背景帧
        androidx.compose.foundation.Image(
            bitmap = bitmap.asImageBitmap(),
            contentDescription = "captured",
            modifier = Modifier.fillMaxSize()
        )

        // 2. 遮罩 + 裁剪框
        Canvas(modifier = Modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                // 拖拽四个角来调整裁剪区域
                // 简化实现：单指拖动调整右下角
                detectDragGestures { change, dragAmount ->
                    val w = size.width
                    val h = size.height
                    cropRight = (cropRight * bitmap.width + dragAmount.x / w).coerceIn(0.2f, 0.95f) / bitmap.width
                    cropBottom = (cropBottom * bitmap.height + dragAmount.y / h).coerceIn(0.2f, 0.95f) / bitmap.height
                }
            }
        ) {
            val w = size.width
            val h = size.height

            // 半透明遮罩（裁剪区外）
            drawRect(color = Color.Black.copy(alpha = 0.5f), size = size)

            // 裁剪区（清除遮罩）
            val cropRect = Rect(
                cropLeft * w, cropTop * h,
                cropRight * w, cropBottom * h
            )
            drawRect(color = Color.Transparent, topLeft = cropRect.topLeft, size = cropRect.size)
            drawRect(color = Color.White, topLeft = cropRect.topLeft, size = cropRect.size, style = Stroke(width = 4f))

            // 8个拖动手柄（小圆点）
            val handlePositions = listOf(
                cropRect.topLeft, cropRect.topRight,
                cropRect.bottomLeft, cropRect.bottomRight,
                Offset(cropRect.center.x, cropRect.top),
                Offset(cropRect.center.x, cropRect.bottom),
                Offset(cropRect.left, cropRect.center.y),
                Offset(cropRect.right, cropRect.center.y),
            )
            for (pos in handlePositions) {
                drawCircle(color = Color.White, radius = 6f, center = pos)
            }
        }

        // 3. 底部操作栏
        Row(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(16.dp)
                .fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceEvenly
        ) {
            OutlinedButton(onClick = onCancel) { Text("取消", fontSize = 14.sp) }
            Button(onClick = {
                // 返回相对于bitmap坐标的裁剪区域
                val rx = cropLeft * bitmap.width
                val ry = cropTop * bitmap.height
                val rw = (cropRight - cropLeft) * bitmap.width
                val rh = (cropBottom - cropTop) * bitmap.height
                if (rw < 20 || rh < 20) return@Button  // 太小不可确认
                onCropConfirm(android.graphics.Rect(
                    rx.toInt(), ry.toInt(),
                    (rx + rw).toInt(), (ry + rh).toInt()
                ))
            }) { Text("确认裁剪", fontSize = 14.sp) }
        }
    }
}
```

- [ ] **Step 3: Build verify (Android)**

```bash
cd /d/Project/Android/GarbageDetection
./gradlew :app:assembleDebug 2>&1 | tail -5
```
Expected: `BUILD SUCCESSFUL`

- [ ] **Step 4: Commit**

```bash
git add app/src/main/java/.../esp32/Esp32Protocol.kt
git add app/src/main/java/.../ui/camera/CropOverlay.kt
git commit -m "feat(android): add TrashMode CMDs + CropOverlay composable"
```

---

### Batch 2 / Task 4 (after Task 1+2): app_trash full implementation

**Files:**
- Modify: `components/app_trash/app_trash.c`

**Interfaces:**
- Consumes: `comms_server_set_app_handler()`, `g_android_connected`, `g_android_ip`, `g_android_port`
- Consumes: `comms_server_send_packet()` for sending 0x30/0x31
- Produces: full state machine as described in spec

- [ ] **Step 1: State machine variables + forward declarations**

Replace the TODO stubs with proper state:

```c
/* 状态 */
typedef enum { TRASH_IDLE, TRASH_STREAMING, TRASH_FETCHING, TRASH_RESULT } trash_state_t;
static trash_state_t s_state = TRASH_IDLE;
static lv_obj_t *s_page = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_detail_label = NULL;   // 结果列表多行显示
static lv_timer_t *s_ip_refresh_timer = NULL;
static lv_timer_t *s_wait_android_timer = NULL;
static int s_wait_timeout = 0;
static bool s_streaming = false;
static int s_udp_fd = -1;
static TaskHandle_t s_stream_task = NULL;
static uint32_t s_frame_id = 0;
```

- [ ] **Step 2: start_streaming / stop_streaming**

Copy the camera init logic from `app_camera.c`:

```c
#include "esp_camera.h"
#include "esp_timer.h"
#include "sys/socket.h"
#include "lwip/inet.h"

extern volatile int g_android_connected;
extern uint32_t g_android_ip;
extern uint16_t g_android_port;
extern char s_sta_ip[16];

static void apply_cam_config(void)
{
    cam_config_t cfg;
    cam_config_load(&cfg);
    sensor_t *s = esp_camera_sensor_get();
    if (!s) { ESP_LOGW(TAG, "sensor_get failed"); return; }
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
}

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

    // NVS quality/resolution override (same as app_camera)
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
    if (s_udp_fd < 0) { ESP_LOGE(TAG, "UDP socket failed"); return; }

    xTaskCreate(stream_task, "trash_stream", 4096, NULL, 4, &s_stream_task);
    s_frame_id = 0;
    s_streaming = true;
    ESP_LOGI(TAG, "Stream started");
}

static void stop_streaming(void)
{
    if (s_stream_task) { vTaskDelete(s_stream_task); s_stream_task = NULL; }
    if (s_udp_fd >= 0) { close(s_udp_fd); s_udp_fd = -1; }
    esp_camera_deinit();
    s_streaming = false;
    ESP_LOGI(TAG, "Stream stopped");
}
```

- [ ] **Step 3: Wait for Android + start streaming + send TrashMode**

Timer callback (same pattern as app_camera's try_start_stream):
```c
static void try_start_stream(lv_timer_t *t)
{
    (void)t;
    if (!g_android_connected) {
        s_wait_timeout++;
        if (s_wait_timeout >= 300) {
            ESP_LOGW(TAG, "Android connect timeout");
            lv_timer_del(s_wait_android_timer); s_wait_android_timer = NULL;
            // Exit app — close_current_app handled by launcher
        }
        return;
    }
    if (g_android_port == 0 || g_android_ip == 0) return;
    if (s_wait_android_timer) {
        lv_timer_del(s_wait_android_timer); s_wait_android_timer = NULL;
    }

    start_streaming();
    comms_server_send_packet(0x30, NULL, 0); // TrashMode → Android
    s_state = TRASH_STREAMING;
    if (s_status_label)
        lv_label_set_text(s_status_label, "Ready for capture");
}
```

In `on_create` after creating UI elements:
```c
    // Wait for Android
    s_wait_android_timer = lv_timer_create(try_start_stream, 200, NULL);
    s_state = TRASH_IDLE;
```

- [ ] **Step 4: Handle StreamStopUdp (0x21) + display "Fetching..."**

In `on_trash_cmd`, add StreamStopUdp handling. But StreamStopUdp (0x21) is handled by comms_server's default handler, not routed to app callback. Fix: In comms_server handle_packet, after processing 0x20/0x21, also call app_handler if registered.

Actually, simpler: modify comms_server to also route 0x20/0x21 to app_handler:

In `comms_server.c handle_packet()`:
```c
    case 0x20: { // StreamStartUdp
        // ...existing handling...
        if (s_app_handler) s_app_handler(cmd, seq, payload, plen);
        break;
    }
    case 0x21: // StreamStopUdp
        send_response(fd, 0x21, seq, NULL, 0);
        if (s_app_handler) s_app_handler(cmd, seq, payload, plen);
        break;
```

In `on_trash_cmd`:
```c
    case 0x21: // StreamStopUdp from Android — "识别" pressed
        stop_streaming();
        s_state = TRASH_FETCHING;
        if (s_status_label) lv_label_set_text(s_status_label, "Fetching...");
        if (s_detail_label) lv_label_set_text(s_detail_label, "");
        break;
```

- [ ] **Step 5: Parse 0x32 TrashResult + display on TFT**

In `on_trash_cmd`:
```c
    case 0x32: { // TrashResult
        if (plen < 1) break;
        uint8_t count = payload[0];
        s_state = TRASH_RESULT;

        if (count == 0) {
            if (s_status_label) lv_label_set_text(s_status_label, "Inference failed");
            if (s_detail_label) lv_label_set_text(s_detail_label, "");
            break;
        }

        // Build display text: "1. Label 92%\n2. Label 5%..."
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

        if (s_status_label) lv_label_set_text(s_status_label, "Trash Detect");
        if (s_detail_label) lv_label_set_text(s_detail_label, buf);
        ESP_LOGI(TAG, "TrashResult displayed: %s", buf);
        break;
    }
```

- [ ] **Step 6: Handle PRESS to resume**

In `on_joystick`:
```c
static void on_joystick(joystick_evt_t evt)
{
    if (evt == JOY_EVT_PRESS && s_state == TRASH_RESULT) {
        // 恢复推流
        start_streaming();
        comms_server_send_packet(0x31, NULL, 0); // ResumeCapture → Android
        s_state = TRASH_STREAMING;
        if (s_status_label) lv_label_set_text(s_status_label, "Ready for capture");
        if (s_detail_label) lv_label_set_text(s_detail_label, "");
    }
}
```

- [ ] **Step 7: Handle StreamStartUdp (0x20) for cancel-crop**

On cancel-crop, Android sends StreamStartUdp. ESP32 needs to restart streaming:
```c
    case 0x20: // StreamStartUdp from Android — cancel crop or initial start
        if (s_state != TRASH_STREAMING) {
            start_streaming();
            s_state = TRASH_STREAMING;
            if (s_status_label) lv_label_set_text(s_status_label, "Ready for capture");
        }
        break;
```

- [ ] **Step 8: TFT layout in on_create — two labels for status + detail**

Replace the `on_create` stub:
```c
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
```

- [ ] **Step 9: Build verify**

```bash
cd /d/Project/ESP32/esp32-cam-app
idf.py build 2>&1 | tail -10
```
Expected: `Project build complete.`

- [ ] **Step 10: Commit**

```bash
git add components/app_trash/app_trash.c components/comms_server/comms_server.c
git commit -m "feat(trash): full state machine — stream lifecycle, result display, PRESS resume"
```

---

### Batch 2 / Task 5 (parallel with Task 4): Android TrashScreen full implementation

**Files:**
- Create: `app/src/main/java/.../ui/camera/TrashScreen.kt`
- Modify: `app/src/main/java/.../navigation/AppNavigation.kt`
- Modify: `app/src/main/java/.../ui/source/SourceSelectionScreen.kt`

**Interfaces:**
- Consumes: `Cmd`, `Protocol.packTrashResult()`, `Esp32Client`, `UdpFrameReceiver`, `TFLiteInferenceEngine`
- Consumes: `CropOverlay` composable from Task 3
- Produces: Full TrashScreen with 5 states (Disconnected → Streaming → CropOverlay → Inferring → Result)

- [ ] **Step 1: Add Trash route + SourceSelection entry**

In `AppNavigation.kt`, add Screen.Trash:
```kotlin
data object Trash : Screen("trash")
```

In `NavHost` block, add route:
```kotlin
composable(Screen.Trash.route) {
    TrashScreen(
        onNavigateBack = { navController.popBackStack() },
        engine = engine
    )
}
```

In `SourceSelectionScreen.kt`, add callback param and button:
```kotlin
fun SourceSelectionScreen(
    onNavigateToCamera: () -> Unit,
    onNavigateToEsp32: () -> Unit,
    onNavigateToTrash: () -> Unit,  // ← ADD
    weatherVM: WeatherViewModel = viewModel()
)
```

Add button after the ESP32 button:
```kotlin
Spacer(Modifier.height(8.dp))
Button(
    onClick = onNavigateToTrash,
    modifier = Modifier.fillMaxWidth().height(56.dp),
    shape = RoundedCornerShape(12.dp),
    colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFE17055))
) { Text("\uD83D\uDDD1\uFE0F Trash Detect", fontSize = 16.sp) }
```

In main Activity or wherever SourceSelectionScreen is called, pass `onNavigateToTrash`.

- [ ] **Step 2: Create TrashScreen.kt** — full state machine

```kotlin
package com.coldfish.garbagedetection.ui.camera

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Rect
import android.util.Log
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.coldfish.garbagedetection.esp32.*
import com.coldfish.garbagedetection.inference.DetectionResult
import com.coldfish.garbagedetection.inference.InferenceEngine
import com.coldfish.garbagedetection.inference.LabelLoader
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val TAG = "GD:TrashScreen"

// 中文→英文标签映射（推理输出中文，回传ESP32用英文）
private val LABEL_EN_MAP = mapOf(
    "塑料瓶" to "Plastic Bottle", "玻璃瓶" to "Glass Bottle",
    "易拉罐" to "Can", "纸箱" to "Cardboard",
    "苹果核" to "Apple Core", "香蕉皮" to "Banana Peel",
    "茶叶渣" to "Tea Leave", "纸巾" to "Tissue",
    "电池" to "Battery", "过期药品" to "Expired Medicine",
    "塑料袋" to "Plastic Bag", "一次性餐具" to "Disposable Cutlery",
)
private fun labelToEn(ch: String) = LABEL_EN_MAP[ch] ?: ch

// 类别→英文
private val CAT_EN_MAP = mapOf(
    "可回收物" to "Recyclable", "厨余垃圾" to "Kitchen Waste",
    "有害垃圾" to "Hazardous", "其他垃圾" to "Other"
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TrashScreen(
    onNavigateBack: () -> Unit,
    engine: InferenceEngine
) {
    // ── 状态 ──
    enum class State { Disconnected, Streaming, CropOverlay, Inferring, Result }
    var state by remember { mutableStateOf(State.Disconnected) }
    var ipText by remember { mutableStateOf("") }
    var statusText by remember { mutableStateOf("输入ESP32 IP") }
    var currentBitmap by remember { mutableStateOf<Bitmap?>(null) }
    var frameToken by remember { mutableIntStateOf(0) }
    var frozenFrame by remember { mutableStateOf<Bitmap?>(null) }  // 识别时冻结的帧
    var detectionResult by remember { mutableStateOf<DetectionResult?>(null) }

    val client = remember { Esp32Client() }
    val udp = remember { UdpFrameReceiver() }
    val scope = rememberCoroutineScope()
    var modelLoaded by remember { mutableStateOf(false) }

    // 加载模型
    LaunchedEffect(Unit) {
        if (!engine.isReady) {
            modelLoaded = withContext(Dispatchers.IO) { engine.loadModel() }
        } else {
            modelLoaded = true
        }
    }

    // ── 事件监听 ──
    LaunchedEffect(Unit) {
        client.onDisconnected = {
            statusText = "ESP32 已断开"
            state = State.Disconnected
            udp.stop()
        }
        launch {
            client.events.collect { pkt ->
                when (pkt.cmd) {
                    Cmd.TrashMode -> {
                        Log.i(TAG, "Received TrashMode → connected")
                        state = State.Streaming
                        udp.start()
                        val port = udp.getLocalPort()
                        val info = client.send(Cmd.StreamStartUdp, Protocol.packShort(port))
                        statusText = "Trash Ready"
                        if (info != null) {
                            val si = Protocol.parseStreamInfo(info.payload)
                            Log.i(TAG, "Stream: ${si.width}x${si.height}")
                        }
                    }
                    Cmd.ResumeCapture -> {
                        Log.i(TAG, "ResumeCapture → back to streaming")
                        udp.start()
                        client.send(Cmd.StreamStartUdp, Protocol.packShort(udp.getLocalPort()))
                        state = State.Streaming
                        frozenFrame = null
                        detectionResult = null
                    }
                    else -> {}
                }
            }
        }
    }

    // ── UDP 帧接收 ──
    LaunchedEffect(state) {
        if (state != State.Streaming) return@LaunchedEffect
        val opts = BitmapFactory.Options().apply { inMutable = true }
        launch(Dispatchers.IO) {
            udp.frames.collect { jpeg ->
                if (state != State.Streaming) return@collect
                try {
                    val bmp = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size, opts)
                    if (bmp != null) {
                        withContext(Dispatchers.Main) {
                            currentBitmap = bmp
                            frameToken++
                        }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Decode: ${e.message}")
                }
            }
        }
    }

    // ── 推理函数 ──
    fun doInference(cropRect: Rect) {
        scope.launch {
            state = State.Inferring
            statusText = "识别中..."
            val result = withContext(Dispatchers.IO) {
                val cropped = Bitmap.createBitmap(
                    frozenFrame!!,
                    cropRect.left, cropRect.top,
                    cropRect.width(), cropRect.height()
                )
                engine.infer(cropped)
            }
            detectionResult = result
            if (result != null) {
                // 发TrashResult到ESP32（英文标签）
                val enTopK = result.topK.map { (label, conf) ->
                    labelToEn(label) to conf
                }
                client.sendNoWait(Cmd.TrashResult, Protocol.packTrashResult(enTopK))
                statusText = "识别完成"
            } else {
                // 推理失败 → 发空结果
                client.sendNoWait(Cmd.TrashResult, byteArrayOf(0))
                statusText = "识别失败"
            }
            state = State.Result
        }
    }

    fun startCapture() {
        frozenFrame = currentBitmap?.copy(Bitmap.Config.ARGB_8888, false)
        // 停UDP + 通知ESP32停推流
        udp.stop()
        client.sendNoWait(Cmd.StreamStopUdp)
        state = State.CropOverlay
        statusText = "裁剪区域"
    }

    // ── UI ──
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(statusText) },
                navigationIcon = {
                    if (state == State.Disconnected || state == State.Streaming) {
                        IconButton(onClick = {
                            client.sendNoWait(Cmd.StreamStopUdp)
                            client.sendNoWait(Cmd.StopCamera)
                            udp.stop()
                            client.disconnect()
                            onNavigateBack()
                        }) { Text("\u2190") }
                    }
                }
            )
        }
    ) { pad ->
        Box(
            modifier = Modifier.fillMaxSize().padding(pad),
            contentAlignment = Alignment.Center
        ) {
            when (state) {
                State.Disconnected -> {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        OutlinedTextField(
                            value = ipText,
                            onValueChange = { ipText = it },
                            label = { Text("ESP32 IP") },
                            placeholder = { Text("192.168.43.x") },
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                            singleLine = true,
                            modifier = Modifier.padding(16.dp)
                        )
                        Button(onClick = {
                            scope.launch {
                                statusText = "连接中..."
                                var target = ipText.trim()
                                if (target.isEmpty()) {
                                    statusText = "自动发现..."
                                    val d = withContext(Dispatchers.IO) { Esp32Discovery.discover() }
                                    if (d != null) { target = d; ipText = d }
                                }
                                if (client.connect(target, 8080)) {
                                    client.send(Cmd.IsValid)  // 握手
                                    statusText = "已连接 (等待TrashMode)"
                                } else {
                                    statusText = "连接失败"
                                }
                            }
                        }) { Text("连接") }
                    }
                }

                State.Streaming -> {
                    Column(modifier = Modifier.fillMaxSize()) {
                        // 实时画面
                        currentBitmap?.let {
                            key(frameToken) {
                                Image(it.asImageBitmap(), null,
                                    modifier = Modifier.weight(1f).fillMaxWidth())
                            }
                        }
                        // 底部识别按钮
                        Button(
                            onClick = { startCapture() },
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp)
                                .height(56.dp),
                            enabled = modelLoaded
                        ) {
                            Text("\uD83D\uDD0D 识别", fontSize = 18.sp)
                        }
                    }
                }

                State.CropOverlay -> {
                    frozenFrame?.let { bmp ->
                        CropOverlay(
                            bitmap = bmp,
                            onCropConfirm = { rect -> doInference(rect) },
                            onCancel = {
                                // 取消裁剪 → 通知ESP32重启
                                client.sendNoWait(Cmd.StreamStartUdp, Protocol.packShort(udp.getLocalPort()))
                                udp.start()
                                state = State.Streaming
                                statusText = "Trash Ready"
                                frozenFrame = null
                            }
                        )
                    }
                }

                State.Inferring -> {
                    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            CircularProgressIndicator()
                            Spacer(Modifier.height(16.dp))
                            Text("识别中...", fontSize = 16.sp)
                        }
                    }
                }

                State.Result -> {
                    Column(
                        modifier = Modifier.fillMaxSize().padding(16.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.Center
                    ) {
                        detectionResult?.let { r ->
                            val catColor = LabelLoader.categoryColor(r.category)
                            Text(r.category,
                                fontSize = 24.sp,
                                color = catColor)
                            Spacer(Modifier.height(8.dp))
                            // Top-K 列表
                            r.topK.forEachIndexed { i, (label, conf) ->
                                Text(
                                    "${label}\t${(conf * 100).toInt()}%",
                                    fontSize = 16.sp
                                )
                            }
                            Spacer(Modifier.height(16.dp))
                            Text("等待ESP32继续...",
                                fontSize = 14.sp,
                                color = androidx.compose.ui.graphics.Color.Gray)
                        } ?: Text("识别失败", fontSize = 16.sp)
                    }
                }
            }
        }
    }

    // 页面退出清理
    DisposableEffect(Unit) {
        onDispose {
            client.sendNoWait(Cmd.StreamStopUdp)
            client.sendNoWait(Cmd.StopCamera)
            udp.stop()
            client.disconnect()
        }
    }
}
```

- [ ] **Step 3: Build verify (Android)**

```bash
cd /d/Project/Android/GarbageDetection
./gradlew :app:assembleDebug 2>&1 | tail -5
```
Expected: `BUILD SUCCESSFUL`

- [ ] **Step 4: Commit**

```bash
git add app/src/main/java/.../ui/camera/TrashScreen.kt
git add app/src/main/java/.../navigation/AppNavigation.kt
git add app/src/main/java/.../ui/source/SourceSelectionScreen.kt
git commit -m "feat(android): TrashScreen full implementation + navigation"
```

---

## Verification

After both batches complete, final verification:

**ESP32 build:**
```bash
idf.py build
```
Expected: clean build.

**Android build:**
```bash
./gradlew :app:assembleDebug
```
Expected: BUILD SUCCESSFUL.

**Protocol integration check:**
- ESP32 `app_trash` registers callback via `comms_server_set_app_handler()`
- Android `TrashScreen` receives `0x30 TrashMode` → enters streaming
- Android sends `0x21 StreamStopUdp` → ESP32 stops camera → TFT "Fetching..."
- Android sends `0x32 TrashResult` → ESP32 displays candidates on TFT
- ESP32 PRESS → sends `0x31 ResumeCapture` → Android `udp.start()` → back to streaming
