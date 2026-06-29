# ESP32-CAM 传感器参数可编辑设置 — 设计文档

## 概述

在 Settings APP 的 Camera Config 段中，将传感器参数从静态显示改为可编辑。修改后写入 NVS 持久化，Camera APP 启动时读取并应用到 OV5640。

## 涉及文件

| 文件 | 改动 |
|------|------|
| `components/app_settings/app_settings.c` | Camera Config 子项改为可编辑 + NVS 读写 |
| `components/app_camera/app_camera.c` | on_create 读取 NVS → sensor_t API 应用 |
| `components/app_camera/CMakeLists.txt` | 添加 nvs_flash 依赖 |

## 1. Settings APP — 编辑交互

### 1.1 参数列表

Camera Config 段展开后子项如下（共 8 项）：

| 显示文本 | Key | 类型 | 默认值 | 可选值 |
|---------|-----|------|--------|--------|
| `Res:  1280x720` | resolution | u8 | 8(FRAMESIZE_HD) | 8=HD / 5=VGA / 4=QVGA |
| `Qual: 10` | quality | u8 | 10 | 0~30 |
| `Mirror: ON` | mirror | u8 | 1(ON) | 0/1 |
| `Flip: OFF` | flip | u8 | 0(OFF) | 0/1 |
| `Bright: 0` | brightness | i8 | 0 | -2~+2 |
| `Contrast: 0` | contrast | i8 | 0 | -2~+2 |
| `WB: Auto` | wb_mode | u8 | 0(AUTO) | 0=Auto/1=Sunny/2=Cloudy/3=Fluorescent |
| `AE Lv: 0` | ae_level | i8 | 0 | -2~+2 |

### 1.2 交互流程

```
初始状态：子项显示当前值，例如 "Qual: 10"
    │
用户 PRESS 该子项
    │
进入编辑态 → 背景高亮 + 行尾闪烁 "◄ 10 ►" 表示可调
    │
X轴 LEFT/RIGHT 改值 — 值实时更新显示
    │
用户再次 PRESS → 确认
    │
写入 NVS → 退出编辑态 → 恢复正常显示
```

- 同时只能编辑一项（退出编辑态后才能选下一项）
- 编辑态时 UP/DOWN 被禁用（不切换选中项）
- 编辑态时 LEFT/RIGHT 改值而非切换子项
- LONG_PRESS 不退出编辑态（保持简单，只 PRESS 确认）

### 1.3 值格式化

| 参数 | 显示格式 | 编码 |
|------|---------|------|
| Resolution | `1280x720` / `640x480` / `320x240` | framesize_t 枚举值 |
| Quality | `10` | 直接 u8 |
| Mirror/Flip | `ON` / `OFF` | u8 0/1 |
| Brightness/Contrast | `-2` `-1` `0` `+1` `+2` | i8 |
| WB Mode | `Auto` `Sunny` `Cloudy` `Fluo` | 枚举字符串映射 |
| AE Level | `-2` `-1` `0` `+1` `+2` | i8 |

## 2. NVS 存储

### 2.1 初始化

NVS 已在 `main.c` 中通过 `nvs_flash_init()` 初始化，Settings APP 直接使用即可。

### 2.2 命名空间与 Key

- **Namespace**: `cam_cfg`
- **读写模式**: `NVS_READWRITE`
- **类型**: 全部用 `uint8_t` / `int8_t`（NVS 原生支持 `i8` 和 `u8`）
- **首次使用**: 若无值则使用默认值（不写入 NVS，只在内存中使用默认值）
- **写入时机**: 用户 PRESS 确认后立即写

### 2.3 读取快照

Camera APP 的 `on_create` 中一次性读取所有参数到局部变量，后续 `sensor_t` API 调用依次写入。

## 3. Camera APP — 应用参数

### 3.1 流程

```
on_create(parent)
  │
  ├─ 读取 NVS "cam_cfg"
  │   resolution, quality, mirror, flip,
  │   brightness, contrast, wb_mode, ae_level
  │
  ├─ reboot_mode_ui()  ← 已有
  │
  └─ [新增] 当进入推流/TFT模式时：
      init_camera(默认HD, PIXFORMAT_JPEG)  ← 已有
      sensor_t *s = esp_camera_sensor_get()
      应用 NVS 参数（覆盖 init_camera 的默认值）：
      s->set_framesize(s, nvs_resolution)
      s->set_quality(s, nvs_quality)
      s->set_hmirror(s, nvs_mirror)
      s->set_vflip(s, nvs_flip)
      s->set_brightness(s, nvs_brightness)
      s->set_contrast(s, nvs_contrast)
      s->set_wb_mode(s, nvs_wb_mode)
      s->set_ae_level(s, nvs_ae_level)
```

### 3.2 注意事项

- 所有 sensor API 调用在 `esp_camera_init()` 成功后进行
- 若某个 sensor API 调用失败，忽略该参数，不影响其他参数

## 4. 质量/白雾修复

将 `jpeg_quality` 改回 10（原 18 压缩太狠导致发白），并添加 `set_ae_level` 调至 -1 防止过曝。

## 5. CMakeLists

`components/app_camera/CMakeLists.txt` 添加 `nvs_flash` 到 `REQUIRES`。

## 6. 边界情况

| 场景 | 处理 |
|------|------|
| NVS 未初始化 | `nvs_open` 返回 ESP_ERR_NVS_NOT_INITIALIZED → 使用默认值 |
| NVS 无该 key | `nvs_get` 返回 ESP_ERR_NVS_NOT_FOUND → 使用默认值 |
| 写入 NVS 失败 | 忽略（静默失败，下次启动仍为旧值） |
| sensor_api 调用失败 | 忽略（传感器不支持该参数时静默） |
| Settings 和 Camera 切换 | 无竞态——两个 APP 不会同时存在 |
