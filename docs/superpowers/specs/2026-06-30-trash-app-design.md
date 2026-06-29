# Trash App — ESP32 + Android 垃圾分类识别功能设计

## 概述

在 ESP32 上新增 `app_trash` 组件，通过摇杆/按键操作，结合 Android 端进行图片裁剪+AI推理，实现垃圾分类识别。识别结果在 ESP32 TFT 显示英文候选列表，Android 显示完整详细信息。

## 整体架构

```
┌─────────────────────┐          TCP(控制)          ┌─────────────────────┐
│  ESP32 app_trash    │◄═══════════════════════════►│  Android TrashScreen │
│                     │                             │                     │
│  · 推流(同Camera)    │◄─────UDP(视频分片)─────────│  · 实时画面显示     │
│  · TFT显示英文列表   │                             │  · 裁剪UI           │
│  · PRESS控制恢复     │                             │  · TFLite推理       │
│  · 显示Fetching...   │                             │  · 结果详情展示     │
└─────────────────────┘                             └─────────────────────┘
```

## 通讯协议扩展

### 新增 CMD

| CMD | 方向 | Payload | 说明 |
|-----|------|---------|------|
| `0x30 TrashMode` | ESP32→Android | 无 | 连接握手后告知 Android 进入 Trash 模式 |
| `0x31 ResumeCapture` | ESP32→Android | 无 | 用户 PRESS 后恢复推流 |
| `0x32 TrashResult` | Android→ESP32 | 见下 | 推理结果回传 |

### 0x32 TrashResult Payload 格式

```
[候选数量N:1B]
N × {
  [标签长度L:1B]    — UTF-8 字节数（最大值63）
  [标签UTF-8:L B]   — 英文标签（如 "Plastic Bottle"）
  [置信度:1B]       — 0-100 百分比
}
```

示例（塑料瓶 92%，玻璃瓶 5%，金属罐 2%）：
```
02
0E 50 6C 61 73 74 69 63 20 42 6F 74 74 6C 65   5C    -- "Plastic Bottle" 92%
0B 47 6C 61 73 73 20 42 6F 74 74 6C 65         05    -- "Glass Bottle" 5%
```

Android 端负责中文→英文标签映射（模型推理输出中文，映射后发送英文）。

### comms_server 回调机制

新增 `comms_server_set_app_handler()`，允许 `app_trash` 注册命令回调（0x30-0x4F 段），comms_server 的 handle_packet 中新增路由到回调函数。

### Android 端协议对齐

在 `Esp32Protocol.kt` 增加：
```kotlin
TrashMode(0x30),
ResumeCapture(0x31),
TrashResult(0x32)
```

### 状态修正：StreamStopUdp 时机

Android 按"识别"后**立即**：
1. Android 发 `StreamStopUdp(0x21)` → ESP32 app_trash **真正停止相机+推流**（释放帧缓冲、停 stream_task、关 UDP socket）
2. TFT 显示 "Fetching..."
3. Android 冻结最后帧、显示 CropOverlay

裁剪确认+推理完成后 → 发 `0x32 TrashResult` → ESP32 TFT 显示结果列表。

PRESS 恢复时：ESP32 app_trash **重启相机+推流** → 发 `0x31 ResumeCapture` → Android `udp.start()` 恢复接收。

**取消裁剪**：Android 发 `StreamStartUdp(0x20)` → ESP32 重启相机+推流 → Android `udp.start()` 恢复接收。

## ESP32 app_trash 组件

### 文件结构

```
components/app_trash/
├── CMakeLists.txt
├── app_trash.c       # 主逻辑
└── app_trash.h       # 接口（app_t + comms_handler 声明）
```

注册到 launcher，菜单名 "Trash Detect"。

### 状态机

```
[Launcher打开]
    │
    ▼
[推流握手模式] ←──── 同 Camera: show IP + wait Android TCP
    │
    ▼ (Android连上, 发 0x30 TrashMode)
[推流中: TFT "Ready"]
    │
    ▼ (Android 按识别 → StreamStopUdp)
[停止相机+推流: TFT "Fetching..."]
    │
    ▼ (收到 0x32 TrashResult)
[结果展示: TFT 英文列表]
    │
    ▼ (PRESS → 重启相机+推流 + 发 0x31)
[回到推流中]
```

### TFT 显示模板

**推流中：**
```
  Trash Detect
────────────────
  IP: 10.136.243.83
────────────────
  Ready for capture
```

**等待推理结果：**
```
  Trash Detect
────────────────
   Fetching...
────────────────
```

**结果展示（Top 3）：**
```
  Trash Detect
────────────────
  1. Plastic Bottle 92%
  2. Glass Bottle   5%
  3. Can            2%
────────────────
  PRESS to resume
```

### 关键实现

- **推流**：完全复用 `app_camera` 的 UDP 推流逻辑（stream_task），可抽取公共函数
- **结果展示**：lv_label 多行显示，摇杆 PRESS 监听 + 发 `0x31` + `StreamStartUdp`
- **停止推流**：监听 `StreamStopUdp`（由 Android 识别时发送），TFT 切到 "Fetching..."

