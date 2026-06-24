# LVGL 60FPS 性能优化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task.

**Goal:** 将 ESP32-S3 + ST7789 + LVGL 的滑动动画 FPS 从 12-20 提升到 40+（40MHz SPI）或 60+（80MHz SPI）

**Architecture:** 三项独立优化叠加——（A）CPU 频率 160→240MHz + LVGL 参数调优；（B）父容器滚动代替 5 对象动画，减少每帧渲染量；（C）flush callback 异步化，SPI 发送与 LVGL 渲染流水线并行

**Tech Stack:** ESP-IDF v5.5.2, LVGL 8.4.0, ESP32-S3, ST7789 (SPI)

## Global Constraints

- ESP32-S3 CPU 频率最终 = 240MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240`)
- LVGL_DISP_DEF_REFR_PERIOD = 10
- ESP_TASK_WDT_TIMEOUT_S = 10（防止 lv_task 5ms 循环误触发）
- SPI clock: 80MHz（首次尝试），如花屏降回 40MHz
- LVGL 缓冲保持现状：20 行 × 240px × 2 字节 = 9600 字节，`MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`
- 不改动 `panel_io_spi_tx_color` 或 IDF 内部代码

---

### Task 1: sdkconfig 参数调优

**Files:**
- Modify: `sdkconfig` (5 处)

- [ ] **Step 1: CPU 频率 160→240MHz**

```patch
- CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
- CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160
+ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
+ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
- CONFIG_ESP32S3_DEFAULT_CPU_FREQ_160=y
- CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ=160
+ CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
+ CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ=240
```

- [ ] **Step 2: LVGL 刷新周期 30→10ms**

```patch
- CONFIG_LV_DISP_DEF_REFR_PERIOD=30
+ CONFIG_LV_DISP_DEF_REFR_PERIOD=10
```

- [ ] **Step 3: Watchdog 超时 5→10s**

```bash
# 搜索 CONFIG_ESP_TASK_WDT_TIMEOUT_S，改为 10
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
```

- [ ] **Step 4: SPI 时钟 40→80MHz（尝试）**

修改 `lcd_st7789.c`：
```patch
- .pclk_hz = 40 * 1000 * 1000,
+ .pclk_hz = 80 * 1000 * 1000,
```

- [ ] **Step 5: 构建验证**

```bash
idf.py fullclean build
```
Expected: BUILD_EXIT:0. 如 80MHz 编译通过，保持该值；后续 Task 如测试发现花屏再降回 40MHz。

---

### Task 2: 父容器滚动代替 5 对象动画

**Files:**
- Modify: `components/ui_system/ui_main.c`（~40 行新增，~30 行删除）

**Interface changes:**
- 新增全局变量: `static lv_obj_t *s_scroll = NULL;` — 卡片父容器
- 移除: `update_card_appearance()` 中的逐卡 x 坐标位移动画部分

- [ ] **Step 1: 创建滚动容器**

在 `ui_main_menu_init()` 中，创建卡片前先创建容器：

```c
/* 滚动容器：所有卡片放里面，裁剪溢出 */
s_scroll = lv_obj_create(lv_scr_act());
lv_obj_set_size(s_scroll, 240, 240);        // 全屏大小
lv_obj_set_pos(s_scroll, 0, 0);
lv_obj_set_style_radius(s_scroll, 0, LV_STATE_DEFAULT);
lv_obj_set_style_border_width(s_scroll, 0, LV_STATE_DEFAULT);
lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, LV_STATE_DEFAULT);
lv_obj_set_style_clip_corner(s_scroll, true, LV_STATE_DEFAULT);
lv_obj_clear_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
```

修改 `create_card()`，父对象从 `lv_scr_act()` 改为 `s_scroll`：
```patch
- lv_obj_t *card = lv_obj_create(lv_scr_act());
+ lv_obj_t *card = lv_obj_create(s_scroll);
```

- [ ] **Step 2: 修改 `update_card_appearance()`**

保留卡片大小/透明度逻辑，**移除** `lv_obj_set_x` 位移动画逻辑。位置在切换时由容器滚动控制：

```c
static void update_card_appearance(void)
{
    for (int i = 0; i < TOTAL_CARDS; i++) {
        if (!s_cards[i]) continue;
        int offset = i - s_center_idx;

        if (offset < -2 || offset > 2) {
            lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);

        // 卡片在容器内的固定偏移位置（不再通过 lv_obj_set_x 动画）
        lv_obj_set_x(s_cards[i], offset * STEP_X);

        if (offset == 0) {
            lv_obj_set_size(s_cards[i], CARD_BIG_W, CARD_BIG_H);
            lv_obj_set_style_opa(s_cards[i], LV_OPA_COVER, LV_STATE_DEFAULT);
            lv_obj_set_style_shadow_width(s_cards[i], 10, LV_STATE_DEFAULT);
        } else {
            lv_obj_set_size(s_cards[i], CARD_SMALL_W, CARD_SMALL_H);
            lv_obj_set_style_opa(s_cards[i], LV_OPA_60, LV_STATE_DEFAULT);
            lv_obj_set_style_shadow_width(s_cards[i], 3, LV_STATE_DEFAULT);
        }
    }
}
```

- [ ] **Step 3: 修改 `animate_to_center()` — 改为容器滚动动画**

**移除**逐卡 x 坐标动画，改为容器 x 坐标动画 + 居中卡独立 size/opa 动画：

```c
static void animate_to_center(int new_center)
{
    int old_center = s_center_idx;
    s_center_idx = new_center;

    /* 更新卡片显隐状态 */
    for (int i = 0; i < TOTAL_CARDS; i++) {
        if (!s_cards[i]) continue;
        int offset = i - s_center_idx;
        if (offset < -2 || offset > 2) {
            lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 容器整体滚动：1 个动画代替 5 个 */
    int target_container_x = - (new_center - 1) * STEP_X;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_scroll);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_scroll), target_container_x);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* 居中卡变大 + 全透明度 */
    for (int i = 0; i < TOTAL_CARDS; i++) {
        if (!s_cards[i]) continue;
        int offset = i - s_center_idx;
        if (offset == 0) {
            lv_anim_t a2;
            lv_anim_init(&a2);
            lv_anim_set_var(&a2, s_cards[i]);
            lv_anim_set_exec_cb(&a2, (lv_anim_exec_xcb_t)set_opa);
            lv_anim_set_values(&a2, LV_OPA_60, LV_OPA_COVER);
            lv_anim_set_time(&a2, 150);
            lv_anim_start(&a2);
        }
    }

    if (s_cards[s_center_idx]) ui_anim_card_bounce(s_cards[s_center_idx]);
}
```

- [ ] **Step 4: 修改卡片初始位置**

在 `create_card` 调用后 / `ui_main_menu_init` 中，卡片放置到容器内时的 x 坐标为 `i * STEP_X`（或直接用 `update_card_appearance` 设置）：

```c
for (int i = 0; i < TOTAL_CARDS; i++) {
    // ... create_card ...
    lv_obj_set_x(s_cards[i], (i - 1) * STEP_X);  // 位置对齐容器原点
}
```

- [ ] **Step 5: 构建验证**

```bash
idf.py build
```
Expected: BUILD_EXIT:0

---

### Task 3: flush callback 异步化（SPI 渲染流水线）

**Files:**
- Modify: `components/ui_lvgl/ui_lvgl.c`（重写 flush callback + 信号量管理）

**Key design:**
- 保持当前 20 行缓冲 + 部分刷新（`full_refresh=0`）
- flush callback 改为：拷贝 → 非阻塞 SPI → 立即 `lv_disp_flush_ready`
- SPI 完成回调释放信号量，控制下一帧不覆盖 SPI 正在发送的缓冲区

- [ ] **Step 1: 添加信号量和回调函数**

在 `ui_lvgl.c` 顶部添加：

```c
#include "driver/spi_master.h"

