# 传感器参数可编辑设置 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Settings APP 的 Camera Config 段中实现可编辑传感器参数，写入 NVS 持久化，Camera APP 启动时读取并应用。

**Architecture:** 新增 `cam_config.c/h` 封装 NVS 读写（namespace `cam_cfg`，8 个参数）。Settings APP 读取/编辑/写入，Camera APP 读取后通过 `sensor_t` API 写入 OV5640。

**Tech Stack:** ESP-IDF NVS, esp32-camera sensor_t API, LVGL

## 全局约束

- NVS namespace: `cam_cfg`
- 参数默认值：resolution=8(HD), quality=10, mirror=1(ON), flip=0(OFF), brightness=0, contrast=0, wb_mode=0(AUTO), ae_level=0
- Settings 编辑态：PRESS 进入→X轴改值→PRESS 确认→写 NVS
- Camera APP: init_camera 后通过 sensor API 覆盖
- Mirror 默认 ON（镜像显示）
- 所有 sensor API 调用失败时静默忽略

---

### Task 1: NVS 存储帮助模块 `cam_config.c/h`

**文件：**
- Create: `components/cam_config/cam_config.h`
- Create: `components/cam_config/cam_config.c`
- Create: `components/cam_config/CMakeLists.txt`

**Interfaces:**
- `void cam_config_load(cam_config_t *cfg)` — 从 NVS 读取所有参数，使用默认值兜底
- `esp_err_t cam_config_save(const cam_config_t *cfg)` — 写入 NVS

**结构体定义（cam_config.h）：**

```c
#pragma once
#include "stdint.h"
#include "esp_err.h"

typedef struct {
    uint8_t resolution;   // framesize_t: 8=HD, 5=VGA, 4=QVGA
    uint8_t quality;      // 0-30
    uint8_t mirror;       // 0/1
    uint8_t flip;         // 0/1
    int8_t  brightness;   // -2~+2
    int8_t  contrast;     // -2~+2
    uint8_t wb_mode;      // 0=Auto 1=Sunny 2=Cloudy 3=Fluorescent
    int8_t  ae_level;     // -2~+2
} cam_config_t;

extern const cam_config_t CAM_CONFIG_DEFAULT;  // 默认值

void cam_config_load(cam_config_t *cfg);
esp_err_t cam_config_save(const cam_config_t *cfg);
```

**cam_config.c 实现：**

```c
#include "cam_config.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "CAM_CFG";
#define NVS_NS "cam_cfg"

const cam_config_t CAM_CONFIG_DEFAULT = {
    .resolution = 8,    // FRAMESIZE_HD
    .quality = 10,
    .mirror = 1,        // ON (镜像)
    .flip = 0,
    .brightness = 0,
    .contrast = 0,
    .wb_mode = 0,       // Auto
    .ae_level = 0,
};

void cam_config_load(cam_config_t *cfg) {
    *cfg = CAM_CONFIG_DEFAULT;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return;

    nvs_get_u8(h, "resolution", &cfg->resolution);
    nvs_get_u8(h, "quality", &cfg->quality);
    nvs_get_u8(h, "mirror", &cfg->mirror);
    nvs_get_u8(h, "flip", &cfg->flip);
    nvs_get_i8(h, "brightness", &cfg->brightness);
    nvs_get_i8(h, "contrast", &cfg->contrast);
    nvs_get_u8(h, "wb_mode", &cfg->wb_mode);
    nvs_get_i8(h, "ae_level", &cfg->ae_level);

    nvs_close(h);
}

esp_err_t cam_config_save(const cam_config_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8(h, "resolution", cfg->resolution);
    nvs_set_u8(h, "quality", cfg->quality);
    nvs_set_u8(h, "mirror", cfg->mirror);
    nvs_set_u8(h, "flip", cfg->flip);
    nvs_set_i8(h, "brightness", cfg->brightness);
    nvs_set_i8(h, "contrast", cfg->contrast);
    nvs_set_u8(h, "wb_mode", cfg->wb_mode);
    nvs_set_i8(h, "ae_level", cfg->ae_level);

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
```

