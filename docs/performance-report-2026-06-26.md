# ESP32-CAM (ESP32-S3) 性能问题诊断报告

> 日期: 2026-06-26
> 项目: esp32-cam-app (IDF v5.5.2, target: esp32s3)
> 构建: CPU@240MHz, PSRAM@80MHz (8MB Octal), Flash@80MHz DIO

---

## 🔴 BUG 级（影响正确性）

### 1. `joy_event_task` 重复创建

**文件**: `main/main.c:98` 和 `main/main.c:115`
**严重度**: 🔴 高

第 98 行创建了 `joy_event_task` 后，第 115 行**再次用完全相同的参数**创建同名任务。第二次 `xTaskCreatePinnedToCore` 因任务名 `"joy_evt"` 已存在而返回 `pdFAIL`，但无法从代码中体现（返回值被丢弃）。浪费 4KB 栈空间。

### 2. 未使用变量 warning

**文件**: `components/app_settings/app_settings.c:417`
**严重度**: 🟡 低

```c
int depth = ui_get_depth();  // 声明后从未读取
```

编译器已报 `[-Wunused-variable]`，虽不影响运行但指示代码中存在冗余。

---

## 🟠 性能瓶颈

### 3. LVGL `full_refresh=1` + 全帧 PSRAM 缓冲 — SPI 总线满载

**文件**: `components/ui_lvgl/ui_lvgl.c:64-80`
**严重度**: 🔴 高

- 分配 **115,200 字节**（240×240×2）全帧缓冲区在 **PSRAM** (`MALLOC_CAP_SPIRAM`)
- `full_refresh = 1` 每帧全帧刷新
- 60MHz SPI 总线理论带宽 ~7.5MB/s，实际 ~5-6MB/s
- **115KB / 帧 → SPI 占用约 20ms，与刷新周期 (20ms) 持平**
- 每帧 6 次 DMA 传输（`max_transfer_sz=19200`），启停开销叠加

**影响**: 动画期间（卡片弹跳、淡入淡出）SPI 总线无空闲，帧率会显著下降。任何其他 SPI 操作（若有）都会阻塞。

### 4. 全帧缓冲在 PSRAM 而非内部 SRAM

**文件**: `components/ui_lvgl/ui_lvgl.c:64`
**严重度**: 🟠 中