/* SPI 传输完成回调：释放弹跳缓冲信号量 */
static void lcd_spi_post_cb(spi_transaction_t *trans)
{
    /* trans->user 指向信号量句柄 */
    SemaphoreHandle_t sem = (SemaphoreHandle_t)trans->user;
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(sem, &need_yield);
    if (need_yield) portYIELD_FROM_ISR(need_yield);
}
```

- [ ] **Step 2: 声明 SPI 设备和信号量**

```c
/* SPI 设备句柄和弹跳缓冲同步 */
static spi_device_handle_t s_spi_dev = NULL;
static SemaphoreHandle_t s_flush_sem = NULL;
static lv_color_t *s_bounce_buf = NULL;
```

- [ ] **Step 3: 重写 `lvgl_flush_cb`**

```c
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_handle_t panel = lcd_get_panel_handle();
    if (!panel) {
        lv_disp_flush_ready(drv);
        return;
    }

    int x1 = area->x1, y1 = area->y1;
    int x2 = area->x2 + 1, y2 = area->y2 + 1;
    size_t len = (x2 - x1) * (y2 - y1) * sizeof(lv_color_t);

    if (len == 0) {
        lv_disp_flush_ready(drv);
        return;
    }

    /* 等待上一帧 SPI 传输完成（信号量保护弹跳缓冲） */
    if (s_flush_sem) {
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    }

    /* 拷贝 PSRAM → SRAM 弹跳缓冲（DMA 需要 SRAM 地址） */
    memcpy(s_bounce_buf, color_p, len);

    /* 用 SPI 直发（跳过 esp_lcd_panel_draw_bitmap，直接用 SPI master API） */
    /* 先发 CASET/RASET/RAMWR 命令包，再发数据 */
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, s_bounce_buf);

    /* 立即释放 LVGL 缓冲，SPI 在后台发送 */
    lv_disp_flush_ready(drv);
}
```

**注意**: 当前 `esp_lcd_panel_draw_bitmap` 内部是阻塞的（`panel_io_spi_tx_color` 会等待事务完成）。为了让 SPI 异步，需要让 panel IO 不等待。最简单的实现方法：

在 `lcd_st7789_init()` 中保存 SPI device handle，然后在 flush callback 中直接发 SPI transaction。

- [ ] **Step 4: 暴露 SPI device handle**

在 `lcd_st7789.h` 添加：
```c
spi_device_handle_t lcd_get_spi_device(void);
```

在 `lcd_st7789.c` 添加：
```c
static spi_device_handle_t s_lcd_spi_dev = NULL;

