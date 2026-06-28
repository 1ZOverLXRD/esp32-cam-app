# ESP32 二进制协议实现计划

## 概述

将 `comms_server.c` 从 JSON 行协议重写为二进制包协议，匹配 Android 端 `Esp32Client` + `Esp32Protocol`。

## 协议回顾

**TCP 包格式（7 字节头）：**
```
┌──────────────┬──────────┬──────────┬─────────────────┐
│ LENGTH(4B LE)│ CMD(1B)  │ SEQ(2B)  │ PAYLOAD(N B LE) │
└──────────────┴──────────┴──────────┴─────────────────┘
LENGTH = CMD(1) + SEQ(2) + PAYLOAD(N) 的总字节数
```

**UDP 帧格式（12 字节头）：**
```
┌───────────┬───────────────┬──────────┬──────────────┐
│ FRAME_ID  │ TIMESTAMP     │ FRAG_ID  │ FRAG_TOTAL   │
│ 4B LE     │ 4B LE         │ 2B LE    │ 2B LE        │
└───────────┴───────────────┴──────────┴──────────────┘
```

**CMD 表：**
| CMD | 名称 | 请求 | 响应 |
|-----|------|------|------|
| 0x01 | IsValid | - | - |
| 0x02 | GetWeather | - | temp(2)+hum(1)+code(1)+city(32) |
| 0x03 | GetSingleImage | - | JPEG |
| 0x04 | CityInfo | - | city(32) |
| 0x10 | StartCamera | - | - |
| 0x11 | StopCamera | - | - |
| 0x20 | StreamStartUdp | Android端口(2) | width(2)+height(2)+quality(1) |
| 0x21 | StreamStopUdp | - | - |
| 0xFF | Error | - | 原CMD(1)+错误码(1) |

## 实现步骤

### Task 1: 替换 comms_server.c 为二进制协议

**文件：** `components/comms_server/comms_server.c`

**改动：**
1. 删 cJSON 相关全部代码（不再需要 JSON 解析）
2. 新增 `read_packet()` — 先读 4 字节 LENGTH，再读 LENGTH 字节数据
3. 新增 `send_response()` — 写 4 字节 LENGTH + CMD + SEQ + payload
4. 新增 `send_error()` — 写 0xFF 错误包
5. 重写主循环：`read(4)` → 得 LENGTH → `read(LEN)` → 解析 CMD+SEQ → 分发

```c
// 读一个完整二进制包，返回 0=成功, -1=断开
static int read_packet(int fd, uint8_t *cmd, uint16_t *seq, 
                        uint8_t *buf, uint32_t *len) {
    uint8_t header[7];
    int n = read(fd, header, 7);
    if (n <= 0) return -1;
    
    uint32_t pktlen = header[0] | (header[1]<<8) | (header[2]<<16) | (header[3]<<24);
    *cmd = header[4];
    *seq = header[5] | (header[6]<<8);
    
    if (pktlen > 3) {  // CMD(1) + SEQ(2) 之外还有 payload
        uint32_t to_read = pktlen - 3;
        uint32_t offset = 0;
        while (offset < to_read) {
            n = read(fd, buf + offset, to_read - offset);
            if (n <= 0) return -1;
            offset += n;
        }
        *len = to_read;
    } else {
        *len = 0;
    }
    return 0;
}

static void send_response(int fd, uint8_t cmd, uint16_t seq, 
                           const uint8_t *payload, uint32_t plen) {
    uint32_t total = 3 + plen;  // CMD + SEQ + payload
    uint8_t header[7];
    header[0] = total & 0xFF;
    header[1] = (total >> 8) & 0xFF;
    header[2] = (total >> 16) & 0xFF;
    header[3] = (total >> 24) & 0xFF;
    header[4] = cmd;
    header[5] = seq & 0xFF;
    header[6] = (seq >> 8) & 0xFF;
    
    write(fd, header, 7);
    if (plen > 0) write(fd, payload, plen);
}
```

6. CMD 分发 switch-case：

```c
static void handle_packet(int fd, uint8_t cmd, uint16_t seq, 
                           const uint8_t *payload, uint32_t plen) {
    switch (cmd) {
    case 0x01: // IsValid
        send_response(fd, 0x01, seq, NULL, 0);
        break;
        
    case 0x02: // GetWeather
        // ESP32 无互联网，回默认数据
        {
            uint8_t resp[36];
            memset(resp, 0, sizeof(resp));
            // temp=25.0°C (2500 = 0x09C4), hum=60, code=0 (晴)
            resp[0] = 0xC4; resp[1] = 0x09;
            resp[2] = 60;   // hum
            resp[3] = 0;    // code
            strcpy((char*)resp + 4, "Zhuzhou"); // city
            send_response(fd, 0x02, seq, resp, sizeof(resp));
        }
        break;
        
    case 0x03: // GetSingleImage
        // TODO: 从 OV5640 取一帧 JPEG，发回
        send_error(fd, seq, 0x03, 0x03); // 资源不可用（尚未实现）
        break;
        
    case 0x04: // CityInfo — 发当前城市名
        {
            uint8_t city[32] = {0};
            strcpy((char*)city, "Zhuzhou");
            send_response(fd, 0x04, seq, city, 32);
        }
        break;
        
    case 0x10: // StartCamera
        // TODO: 初始化 OV5640
        send_response(fd, 0x10, seq, NULL, 0);
        break;
        
    case 0x11: // StopCamera
        // TODO: 关闭 OV5640
        send_response(fd, 0x11, seq, NULL, 0);
        break;
        
    case 0x20: // StreamStartUdp
        // payload 前 2 字节 = Android UDP 端口
        if (plen >= 2) {
            uint16_t android_port = payload[0] | (payload[1] << 8);
            // TODO: 保存 android_port，启动推流任务
            uint8_t info[5];
            info[0] = 640 & 0xFF; info[1] = (640 >> 8) & 0xFF;   // width
            info[2] = 480 & 0xFF; info[3] = (480 >> 8) & 0xFF;   // height
            info[4] = 12;  // quality
            send_response(fd, 0x20, seq, info, 5);
        }
        break;
        
    case 0x21: // StreamStopUdp
        // TODO: 停止推流
        send_response(fd, 0x21, seq, NULL, 0);
        break;
        
    default:
        send_error(fd, seq, cmd, 0x01); // 未知命令
        break;
    }
}

static void send_error(int fd, uint16_t seq, uint8_t orig_cmd, uint8_t err) {
    uint8_t payload[2] = {orig_cmd, err};
    send_response(fd, 0xFF, seq, payload, 2);
}
```

7. 主循环改为二进制包模式：

```c
while (1) {
    uint8_t cmd, payload[512];
    uint16_t seq;
    uint32_t plen = 0;
    
    if (read_packet(s_client_fd, &cmd, &seq, payload, &plen) < 0)
        break;
        
    handle_packet(s_client_fd, cmd, seq, payload, plen);
}
```

### Task 2: CMakeLists — 删除 cJSON 依赖

**文件：** `components/comms_server/CMakeLists.txt`

```cmake
idf_component_register(SRCS "comms_server.c"
                    INCLUDE_DIRS ".")
```

### 待完成项（后续 Task）

- OV5640 初始化与 JPEG 捕获
- UDP 推流 socket + 分片发送
- 城市名切换逻辑

## 编译验证

```bash
cmd.exe //c "F:\Cache\Hermes\scripts\build_check.bat" 2>&1 | tail -5
```
Expected: BUILD_EXIT:0
