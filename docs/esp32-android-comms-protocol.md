# ESP32 ↔ Android 通讯协议文档

> 文件位置：`D:\Project\ESP32\esp32-cam-app\docs\esp32-android-comms-protocol.md`

---

## 一、架构概览

```
┌─────────────────────────────────────────────────────────┐
│  Android (客户端)                                        │
│  ┌──────────────────┐   ┌──────────────────────────┐    │
│  │ Esp32Client      │   │ UdpFrameReceiver         │    │
│  │ TCP → 8080       │   │ UDP ← 随机端口            │    │
│  └──────┬───────────┘   └──────────▲────────────────┘    │
│         │                         │                      │
└─────────┼─────────────────────────┼──────────────────────┘
          │ TCP(控制)               │ UDP(视频流)
          │                         │
┌─────────┼─────────────────────────┼──────────────────────┐
│  ESP32 (服务器)                    │                       │
│  ┌──────┴───────────┐   ┌────────┴───────────────┐       │
│  │ comms_server     │   │ app_camera             │       │
│  │ TCP listening:   │   │ stream_task            │       │
│  │ port 8080        │   │ UDP → Android:port     │       │
│  └──────────────────┘   └────────────────────────┘       │
│          ▲                        ▲                       │
│          │                        │                       │
│  ┌───────┴────────────────────────┴───────────────┐       │
│  │ 全局变量(g_android_ip/g_android_port            │       │
│  │ /g_android_connected)                          │       │
│  └────────────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────────┘
```

- **控制通道**: TCP 8080（二进制协议，一问一答）
- **视频通道**: UDP（ESP32 → Android，分片传输）
- **自动发现**: UDP 广播 8081

---

## 二、TCP 二进制协议

### 2.1 包格式

```
┌──────────────────────────────────────────────────────────┐
│  LENGTH(4) │  CMD(1)  │  SEQ(2)  │  PAYLOAD(可变)        │
│  little-endian                                          │
│  total = CMD + SEQ + PAYLOAD 字节数                      │
└─────────────────────────────────────────────────────────-┘
```

- **LENGTH**: uint32 LE，`3 + PAYLOAD` 字节数（CMD 1B + SEQ 2B + payload）
- **CMD**: uint8 命令码
- **SEQ**: uint16 LE，请求时递增，响应带回相同 SEQ
- **PAYLOAD**: 变长数据

### 2.2 命令码

| 命令 | 码 | 方向 | Payload | 响应 |
|------|---|------|---------|------|
| IsValid | 0x01 | A→E | 无 | 0x01 空 |
| GetWeather | 0x02 | A→E | 无 | 36B 天气数据 |
| GetSingleImage | 0x03 | A→E | 无 | 未实现(0xFF+0x03) |
| CityInfo | 0x04 | A→E | 无 | 32B 城市名 |
| StartCamera | 0x10 | A→E | 无 | 0x10 空 |
| StopCamera | 0x11 | A→E | 无 | 0x11 空 |
| StreamStartUdp | 0x20 | A→E | 2B LE Android UDP端口 | 5B (width 2B + height 2B + quality 1B) |
| StreamStopUdp | 0x21 | A→E | 无 | 0x21 空 |
| AppExit | 0xF0 | E→A | 无 | 无（通知型） |
| Error | 0xFF | E→A | 2B (原始CMD + 错误码) | — |

### 2.3 通信流程

```
Android                          ESP32
  │                                │
  │──── TCP connect :8080 ────────→│ g_android_connected=1
  │                                │
  │──── IsValid(0x01) ────────────→│
  │←─── IsValid resp ─────────────│
  │                                │
  │──── StartCamera(0x10) ────────→│ （stub）
  │←─── StartCamera resp ─────────│
  │                                │
  │─┐ udp.start()                 │
  │─┤ getLocalPort() → 47758      │
  │─┘                             │
  │──── StreamStartUdp(0x20) ─────→│ 发送 port=47758
  │    payload: [0xAE, 0xBA]      │ g_android_ip/g_android_port 写入
  │←─── StreamStartUdp resp ──────│ 5B: 640, 480, 12
  │    (640x480 quality=12)       │
  │                                │
  │    connected=true              │
  │                                │
  │         ←──── UDP视频帧 ──────│ stream_task 开始推流
  │                                │
  │←─── AppExit(0xF0) ────────────│ 用户长按退出时发送
  │                                │
  │──── TCP 断开 ─────────────────→│ g_android_ip/port/connected=0
```

### 2.4 关键文件

| 文件 | 角色 |
|------|------|
| `comms_server.c/h` | ESP32 TCP 服务器（监听/分发/响应） |
| `comms_server.h` | 导出 `g_android_ip` / `g_android_port` / `g_android_connected` |
| `Esp32Client.kt` | Android TCP 客户端（连接/发送/响应匹配） |
| `Esp32Protocol.kt` | Android 协议定义（Cmd 枚举 / Packet / 打包/解包） |

---

## 三、UDP 视频帧协议

### 3.1 分片格式

```
┌──────────────────────────────────────────────────────────┐
│  FRAME_ID(4) │  TIMESTAMP(4) │  FRAG_ID(2) │  FRAG_TOTAL(2) │  JPEG DATA
│  LE           │  LE           │  LE         │  LE            │  变长
├──────────────┴───────────────┴─────────────┴───────────────┴──────────┤
│ 12 字节头                                                             │
└──────────────────────────────────────────────────────────────────────┘
```