esp_err_t lcd_st7789_init(void)
{
    // ... 原有代码 ...
    // 在 esp_lcd_new_panel_io_spi 之后保存 SPI device
    // panel_io_spi 内部有 spi_dev, 需要通过 panel IO 反查
    // 可选方案：用自定义 IO config 的 on_color_trans_done 回调获取 device
}

spi_device_handle_t lcd_get_spi_device(void)
{
    return s_lcd_spi_dev;
}
```

实际上，更简单的方法：在 flush callback 中直接用 `spi_device_transmit` 而不是 `esp_lcd_panel_draw_bitmap`。但这样需要手动管理 DC 线和 CS 线。

**替代方案（推荐）**：保持 `esp_lcd_panel_draw_bitmap` 阻塞调用不变，但利用 ESP-IDF SPI 驱动的队列机制——当 `trans_queue_depth > 1` 时，`spi_device_queue_trans` 会立即返回（不等传输完成），只有在队列满时才阻塞。

对 20 行缓冲（一个 transaction），队列深度 1 就够了——不阻塞。真正的瓶颈不在 SPI 本身，而在于 `lv_disp_flush_ready` 在 flush 完成后才调用，导致 LVGL 在下一帧渲染前等待。

**关键发现**：把 flush callback 改为**先调用 `lv_disp_flush_ready`，再发 SPI**——次序反转：

```c
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_handle_t panel = lcd_get_panel_handle();

    /* 1) 立即释放 LVGL 缓冲，让 LVGL 开始渲染下一块 */
    lv_disp_flush_ready(drv);

    /* 2) 再发 SPI（此时 SPI 传输与 LVGL 下一块渲染并行） */
    if (panel) {
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    }
}
```

这里有个问题：`color_p` 指向 LVGL 内部缓冲。`lv_disp_flush_ready` 之后，LVGL 可能立即往同一缓冲写入新数据。如果 SPI 还在从 `color_p` 地址读数据，就会产生撕裂。

**解决方案**：数据在释放前拷贝到弹跳缓冲：

```c
static lv_color_t *s_bounce = NULL;  // SRAM DMA-safe bounce buffer

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    size_t len = (area->x2 + 1 - area->x1) * (area->y2 + 1 - area->y1) * sizeof(lv_color_t);
    if (len > 0 && s_bounce) {
        memcpy(s_bounce, color_p, len);
    }

    /* 释放 LVGL 缓冲 */
    lv_disp_flush_ready(drv);

    /* 从弹跳缓冲发 SPI（不占 LVGL 缓冲） */
    if (len > 0 && s_bounce) {
        esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                                  area->x2 + 1, area->y2 + 1, s_bounce);
    }
}
```

弹跳缓冲在初始化时分配一次：
```c
s_bounce = heap_caps_malloc(240 * 20 * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
```

**这就是最终的异步 flush 方案**——无信号量、无回调、改动最小，利用 `memcpy` 解耦 LVGL 缓冲和 SPI 缓冲。

- [ ] **Step 5: 完整的 `ui_lvgl.c` 改动**

```patch
- #include "lcd_st7789.h"
- #include "esp_lcd_panel_ops.h"
+ #include "lcd_st7789.h"
+ #include "esp_lcd_panel_ops.h"
+ #include "string.h"
+ #include "esp_heap_caps.h"

  static const char *TAG = "LVGL";
  static lv_disp_t *s_disp = NULL;
+ static lv_color_t *s_bounce = NULL;  // DMA-safe 弹跳缓冲

  #define LVGL_BUF_LINES 20

  static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
  {
      esp_lcd_panel_handle_t panel = lcd_get_panel_handle();
-     if (panel) {
-         esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
-     }
-     lv_disp_flush_ready(drv);
+
+     size_t len = (area->x2 + 1 - area->x1) * (area->y2 + 1 - area->y1) * sizeof(lv_color_t);
+
+     /* 拷贝到弹跳缓冲（解耦 LVGL 缓冲，允许立即释放） */
+     if (len > 0 && s_bounce && color_p) {
+         memcpy(s_bounce, color_p, len);
+     }
+
+     /* 立即释放 LVGL 缓冲 → LVGL 开始渲染下一块 */
+     lv_disp_flush_ready(drv);
+
+     /* SPI 传输（用弹跳缓冲，与 LVGL 渲染并行） */
+     if (panel && len > 0 && s_bounce) {
+         esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
+                                   area->x2 + 1, area->y2 + 1, s_bounce);
+     }
  }
