# ESP32 ↔ Android 通讯协议设计（v2 — 采纳 Android 端审查意见）

## 架构

```
TCP 控制通道（:8080）  ─── 可靠、一问一答：IsValid / GetWeather / StartCamera / StopCamera
UDP 视频通道（端口协商）─── 实时、无重传：OV5640 JPEG 帧分片推送
```

---

## 一、TCP 控制通道

### 数据包头（7 字节）

```
┌────────────────┬──────────┬──────────┬─────────────────┐
│ LENGTH (4B LE) │ CMD (1B) │ SEQ (2B) │ PAYLOAD (可选)   │
│ 不含自身长度   │ 命令码   │ 序号     │ 按 CMD 定义      │
└────────────────┴──────────┴──────────┴─────────────────┘
```

| 字段 | 长度 | 说明 |
|------|------|------|
| `LENGTH` | 4B | payload 字节数（小端），不含这 7 字节头 |
| `CMD` | 1B | 命令码 |
| `SEQ` | 2B | 请求序号（小端），响应原样返回，用以匹配请求和响应 |

**读包流程：**
```
read(4)    → LENGTH
read(3)    → CMD + SEQ
read(LEN)  → PAYLOAD（如果有）
```

### 命令表

| HEX | 名称 | 方向 | PAYLOAD(请求) | PAYLOAD(响应) | 说明 |
|-----|------|------|---------------|---------------|------|
| `0x01` | IsValid | 双向 | (无) | (无) | 空包握手 |
| `0x02` | GetWeather | 请求→响应 | (无) | temp(2B int16/100) + hum(1B uint8) + code(1B int8) | 温度精确到0.01°C |
| `0x03` | GetSingleImage | 请求→响应 | (无) | JPEG 裸数据 | 拍一张照片 |
| `0x10` | StartCamera | 请求→响应 | (无) | (无) | 启动摄像头（上电+初始化） |
| `0x11` | StopCamera | 请求→响应 | (无) | (无) | 关闭摄像头（省电） |
| `0x20` | StreamStartUdp | 请求→响应 | Android UDP 端口(2B LE) | width(2B) + height(2B) + quality(1B) | 协商 UDP 推流参数 |
| `0xFF` | Error | 响应 | — | 失败原CMD(1B) + 错误码(1B) | 错误报告 |

### SEQ 说明

SEQ 自增（每次请求 +1，从 1 开始）。Android 发请求时填一个序号，ESP32 响应时原样带回。这样同时发多个请求也能明确配对。

**示例：**

```
→ GetWeather #1:   [LEN:0x00000001][CMD:0x02][SEQ:0x0001]
→ GetWeather #2:   [LEN:0x00000001][CMD:0x02][SEQ:0x0002]
← 响应 #1:         [LEN:0x00000005][CMD:0x02][SEQ:0x0001][temp×2][hum][code]
← 响应 #2:         [LEN:0x00000005][CMD:0x02][SEQ:0x0002][temp×2][hum][code]
```

### 错误码

| 码 | 含义 |
|----|------|
| `0x00` | 成功 |
| `0x01` | 未知命令 |
| `0x02` | 内部错误 |
| `0x03` | 资源不可用 |

---

## 二、UDP 视频通道

### UDP 数据包头（12 字节）

```
┌───────────┬───────────────┬──────────┬──────────────┐
│ FRAME_ID  │ TIMESTAMP     │ FRAG_ID  │ FRAG_TOTAL   │
│ 4B LE     │ 4B LE         │ 2B LE    │ 2B LE        │
└───────────┴───────────────┴──────────┴──────────────┘
```

| 字段 | 长度 | 说明 |
|------|------|------|
| `FRAME_ID` | 4B | 帧序号，每帧 +1。4B 空间可跑 4.5 年不溢出（30FPS） |
| `TIMESTAMP` | 4B | `esp_timer_get_time() / 1000`（毫秒），用于端到端延迟统计、录像同步 |
| `FRAG_ID` | 2B | 当前分片索引（从 0 开始） |
| `FRAG_TOTAL` | 2B | 完整帧的总分片数 |

每片 UDP payload = 12 字节头 + JPEG 分片数据（≤ 1400 字节）。

### Android 收流逻辑

```
收到 UDP 包 → 读 FRAME_ID

if FRAME_ID != 当前正在拼的帧：
    如果上一帧没凑齐 → 丢弃（丢帧容忍）
    重置缓冲区
    预期分片数 = FRAG_TOTAL
    开始时间 = TIMESTAMP

按 FRAG_ID 填入缓冲区对应位置

当收到的分片数 == FRAG_TOTAL:
    JPEG 到手 → 解码显示
    记录 TIMESTAMP → FPS / 延迟统计
```

---

## 三、建议 Android 端实现

```kotlin
// TCP 控制通道
class TcpClient(host: String, port: Int = 8080) {
    private var seq = 0
    private val pending = ConcurrentHashMap<Int, CompletableFuture<Packet>>()

    fun connect()
    fun send(cmd: Byte, payload: ByteArray = byteArrayOf()): Packet {
        val s = ++seq
        // write LENGTH(4) + CMD(1) + SEQ(2) + payload
        // 注册 pending[s] = future
        // 读线程匹配响应 CMD+SEQ 到 future
        return future.get()
    }
}

// UDP 接收通道
class UdpReceiver(localPort: Int) {
    data class VideoFrame(
        val frameId: Long,
        val timestamp: Long,
        val jpeg: ByteArray
    )

    private val frameBuf = mutableMapOf<Long, MutableList<ByteArray?>>()
    private val callback: (VideoFrame) -> Unit

    fun onPacket(data: ByteArray) {
        // 解析 12 字节头
        val frameId = readLE32(data, 0)
        val timestamp = readLE32(data, 4)
        val fragId = readLE16(data, 8)
        val fragTotal = readLE16(data, 10)
        val jpeg = data.copyOfRange(12, data.size)

        // 拼帧，凑齐后回调
    }
}
```

---

## 四、完整交互流程

```
1. Android 连接 TCP:8080
2. IsValid     → 确认连通
3. StartCamera → 初始化 OV5640
4. GetSingleImage → 拍一张测试照
5. StreamStartUdp (Port=9001) → ESP32 回 width/height/quality
6. ESP32 开始往 Android:9001 发 UDP 分片
7. Android 收 UDP、拼帧、显示 30FPS 推流
8. 用户关闭 → StreamStopUdp (通过 TCP)
9. StopCamera → 关摄像头省电
```