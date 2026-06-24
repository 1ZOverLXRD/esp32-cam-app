# ESP32-CAM Smart Desktop — 性能优化设计文档

## 1. 概述

对 `esp32-cam-app` （ESP32-S3 + OV5640 + ST7789 240x240 + LVGL 桌面）进行系统级性能审计和优化。目标是消除所有 P0 级的阻塞性缺陷，建立可推流的 MJPEG 传输路径，优化 LWIP/SPI/PSRAM 配置至生产级别。

## 2. 当前架构问题

当前项目具有完整的 LVGL 菜单 UI（6 App carousel）、摇杆导航、TCP 8080 与 Android 通信、Web 80 配置页面，但**摄像头功能完全缺失**，网络和显示底层存在多处子优配置。

### 2.1 组件依赖图（当前）

```
app_main()
├── lcd_st7789_init()      — SPI @40MHz, queue_size=1
├── joystick_init()        — ADC1 poll @50ms
├── ui_lvgl_init()         — 20行双缓冲, 30ms刷新
├── ui_main_menu_init()    — 6卡片 Carousel
├── comms_server_init()    — TCP 8080 单线程阻塞
└── web_config_server_start() — HTTP 80, stack=4096
```

**摄像头模块、MJPEG 推流均不存在。**

### 2.2 SDKConfig 关键缺陷

| 配置 | 当前值 | 目标值 | 影响 |
|------|--------|--------|------|
| `ESPTOOLPY_FLASHMODE` | DIO | QIO | Flash 读取带宽减半 |
| `LWIP_TCP_SND_BUF_DEFAULT` | 5760 | 24576 | TCP 发送背压 |
| `LWIP_TCP_WND_DEFAULT` | 5760 | 24576 | TCP 接收窗口过小 |
| `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 未设 | 64 | WiFi TX 拥塞 |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | 4096 | 16384 | 网络缓冲挤到 PSRAM |
| `SPIRAM_FETCH_INSTRUCTIONS` | 未设 | y | flash→PSRAM 指令缓存 |
| `SPIRAM_RODATA` | 未设 | y | flash→PSRAM 只读数据 |

## 3. 优化方案 — 分阶段执行

### Phase 1: sdkconfig 与底层优化（可独立烧录验证）

在**不动代码逻辑**的前提下，修改 sdkconfig 开启所有底层性能选项。此阶段改完可直接 `idf.py build` 烧录验证基础系统运行正常。

**改动清单：**

1. `ESPTOOLPY_FLASHMODE_DIO=y` → `ESPTOOLPY_FLASHMODE_QIO=y`
2. `LWIP_TCP_SND_BUF_DEFAULT=5760` → `24576`
3. `LWIP_TCP_WND_DEFAULT=5760` → `24576`
4. `SPIRAM_MALLOC_ALWAYSINTERNAL=4096` → `16384`
5. `SPIRAM_FETCH_INSTRUCTIONS=n` → `y`
6. `SPIRAM_RODATA=n` → `y`
7. `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32`（默认）→ `64`
8. `LV_DISP_DEF_REFR_PERIOD=30` → `20`
9. 新增 `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32` → `64`

### Phase 2: 显示子系统优化（SPI 流水线 + 栈安全）

**改动文件：**
- `components/display_lcd/lcd_st7789.c`
- `components/web_config_server/web_config_server.c`

**具体修改：**

10. SPI `queue_size=1` → `3`（允许 3 笔传输排队，LVGL flush 不阻塞）
11. Web 服务器 `stack_size=4096` → `8192`

### Phase 3: TCP 通信服务器重构（多路复用）

**改动文件：**
- `components/comms_server/comms_server.c`

**具体修改：**

12. 单线程阻塞 accept → 使用 `select()` 多路复用，同时监听原 socket + 已连接 client
13. 或：独立的 accept 任务 + 每个连接 spawn 临时阅读任务（受 PSRAM 限制需控制最大连接数 ≤3）
14. 接收缓冲 `BUF_SIZE=512` → `2048`
15. 为每条客户端消息添加最大长度限制（4KB），防止恶意 OOM

### Phase 4: 摄像头初始化 + MJPEG 推流

**新增文件：**
- `components/camera_streamer/camera_streamer.c` + `camera_streamer.h` + `CMakeLists.txt`

**改动文件：**
- `main/main.c`（添加 camera init + 注册 streamer）
- `web_config_server/web_config_server.c`（添加 `/stream`, `/capture` 路由）

**具体实现：**

16. 摄像头配置：OV5640 @ XCLK=16MHz, PIXFORMAT_JPEG, FRAMESIZE_HD(720p), jpeg_quality=16, fb_count=3, grab_mode=CAMERA_GRAB_LATEST
17. MJPEG-over-HTTP 推流：`/stream` 端点返回 multipart/x-mixed-replace
18. `/capture`：单帧高质量 JPEG 快照
19. `/ctrl`：POST 切换分辨率（QVGA/HD）

### Phase 5: 健壮性优化（可选）

20. LED 闪烁从轮询任务改为硬件定时器或睡眠触发
21. 摇杆 SW 启用 GPIO 中断（减少轮询延迟）
22. 添加任务看门狗超时恢复
23. 移除 LVGL 动画 `lv_anim_path_overshoot`/`bounce` 等高开销路径曲线（改为 `ease_out`）

## 4. 验证策略

| Phase | 验证方法 | 预期结果 |
|-------|---------|---------|
| 1 | `idf.py build` 编译 + 烧录 | 系统正常启动，菜单可用，AP 可连 |
| 2 | 编译烧录 | 菜单动画无明显卡顿，Web 配置正常 |
| 3 | 启动后连 TCP 8080 发 JSON | Android 发送/接收消息正常 |
| 4 | 浏览器打开 `http://192.168.4.1/stream` | 720p 15fps+ 流畅画面 |
| 5 | 整机运行 1 小时 | 无重启/卡死 |

## 5. 风险与回退

- **QIO 模式**：少量 ESP32-S3 模组 Flash 不支持 QIO，烧录后无法启动 → 回退 DIO
- **SPIRAM_FETCH_INSTRUCTIONS**：PSRAM 在极端温度下不稳定，开启后偶发崩溃 → 关闭
- **摄像头 AF + PSRAM DMA 冲突**：已知 OV5640 AF 初始化与 `CONFIG_CAMERA_PSRAM_DMA` 冲突 → 先测 AF 再开 DMA
- **TCP select() 重构**：比单线程复杂，可能出现 `select` 返回但不触发事件的边缘情况 → 降级为 accept 线程 + worker 线程池模式

## 6. 不纳入范围

- 不重构 LVGL 菜单代码结构
- 不改摇杆物理电气设计（内部上拉保留）
- 不添加 OTA 升级功能
- 不重写 Web 配置页面 UI