```

在 `ui_lvgl_init()` 中分配弹跳缓冲：
```patch
  s_buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  s_buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

+ s_bounce = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
+ if (!s_bounce) {
+     ESP_LOGE(TAG, "Failed to allocate bounce buffer");
+     return ESP_ERR_NO_MEM;
+ }
```

- [ ] **Step 6: `lv_task` 延时 20→5ms**

```patch
- vTaskDelay(pdMS_TO_TICKS(20));
+ vTaskDelay(pdMS_TO_TICKS(5));
```

- [ ] **Step 7: 构建验证**

```bash
idf.py build
```
Expected: BUILD_EXIT:0

---

### Task 4: 最终集成测试与调优

**Files:**
- Read-only: `build/` 输出

- [ ] **Step 1: 全量构建**

```bash
idf.py fullclean build
```
Expected: BUILD_EXIT:0

- [ ] **Step 2: 烧录后验证启动日志**

```bash
idf.py -p COM5 flash monitor
```

预期日志序列：
```
cpu_start: cpu freq: 240000000 Hz
MAIN: PSRAM: 8192 KB
MAIN: LCD OK  → ST7789 240x240
MAIN: LVGL OK → LVGL initialized: 240x240, 20-line double buffer
WiFi AP started: ESP32-CAM / 12345678
MAIN: All init OK!
JOY: ADC: X=19xx Y=19xx ...
```

- [ ] **Step 3: FPS 读数调优**

- 观察 LVGL 右上角 FPS 计数器
- 空闲状态：预期 < 5 FPS（无变化时几乎不刷）
- 滑动动画：预期 35+ FPS（40MHz SPI）或 60+ FPS（80MHz SPI）
- 如 SPI 80MHz 导致花屏：`lcd_st7789.c` 降回 `40 * 1000 * 1000` 并重建

- [ ] **Step 4: 抖动/撕裂检查**

- 快速左右滑动菜单，检查是否有：
  - 画面撕裂（上半帧和下半帧错位）
  - 闪烁
  - 卡片位置跳跃
- 如有撕裂，调整 `LV_DISP_DEF_REFR_PERIOD`（升到 15-20）
- 如有闪烁，检查 `memcpy` 和 `lv_disp_flush_ready` 顺序

---

## 执行顺序

```
Task 1: sdkconfig 调优 ───┐
Task 2: 父容器滚动 ───────┤ → Task 4: 集成测试
Task 3: 异步 flush ───────┘
```