# Camera APP + OV5640 推流/显示 设计

## 架构

```
ESP32 主菜单
├── Settings       (已有)
├── Camera         (新增)
│   ├── 📡 Stream to Android  → OV5640 → UDP 推流
│   └── 🖥️  Local TFT Display → OV5640 → LVGL 显示
├── Trash          (未来)
└── Weather        (未来)
```

## 组件

| 组件 | 来源 | 职责 |
|------|------|------|
| `esp32-camera` | ESP Registry managed component | OV5640 驱动：初始化、配置、取 JPEG 帧 |
| `app_camera` | 新建 | Camera APP 入口、模式选择、TFT 显示/推流控制 |

## Camera APP 行为

### 入口
进入 Camera APP 时显示两个全屏按钮：
```
┌──────────────────────┐
│                      │
│  📡 Stream to Android│  ← PRESS 选择
│                      │
│  🖥️  Local TFT Display│  ← PRESS 选择
│                      │
│  按 ← 返回主菜单     │
└──────────────────────┘
```

### Stream to Android 模式
1. 初始化 OV5640（QVGA 320×240, JPEG 输出）
2. 启动 UDP 推流任务：
   - 每帧：`esp_camera_fb_get()` → 按 UDP 分片协议发包
   - 分片格式：`FRAME_ID(4) + TIMESTAMP(4) + FRAG_ID(2) + FRAG_TOTAL(2) + JPEG`
   - Android 端 `UdpFrameReceiver` 接收拼帧
3. TFT 显示 "Streaming..." 静态文字
4. Android 收到帧后送入 TFLite 推理（Trash detection）

### Local TFT Display 模式
1. 初始化 OV5640（QQVGA 160×120, RGB565 输出）
2. 定时取帧 → 直接 DMA 到 LVGL `lv_img` → 显示实时画面
3. 帧率约 5-10fps

### 退出
LONG_PRESS → 关摄像头 → `esp_camera_deinit()` → 回到模式选择画面

## 依赖

`idf_component.yml` 添加：
```yaml
dependencies:
  espressif/esp32-camera: "^2.0"
```

## 待改动文件

| 文件 | 动作 |
|------|------|
| `main/idf_component.yml` | 添加 esp32-camera 依赖 |
| `components/app_camera/app_camera.c` | 新建 — Camera APP 主体 |
| `components/app_camera/app_camera.h` | 新建 |
| `components/app_camera/CMakeLists.txt` | 新建 |
| `main/main.c` | 注册 `app_camera` 实例 |
| `sdkconfig.defaults` | 已加 SimSun 16 CJK |

## Android 端（已有）

- `Esp32CameraScreen.kt` → `Esp32Client.send(Cmd.StartCamera)` → `StreamStartUdp` → `UdpFrameReceiver` → JPEG → `engine.infer(bitmap)`
- TFLite 推理（Trash detection）在收到完整帧后自动执行