## Android TrashScreen

### 文件结构

```
ui/camera/TrashScreen.kt     # 主屏幕 Composable
ui/camera/CropOverlay.kt     # 裁剪 UI 组件
```

导航路由新增 `Screen.Trash`（已存在的 `Screen.Esp32Camera` 保留不变）。

### 状态机

```
Disconnected → Connecting
    │
    ▼ (TCP成功 + 收到0x30 TrashMode)
Streaming ─── 点击"识别" → 发StopUdp → CropOverlay
    │                           
    │     ┌─ 取消裁剪 → 发StartUdp+udp.start → Streaming
    │     ▼              
    │  CropOverlay         
    │  (裁剪框确认)            
    │     │               
    │     ▼               
    │  Inferring          
    │  (进度指示器)            
    │     │               
    │     ▼               
    │  Result             
    │  (发0x32 TrashResult)
    │     │               
    └── ← 0x31 ResumeCapture ─┘
           udp.start → Streaming
```

### UI 布局

**Streaming 状态（实时画面 + 底部按钮）：**
```
┌─────────────────────┐
│   ← 返回    Trash   │ ← TopBar（显示模式名称）
├─────────────────────┤
│                     │
│     实时推流画面     │
│    (全屏，解码显示)   │
│                     │
├─────────────────────┤
│  [   🔍 识别  ]     │ ← 底部居中 Button（仅 Streaming 态可见）
└─────────────────────┘
```

**Captured → CropOverlay（冻结 + 裁剪）：**
```
┌─────────────────────┐
│  取消    裁剪区域     │ ← TopBar
├─────────────────────┤
│                     │
│   冻结帧 (全屏)       │
│   半透明遮罩          │
│   ┌──────────┐      │
│   │ 裁剪矩形框 │      │ ← 8个拖动手柄
│   │           │      │
│   └──────────┘      │
│                     │
├─────────────────────┤
│  [  确认裁剪  ]      │ ← 确认后推理
└─────────────────────┘
```

**Result 状态（显示详细结果，等待 ESP32）：**
```
┌─────────────────────┐
│   Trash — 等待ESP32 │ ← TopBar 无返回
├─────────────────────┤
│                     │
│   冻结帧 (缩小预览)   │
│                     │
├─────────────────────┤
│  🗑 厨余垃圾           │
│  ────────────────    │
│  苹果核   88%        │
│  香蕉皮    6%        │
│  茶叶渣    3%        │
│  纸巾      1%        │
│                      │
│  等待ESP32继续...    │ ← 收到 0x31 后自动回到 Streaming
└─────────────────────┘
```

### 裁剪实现

- Android 按"识别" → `StreamStopUdp` 通知 ESP32 停相机 → Android 冻结最后帧 → CropOverlay
- CropOverlay：全屏半透明遮罩 + 可拖动矩形框（Canvas + pointerInput 拖动手柄）
- 确认 → 缩放矩形区域到 TFLite 模型输入尺寸（224×224），执行推理
- 推理完 → 发 `0x32 TrashResult` → ESP32 显示结果
- 取消裁剪 → `StreamStartUdp` 通知 ESP32 重启相机 → `udp.start()` 恢复接收
- 收到 `0x31 ResumeCapture` → `udp.start()` 恢复接收

### TFLite 推理

- 复用 `TFLiteInferenceEngine`（当前项目中已有）
- 裁剪区域缩放到 224×224，做同样预处理
- 推理输出 = labels.json 中的索引 + 置信度
- 取 Top-K（默认 K=5），中文标签 → 英文标签映射
- 当前 TFLite 推理已知有坑，后续再修

## 导航入口

### Android 端

`SourceSelectionScreen` 增加"Trash Detect"入口，点击跳转 `TrashScreen`：
```
┌──────────────────┐
│ 选择检测源       │
│                  │
│ [📷 本地相机]    │ ← 已有
│ [📡 ESP32 Camera]│ ← 已有
│ [🗑 Trash Detect]│ ← 新增
└──────────────────┘
```

### ESP32 端

launcher 菜单新增：
```
Camera        ← 已有
Weather       ← 已有
Trash Detect  ← 新增
Settings      ← 已有
```

## 错误处理

| 场景 | 处理 |
|------|------|
| Android 取消裁剪 | 发送 `StreamStartUdp(0x20)` → ESP32 重启相机推流 → Android `udp.start()` 恢复接收 |
| TFLite 推理失败 | 发 `0x32 TrashResult` 候选数=0，TFT 显示 "Inference failed" |
| 裁剪区域太小 | 弹 Toast "区域太小"，不允许确认 |
| Android 断连 | ESP32 自动退出 Trash 模式回到 launcher |
| ESP32 断推流 | Android 自动重连（复用现有逻辑） |

## 不涉及变更

- 不修改 `app_camera` 现有推流逻辑
- 不修改 ESP32 WiFi/连接/发现机制
- 不修改 Android 现有 `Esp32CameraScreen` / `CameraScreen`
- 不修改现有 TFLite 模型、labels.json