`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 将全帧缓冲放在 PSRAM。PSRAM 访问延迟 ~70ns，内部 SRAM 约 4ns。LVGL 的 `lv_task_handler()` 每 5ms 读写该缓冲，PSRAM 延迟会拖慢 flush 回调。

**建议**: LVGL flush 缓冲如果用内部 SRAM（`MALLOC_CAP_INTERNAL`）效果更好，但 ESP32-S3 内部 SRAM 仅 512KB，需权衡。

### 5. ADC 使用旧版 Legacy API + 阻塞采样

**文件**: `components/input_joystick/joystick.c:87-88, 179-183`
**严重度**: 🟠 中

- 使用 `adc1_config_width()` / `adc1_get_raw()`（IDF 5.x legacy API）
- 每 30ms 轮询 2 个 ADC 通道 + 3 次 SW 采样（各延时 2ms）
- **每轮约 6ms** 用于阻塞 ADC 转换 + 消抖延时

**建议**: 改用 `adc_oneshot` 新驱动 + 非阻塞轮询，或将消抖改用时间戳计数而非 `vTaskDelay`。

### 6. SPI 传输分片过多

**文件**: `components/display_lcd/lcd_st7789.c:35`
**严重度**: 🟡 低

```c
.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t) / 6  // = 19200 bytes
```

全帧 115200 字节需 **6 次 DMA 传输**。每次 DMA 有 SPI 总线启停和中断开销。增大至 `/2` 或 `/3` 可减少到 2-3 次传输。

---

## 🟡 架构风险

### 7. TCP Server 无超时阻塞

**文件**: `components/comms_server/comms_server.c:131`
**严重度**: 🟠 中

`read(s_client_fd, buf, ...)` 无 `setsockopt(SO_RCVTIMEO)`。客户端异常断开后不发送 FIN 时，任务永久阻塞。4KB 任务栈无法回收。

### 8. HTTP Handler 内忙等 5 秒

**文件**: `components/web_config_server/web_config_server.c:117-126`
**严重度**: 🟠 中

`handler_wifi_test` 在 esp_http_server 线程中轮询 WiFi 连接最多 5 秒（50×100ms）。期间 HTTP 服务器无法处理其他请求。

### 9. 跨任务全局变量无并发保护

**文件**: 多处 (`main.c`, `ui_main.h`, `web_config_server.c`)
**严重度**: 🟡 低

- `s_wifi_connected`：WiFi 事件回调（系统任务）写入、HTTP handler 读取、摇杆事件任务读取
- `s_sta_ip`：同上

当前场景下仅写入一次，无数据竞争，但仍是隐患。建议加 `volatile` + 内存屏障或互斥锁。

### 10. 部分任务未固定核心

**文件**: 
- `comms_server.c:154` — `xTaskCreate()` 无核心固定
- `joystick.c:220` — `xTaskCreate()` 无核心固定

未指定 core 的任务可能被 FreeRTOS 调度器在双核间迁移，影响缓存命中率。

### 11. TCP comms 任务栈尺寸偏紧

**文件**: `comms_server.c:154` — 栈 4096 字节

TCP 任务含 LWIP socket 调用 + cJSON 解析。cJSON 栈使用较高，LWIP TCP 连接状态机也在同一任务中，4096 可能不足。

| 任务 | 栈大小 | 核心 | 优先级 |
|------|--------|------|--------|
| `lv_task` (LVGL) | 4096 | 0 | 3 |
| `lv_tick` | 2048 | 1 | 1 |
| `joy_poll` | 3072 | 未指定 | 5 |
| `joy_evt` | 4096 | 1 | 4 |
| `comms` (TCP) | 4096 | 未指定 | 4 |

---

## 🔵 配置级优化建议

### 12. Flash DIO 模式而非 QIO

**文件**: `sdkconfig:555`
`CONFIG_ESPTOOLPY_FLASHMODE_DIO` （2线）→ **QIO**（4线）可提升 2x 闪存读取带宽。需确认硬件支持。

### 13. WiFi 动态 RX 缓冲区偏大

**文件**: `sdkconfig:1382`
`CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32` → 对 4 客户端 AP 场景，16 足够。

### 14. SoftAP Beacon 最大长度过大

**文件**: `sdkconfig:1400`
`CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN=752` → SSID 仅 13 字节，降至 256 即可。

### 15. 数据缓存行大小可提升

**文件**: `sdkconfig:1280`
`CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=32` → 可提升至 64 字节提高大块 DMA 数据的 cache 命中率（需注意 memory overhead）。

### 16. LVGL 刷新周期被覆盖

**文件**: `components/ui_lvgl/CMakeLists.txt:3`
`LV_DISP_DEF_REFR_PERIOD=20` 覆盖了 sdkconfig 默认的 `CONFIG_LV_DISP_DEF_REFR_PERIOD=10`。20ms = 50fps，与 SPI 承载能力匹配，但若全帧刷新每次需 ~20ms，刷新周期可放宽至 30ms 换取 CPU 空闲。

---

## 📊 优先级总览

| 优先级 | 问题 | 影响 | 修复预估 |
|--------|------|------|----------|
| P0 | 事件任务重复创建 | 功能 Bug | 1 行删除 |
| P1 | full_refresh + PSRAM 缓冲 | 帧率卡顿 | 中等（改用局部刷新或缩减缓冲） |
| P1 | SPI 传输分片 6 次 | 帧率开销 | 1 行改参数 |
| P2 | TCP 无超时阻塞 | 资源泄漏 | 3 行加 SO_RCVTIMEO |
| P2 | HTTP handler 忙等 5s | 阻塞其他请求 | 改用异步回调 |
| P3 | ADC legacy API + 阻塞采样 | 轮询效率 | 迁移新驱动 |
| P3 | Flash DIO vs QIO | 带宽 | 确认硬件后改配置 |
| P4 | 未使用变量、冗余配置、核心固定 | 代码质量 | 低风险随手修复 |