**CMakeLists.txt：**

```cmake
idf_component_register(SRCS "cam_config.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash)
```

- [ ] **Step 1: 创建 cam_config.h**
- [ ] **Step 2: 创建 cam_config.c**
- [ ] **Step 3: 创建 CMakeLists.txt**
- [ ] **Step 4: 验证编译** `idf.py build`

---

### Task 2: Settings APP — Camera Config 段可编辑

**文件：**
- Modify: `components/app_settings/app_settings.c`
- Requires: `cam_config`（添加 REQUIRES 到 CMakeLists.txt）

**设计：**

Camera Config 段（`s_selected == 0` 时）展开后子项原为 3 项静态文本，改为 8 项动态参数。每项格式 `"参数名: 值"`。

**子项定义：**

```c
// 替换原有的 CAMERA_ITEMS
#define CAM_CFG_COUNT 8
static const char *CAM_CFG_NAMES[CAM_CFG_COUNT] = {
    "Res", "Qual", "Mirror", "Flip",
    "Bright", "Contrast", "WB", "AE Lv"
};

// 全局
static cam_config_t s_cam_cfg;
static int s_editing = -1;  // -1=不在编辑，>=0=正在编辑的索引
static int s_editing_initial = 0;  // 进入编辑时的初始值，用于判断是否变更
```

**on_create 加载 NVS：**

在 `on_create` 末尾（`set_header_style` 之后）调用 `cam_config_load(&s_cam_cfg)`。

**编辑交互（在 on_joystick 中 Camera Config 选中的分支处理）：**

```c
// 仅在 Camera Config 段（s_selected == 0）且展开时启用编辑
if (s_selected == 0 && s_sections[0].expanded) {
    if (s_editing >= 0) {
        // ── 编辑态 ──
        switch (evt) {
        case JOY_EVT_PRESS:
            // 确认修改 → 写 NVS
            cam_config_save(&s_cam_cfg);
            s_editing = -1;
            break;
        case JOY_EVT_LEFT:
            modify_cam_cfg(s_editing, -1);  // 减小值
            break;
        case JOY_EVT_RIGHT:
            modify_cam_cfg(s_editing, 1);   // 增大值
            break;
        default: break;
        }
        update_cam_cfg_display();  // 刷新子项文本
        return;  // 消费事件，不继续导航
    } else {
        // ── 非编辑态 ──
        if (evt == JOY_EVT_PRESS) {
            // 进入编辑态
            s_editing = s_sub_selected;
            return;
        }
    }
}
```

**值修改函数：**

```c
static void modify_cam_cfg(int idx, int delta) {
    switch (idx) {
    case 0: // Res: HD/VGA/QVGA
        s_cam_cfg.resolution = clamp_u8(s_cam_cfg.resolution - delta, 4, 8);
        // 只在 4(QVGA),5(VGA),8(HD) 之间跳
        break;
    case 1: // Quality: 0-30
        s_cam_cfg.quality = clamp_u8(s_cam_cfg.quality + delta, 0, 30);
        break;
    case 2: // Mirror: ON/OFF
        s_cam_cfg.mirror = !s_cam_cfg.mirror;
        break;
    case 3: // Flip: ON/OFF
        s_cam_cfg.flip = !s_cam_cfg.flip;
        break;
    case 4: // Brightness: -2~+2
        s_cam_cfg.brightness = clamp_i8(s_cam_cfg.brightness + delta, -2, 2);
        break;
    case 5: // Contrast: -2~+2
        s_cam_cfg.contrast = clamp_i8(s_cam_cfg.contrast + delta, -2, 2);
        break;
    case 6: // WB: 0~3 循环
        s_cam_cfg.wb_mode = (s_cam_cfg.wb_mode + delta) % 4;
        break;
    case 7: // AE Level: -2~+2
        s_cam_cfg.ae_level = clamp_i8(s_cam_cfg.ae_level + delta, -2, 2);
        break;
    }
}
```