- **FRAME_ID**: uint32 LE，帧序号，从 0 递增
- **TIMESTAMP**: uint32 LE，毫秒时间戳（`esp_timer_get_time()/1000`）
- **FRAG_ID**: uint16 LE，当前分片序号（0-based）
- **FRAG_TOTAL**: uint16 LE，总分片数
- **JPEG DATA**: 每片最大 1388 字节

### 3.2 分片逻辑

```c
// 每个 JPEG 帧拆成多个 1388 字节的分片发送
int frag_total = (jpeg_len + 1388 - 1) / 1388;
for (int frag_id = 0; offset < jpeg_len; frag_id++) {
    size_t chunk = min(1388, jpeg_len - offset);
    memcpy(pkt + 12, jpeg + offset, chunk);  // 12字节头+JPEG数据
    sendto(udp_fd, pkt, 12 + chunk, 0, &dest, sizeof(dest));
    offset += chunk;
    vTaskDelay(8 / portTICK_PERIOD_MS);  // 8ms间隔防WiFi缓冲溢出
}
```

### 3.3 帧重组（Android）

```kotlin
// UdpFrameReceiver.onFragment
if (frag.frameId != currentFrameId) {
    // 新帧开始，检查上一帧是否完整
    if (frameBuf.isNotEmpty()) {
        _droppedFrames++  // 上一帧不完整，丢弃
    }
    currentFrameId = frag.frameId
    frameBuf.clear()
    fragTotal = frag.fragTotal
}
frameBuf[frag.fragId] = frag.jpeg
if (frameBuf.size == fragTotal) {
    // 帧完整，拼装JPEG并送入Channel
    val jpeg = concatFragments(frameBuf)
    _frames.trySend(jpeg)
}
```

### 3.4 关键文件

| 文件 | 角色 |
|------|------|
| `app_camera.c` | ESP32 stream_task（抓帧/分片/发送） |
| `UdpFrameReceiver.kt` | Android 接收/重组/送显 |

---

## 四、自动发现协议

### 4.1 流程

```
Android                          ESP32
  │                                │
  │── UDP广播 "ESP32?" ──:8081───→│ discovery_start()
  │                                │ 收到 "ESP32?" 后
  │←── UDP单播 "ESP32:IP" ───────│ 回复本机 STA IP
  │                                │
  │ 解析 IP，TCP connect :8080    │
```

### 4.2 关键文件

| 文件 | 角色 |
|------|------|
| `discovery.c` | ESP32 UDP 发现响应 |
| `Esp32Discovery.kt` | Android UDP 发现客户端 |

---

## 五、Android 端核心组件

| 类 | 职责 |
|----|------|
| `Esp32Client` | TCP 连接、发送请求、等待响应、事件推送 |
| `UdpFrameReceiver` | UDP 接收、帧重组、Channel 输出 |
| `Esp32Discovery` | UDP 广播自动发现 |
| `Esp32Protocol` | 协议常量和序列化工具 |
| `Esp32CameraScreen` | Compose UI、连接/显示/FPS/丢包统计 |

### UdpFrameReceiver 状态重置

每次 `start()` 或 `stop()` 调用时，`_totalFrames` 和 `_droppedFrames` 重置为 0，防止跨会话累计。

---

## 六、ESP32 关键文件

| 文件 | 职责 |
|------|------|
| `comms_server.c` | TCP Server 循环（listen/accept/read/dispatch） |
| `comms_server.h` | 全局变量导出 |
| `discovery.c` | UDP 发现响应 |
| `app_camera.c` | Camera APP 全生命周期（UI/推流/TFT/健康检查） |

---

## 七、全局变量（跨组件通信）

```c
// comms_server.h 导出，app_camera.c 读取
extern volatile int g_android_connected;  // 1=有TCP连接
extern uint32_t g_android_ip;             // Android STA IP（StreamStartUdp时设置）
extern uint16_t g_android_port;           // Android UDP端口（StreamStartUdp时设置）
```

### 生命周期

| 事件 | g_android_connected | g_android_ip | g_android_port |
|------|-------------------|--------------|----------------|
| TCP 连接 | 1 | 不变 | 不变 |
| StreamStartUdp 收到 | 1 | 写入 | 写入 |
| TCP 断开 | 0 | 清零 | 清零 |

### 使用位置

- `try_start_stream()`: 等待 `g_android_connected && g_android_port` 后才启动摄像头
- `stream_task()`: 每帧检查 `g_android_connected`，断连即停流
- `stream_task()`: 使用 `g_android_ip:g_android_port` 作为 UDP 目标

---

## 八、帧率与丢包优化历程

| 调整 | 分片间隔 | 帧间隔 | JPEG质量 | 效果 |
|------|---------|-------|---------|------|
| 初始 | 0ms | 0ms | 10 | ~15fps, ~70%丢包 |
| +分片间隔 | 2ms | 50ms | 10 | ~10fps, ~50%丢包 |
| +分片间隔 | 5ms | 100ms | 10 | ~5fps, ~30%丢包 |
| 去帧间隔 | 3ms | 无 | 10 | ~7fps, ~60%丢包 |
| 最终 | 8ms | 无 | 15 | ~2-3fps, <10%丢包 (期望值) |