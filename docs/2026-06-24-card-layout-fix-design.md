# 主菜单卡片布局修整设计

## 问题

1. **卡片重叠**：`STEP_X=120` 导致中心卡右沿(190)侵入右卡左沿(170)，间距 -20px
2. **贴顶**：卡片 y=0，无顶部留白
3. **缺少启动提示**：底部无 "Launch" 引导标签

## 改动

### 布局常量（`ui_main.c`）

```c
#define CARD_BIG_W   140
#define CARD_BIG_H   150    // 160→150，降低高度
#define CARD_SMALL_W 100    // 110→100，更窄
#define CARD_SMALL_H 120    // 130→120
#define CENTER_X     50     // 中心卡左边缘
#define STEP_X       160    // 120→160，留出20px间隙
#define CARD_TOP     30     // 新增：卡片距顶边
```

### 卡片位置计算

所有涉及卡片 `y` 或 `x` 的地方加上偏移：

```c
// 中心卡
lv_obj_set_x(card, CENTER_X);
lv_obj_set_y(card, CARD_TOP);

// 侧边卡
lv_obj_set_x(card, CENTER_X + offset * STEP_X);
lv_obj_set_y(card, CARD_TOP + (offset != 0 ? 15 : 0)); // 侧边卡Y偏移略大
```

### 底部标签

```c
lv_obj_t *launch = lv_label_create(lv_scr_act());
lv_label_set_text(launch, "Launch");
lv_obj_align(launch, LV_ALIGN_BOTTOM_MID, 0, -15);
lv_obj_set_style_text_color(launch, lv_color_make(180, 180, 180), LV_STATE_DEFAULT);
```

### 涉及函数

- `create_card()`：初始位置设 `y=CARD_TOP`
- `update_card_appearance()`：调整 Y 位置
- `animate_to_center()`：动画不涉及 Y，但最终位置需设 Y
- `ui_main_menu_init()`：底部标签创建