**值格式化显示：** 在 `update_cam_cfg_display()` 中更新子项的 label text。例如：

```c
static void update_cam_cfg_display(void) {
    section_t *sec = &s_sections[0];
    if (!sec->header) return;

    const char *res_str = s_cam_cfg.resolution == 8 ? "1280x720" :
                          s_cam_cfg.resolution == 5 ? "640x480" : "320x240";
    const char *wb_str = s_cam_cfg.wb_mode == 0 ? "Auto" :
                         s_cam_cfg.wb_mode == 1 ? "Sunny" :
                         s_cam_cfg.wb_mode == 2 ? "Cloudy" : "Fluo";

    // 逐个更新子项 label 文本
    for (int i = 0; i < CAM_CFG_COUNT; i++) {
        if (!sec->sub_items[i]) continue;
        lv_obj_t *lbl = lv_obj_get_child(sec->sub_items[i], 0);
        if (!lbl) continue;

        char buf[32];
        switch (i) {
        case 0: snprintf(buf, sizeof(buf), "%s: %s", CAM_CFG_NAMES[i], res_str); break;
        case 1: snprintf(buf, sizeof(buf), "%s: %u", CAM_CFG_NAMES[i], s_cam_cfg.quality); break;
        case 2: snprintf(buf, sizeof(buf), "%s: %s", CAM_CFG_NAMES[i], s_cam_cfg.mirror ? "ON" : "OFF"); break;
        case 3: snprintf(buf, sizeof(buf), "%s: %s", CAM_CFG_NAMES[i], s_cam_cfg.flip ? "ON" : "OFF"); break;
        case 4: snprintf(buf, sizeof(buf), "%s: %+d", CAM_CFG_NAMES[i], s_cam_cfg.brightness); break;
        case 5: snprintf(buf, sizeof(buf), "%s: %+d", CAM_CFG_NAMES[i], s_cam_cfg.contrast); break;
        case 6: snprintf(buf, sizeof(buf), "%s: %s", CAM_CFG_NAMES[i], wb_str); break;
        case 7: snprintf(buf, sizeof(buf), "%s: %+d", CAM_CFG_NAMES[i], s_cam_cfg.ae_level); break;
        }
        lv_label_set_text(lbl, buf);
    }
}
```

**编辑态高亮：** 编辑中的子项背景色设为更亮（`lv_color_make(80, 80, 160)`），正常选中为 `lv_color_make(60, 60, 110)`。

**对 UI 结构的影响：**
- `SUB_COUNTS[0]` 从 3 改为 `CAM_CFG_COUNT` = 8
- `SUB_ITEMS[0]` 改为 NULL（动态刷新，不用静态字符串数组）
- `create_sub_item` 中初始文本为空，由 `update_cam_cfg_display` 填充
- `update_cam_cfg_display()` 在 `on_create` 末尾和每次修改后调用
- 编辑态时 UP/DOWN 不处理，LEFT/RIGHT 仅用于改值

- [ ] **Step 1: 修改 sub_count、CAMERA_ITEMS 定义，添加 s_cam_cfg、s_editing 变量**
- [ ] **Step 2: 实现 modify_cam_cfg() 和 update_cam_cfg_display()**
- [ ] **Step 3: 修改 on_create 加载 NVS + 调用 update_cam_cfg_display**
- [ ] **Step 4: 修改 on_joystick —— Camera Config 段加编辑态判断**
- [ ] **Step 5: 编辑态高亮样式**
- [ ] **Step 6: CMakeLists.txt 添加 cam_config 依赖**
- [ ] **Step 7: 验证编译**

---

