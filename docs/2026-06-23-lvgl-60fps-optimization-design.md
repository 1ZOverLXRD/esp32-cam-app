# ESP32-S3 + ST7789 + LVGL 60FPS 性能优化设计

## 背景

当前 ESP32-CAM 智能桌面项目使用 ESP32-S3 + ST7789（240x240，SPI）+ LVGL v8.4。滑动卡片菜单时 FPS 仅 ~12-20，明显卡顿，刷新率不足。空闲时 FPS 计数器显示 66 但画面有滞后感（伪帧）。

## 目标

- 静止界面：不浪费 SPI 带宽，空闲不触发无效刷新
- 滑动动画：稳定 **35+ FPS**（40MHz SPI）或 **60+ FPS**（80MHz SPI 可行时）
- 消除伪帧感：FPS 计数器反映真实可见帧率

## 关键发现

1. **CPU 频率仅 160MHz**（ESP32-S3 最高 240MHz），LVGL 纯软件渲染损失 50% 性能
2. **5 个独立卡片动画** — 每帧 LVGL 要分别处理 5 个移动对象
3. **SPI 传输与 LVGL 渲染串行** — CPU 等待 SPI 发完才开始渲染下一块
4. **lv_task 延时 20ms** + `REFR_PERIOD=30ms` 限制理论上限 33 FPS
5. **FPS 伪帧** — LVGL 在无变化时仍持续标记屏幕为 dirty（可能 hint 标签），计数器涨了但 SPI 发的是冗余帧

## 改动清单

### A. CPU 频率 + LVGL 刷新率 + Watchdog

| 文件 | 改动 |
|------|------|
| `sdkconfig` | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160` → `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240` |
| `sdkconfig` | `CONFIG_LV_DISP_DEF_REFR_PERIOD=30` → 10 |
| `sdkconfig` | `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5` → 10 |
| `ui_lvgl.c` | `vTaskDelay(20)` → `vTaskDelay(5)` |

### B. 父容器滚动代替多对象动画

**改动文件**：`ui_main.c`

**原理**：
- 创建 `s_scroll_container`（`lv_obj_create(lv_scr_act())`, `clip_corner=true`）作为所有卡片的父容器
- 卡片固定在容器内，间距 `STEP_X`
- 切换时只动画容器的 `x` 坐标（**1 个动画代替 5 个**）
- 中心/侧边卡的大小和透明度通过独立的 size/opa 动画调整
- 超出 -2/+2 范围的卡片 `HIDDEN`

### C. PSRAM 全屏帧缓冲 + 异步 DMA 双缓冲流水线

**改动文件**：`ui_lvgl.c`、`lcd_st7789.c`

#### C1. 缓冲架构

`full_refresh=1` 下 LVGL 自己管理一个全屏缓冲。我们只需要：

```
LVGL 渲染 → LVGL全屏缓冲 (PSRAM, 115KB, 由LVGL管理)
                ↓
         SRAM 弹跳缓冲 (20行×9.6KB, DMA-capable)
                ↓
         SPI DMA 发送 (后台)
```

- `full_refresh=1`：LVGL 只在全帧就绪后调用 flush，传入 `color_p` 指向 LVGL 内部缓冲
- **LVGL 全屏缓冲**：由 `lv_disp_draw_buf_init` 分配，`MALLOC_CAP_SPIRAM`
- **SRAM DMA 弹跳缓冲**：`MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`，`240×20×2=9600` 字节
- flush callback 把 `color_p`（PSRAM）拷贝到弹跳缓冲（SRAM），发 SPI，立即返回
- `lv_disp_flush_ready(drv)` 告诉 LVGL 缓冲已释放，可以画下一帧到同一缓冲

**安全保证**：
- 弹跳缓冲受信号量保护，SPI 传输期间新的 flush 等待
- LVGL 缓冲由 `lv_disp_flush_ready` 保护（LVGL 内部机制）

#### C2. 异步 flush 流程（关键修正）

当前 flush callback 是阻塞的——`spi_device_queue_trans` 等待 SPI 发完才返回。改为完全异步：

```
帧N flush 触发:
  1. 拷贝 PSRAM 缓冲 → SRAM 弹跳缓冲 (memcpy, ~0.3ms)
  2. spi_device_queue_trans(非阻塞, portMAX_DELAY → 0)
  3. lv_disp_flush_ready(drv) 立即返回  ← 让LVGL开始画下一帧
  4. SPI post-trans callback 中标记弹跳缓冲可用
     （通知帧N传输完成，不阻塞任何流程）

帧N+1 flush 触发:
  1. 等待弹跳缓冲可用（信号量/标志位）
  2. 拷贝帧N+1 → 弹跳缓冲
  3. 同上非阻塞 SPI...
```

**流水线时序（80MHz SPI）：**

```
时间: 0ms    10ms    20ms    30ms
CPU:  [画帧N→PSRAM] [画帧N+1→PSRAM] [画帧N+2→PSRAM]
SPI:  [空闲]    [发帧N-1]   [发帧N]    [发帧N+1]
```

SPI 和 CPU 完全并行，每帧延迟 = max(CPU渲染时间, SPI传输时间)。

#### C3. 弹跳缓冲同步

- 使用 `xSemaphoreCreateBinary()` 控制弹跳缓冲所有权
- flush callback：take 信号量（等待弹跳缓冲空闲）→ 拷贝 + 发起 SPI → give（在 post_cb 中）
- SPI 的 post_cb 在 ISR 上下文中运行，用 `xSemaphoreGiveFromISR`

#### C4. SPI 参数

- `max_transfer_sz`：`LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t) = 115200`
- SPI 时钟：优先尝试 **80MHz**；如花屏/不稳定，fallback 到 **40MHz**
- DMA：保持 `SPI_DMA_CH_AUTO`

## 预期效果

| 场景 | 当前 FPS | 优化后（40MHz SPI） | 优化后（80MHz SPI） |
|------|---------|-------------------|-------------------|
| 空闲 | 66（伪帧） | 0-5（几乎不刷） | 0-5 |
| 滑动动画 | 12-20 | **35-45** | **60-80** |

## 风险评估

- 80MHz SPI：部分 ST7789 模块不支持，需 fallback 至 40MHz
- PSRAM 缓冲 + DMA 弹跳缓冲增加 ~250KB 内存占用（PSRAM 8MB 充足，无影响）
- `full_refresh=1` 在部分场景可能增加响应延迟（完整帧才刷），但卡片动画是全帧变化因此无影响
- 异步 flush 增加代码复杂度，但 SPI post_cb 是标准 ESP-IDF 模式