### Task 3: Camera APP — 读取 NVS 并应用 sensor API

**文件：**
- Modify: `components/app_camera/app_camera.c`
- Modify: `components/app_camera/CMakeLists.txt`
- Requires: `cam_config`（添加 REQUIRES）

**在 try_start_stream（推流模式 init_camera 后）添加：**

```c
#include "cam_config.h"

// 在 init_camera(FRAMESIZE_HD, PIXFORMAT_JPEG) 成功之后，stream_task 创建之前：
cam_config_t cfg;
cam_config_load(&cfg);
sensor_t *s = esp_camera_sensor_get();
if (s) {
    s->set_framesize(s, cfg.resolution);
    s->set_quality(s, cfg.quality);
    s->set_hmirror(s, cfg.mirror);
    s->set_vflip(s, cfg.flip);
    s->set_brightness(s, cfg.brightness);
    s->set_contrast(s, cfg.contrast);
    s->set_wb_mode(s, cfg.wb_mode);
    s->set_ae_level(s, cfg.ae_level);
    ESP_LOGI(TAG, "Sensor config applied: res=%u qual=%u mirror=%u flip=%u",
             cfg.resolution, cfg.quality, cfg.mirror, cfg.flip);
}
```

**TFT 模式（enter_tft_mode）中同样添加**（init_camera 后 apply）。

**移除硬编码质量：** `cfg.jpeg_quality` 由 NVS quality 覆盖，`init_camera` 中可用回 10 或任意默认值。

- [ ] **Step 1: 在 try_start_stream 中添加 NVS 读取 + sensor API 调用**
- [ ] **Step 2: 在 enter_tft_mode 中添加同样的逻辑**
- [ ] **Step 3: CMakeLists.txt 添加 cam_config 依赖**
- [ ] **Step 4: 验证编译**

---

### Task 4: 全量构建 + 烧录验证

**文件：**
- 无需修改

- [ ] **Step 1: 清理构建并全量编译** `idf.py fullclean build`
- [ ] **Step 2: 检查编译警告和错误**
- [ ] **Step 3: 烧录并验证启动日志**
  - 预期日志顺序：`CAM_CFG: loaded → APP_CAMERA: Camera init OK → Sensor config applied → Stream task started`
  - 检查 Mirror 是否为 ON（画面不再镜像）
  - 检查 Quality 10 下无白雾
- [ ] **Step 4: 进入 Settings → Camera Config，验证子项可编辑**
  - PRESS 进入编辑态 → 高亮变化
  - LEFT/RIGHT 改值 → 值实时更新
  - PRESS 确认 → 写入 NVS（重启后值保持）
- [ ] **Step 5: 重启后进入 Camera 推流，验证参数被应用**

---

## 文件变更总结

| 文件 | 操作 | 说明 |
|------|------|------|
| `components/cam_config/cam_config.h` | Create | 结构体 + 函数声明 |
| `components/cam_config/cam_config.c` | Create | NVS 读写实现 |
| `components/cam_config/CMakeLists.txt` | Create | IDF 组件注册 |
| `components/app_settings/app_settings.c` | Modify | Camera Config 子项可编辑 + NVS 加载/保存 |
| `components/app_settings/CMakeLists.txt` | Modify | 添加 cam_config 依赖 |
| `components/app_camera/app_camera.c` | Modify | init_camera 后应用 NVS sensor 参数 |
| `components/app_camera/CMakeLists.txt` | Modify | 添加 cam_config 依赖 |

## 子项变动

- `s_sections[0].sub_count`：3 → 8
- `CAMERA_ITEMS`（3 项静态文本）→ `CAM_CFG_NAMES`（8 项动态参数）
- `s_cam_cfg`：全局存储当前 Camera 配置
- `s_editing`：当前编辑的子项索引，-1 表示不编辑
- 编辑态时 UP/DOWN 禁用、LEFT/RIGHT 改值、PRESS 确认退出