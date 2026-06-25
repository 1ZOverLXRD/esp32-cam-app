#include "app_t.h"
#include "ui_main.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "APP_SETTINGS";

/* 外部标志：通知ui_main.c不要触发退出 */
extern volatile bool s_app_handled;
static lv_obj_t *s_page = NULL;
static int s_first_event = 1;
static int s_skip_press = 0;

/* 塌缩弧进度指示 */
static lv_obj_t *s_col_overlay = NULL;
static lv_obj_t *s_col_arc = NULL;
static bool s_col_animating = false;

#define SECTION_COUNT 3
#define WIFI_SEC_IDX 1  // WiFi Info 段索引（Camera=0, WiFi=1, Joystick=2）

typedef struct {
    lv_obj_t *header;
    lv_obj_t *hdr_label;
    lv_obj_t *indicator;
    lv_obj_t *cursor;
    lv_obj_t *sub_items[6];
    int sub_count;
    int expanded;
} section_t;

static section_t s_sections[SECTION_COUNT];
static int s_selected = 0;
static int s_sub_selected = 0;

/* 表头名称 */
static const char *HEADER_NAMES[SECTION_COUNT] = {
    "Camera Config",
    "WiFi Info",
    "Joystick Report",
};

/* 各段子项 */
static const char *CAMERA_ITEMS[] = {"Resolution: 720p", "Frame Rate: 25fps", "Flip: Off"};
/* WiFi Info 段：断开 vs 连接 */
static const char *WIFI_ITEMS_DISCONNECTED[] = {"Status:  AP Mode / 192.168.4.1",
                                                 "SSID:    COLDFISHESP32",
                                                 "Password: 12060606"};
static const char *JOY_ITEMS[] = {"X:0.00  Y:0.00  SW:1"};

static const char **SUB_ITEMS[SECTION_COUNT] = {
    CAMERA_ITEMS, WIFI_ITEMS_DISCONNECTED, JOY_ITEMS,
};

static const int SUB_COUNTS[SECTION_COUNT] = {
    3, 3, 1,
};

#define HEADER_H 36
#define SUB_H    32
#define ITEM_GAP 2
#define BASE_Y   48

/* 更新折叠状态+动画 */
static void update_expand(section_t *sec, int expand)
{
    sec->expanded = expand;
    /* 更新指示器 */
    if (sec->indicator)
        lv_label_set_text(sec->indicator, expand ? "[-]" : "[+]");

    /* 子项显隐（直接显示/隐藏，不做透明度动画避免不可见） */
    for (int j = 0; j < sec->sub_count; j++) {
        if (!sec->sub_items[j]) continue;
        if (expand) {
            lv_obj_clear_flag(sec->sub_items[j], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sec->sub_items[j], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 重排所有段位置（展开项后面多留子项空间），跳过Y没变项减少SPI刷新 */
    int y = BASE_Y;
    for (int i = 0; i < SECTION_COUNT; i++) {
        section_t *s = &s_sections[i];
        if (!s->header) continue;
        if ((int)lv_obj_get_y(s->header) != y)
            lv_obj_set_y(s->header, y);
        y += HEADER_H + ITEM_GAP;

        if (s->expanded) {
            for (int j = 0; j < s->sub_count; j++) {
                if (!s->sub_items[j]) continue;
                if ((int)lv_obj_get_y(s->sub_items[j]) != y)
                    lv_obj_set_y(s->sub_items[j], y);
                y += SUB_H + ITEM_GAP;
            }
        }
    }
}

/* 选中样式 — '>' 游标标记选中 */
static void set_header_style(lv_obj_t *obj, int selected)
{
    section_t *sec = NULL;
    for (int i = 0; i < SECTION_COUNT; i++)
        if (s_sections[i].header == obj) { sec = &s_sections[i]; break; }
    if (!sec) return;

    /* 延迟创建 '>' 游标 */
    if (selected && !sec->cursor) {
        sec->cursor = lv_label_create(sec->header);
        lv_label_set_text(sec->cursor, ">");
        lv_obj_set_style_text_color(sec->cursor, lv_color_make(140, 140, 255), LV_STATE_DEFAULT);
        lv_obj_align(sec->cursor, LV_ALIGN_LEFT_MID, 2, 0);
    }
    if (sec->cursor)
        lv_obj_set_style_opa(sec->cursor, selected ? LV_OPA_COVER : LV_OPA_0, LV_STATE_DEFAULT);
}

static void set_sub_style(lv_obj_t *obj, int selected)
{
    if (selected) {
        lv_obj_set_style_bg_color(obj, lv_color_make(60, 60, 110), LV_STATE_DEFAULT);
        /* 查找子项中的 '>' 游标，没有则创建 */
        lv_obj_t *cursor = lv_obj_get_child(obj, 1); // 第二个子控件（第一个是label）
        if (!cursor) {
            cursor = lv_label_create(obj);
            lv_label_set_text(cursor, ">");
            lv_obj_set_style_text_color(cursor, lv_color_make(140, 140, 255), LV_STATE_DEFAULT);
            lv_obj_align(cursor, LV_ALIGN_LEFT_MID, 2, 0);
        }
        lv_obj_clear_flag(cursor, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_bg_color(obj, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
        lv_obj_t *cursor = lv_obj_get_child(obj, 1); // 第二个子控件
        if (cursor) lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 创建单个子项 */
static lv_obj_t *create_sub_item(lv_obj_t *parent, const char *text, int y_pos)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_size(item, 210, SUB_H);
    lv_obj_set_style_radius(item, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(item, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(item, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_y(item, y_pos);
    lv_obj_set_x(item, 8);  // 统一左偏移 8px

    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, lv_font_default(), LV_STATE_DEFAULT);
    lv_obj_set_width(label, 200);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_set_style_opa(item, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);                 // 初始隐藏
    return item;
}

/* 创建表头 */
static lv_obj_t *create_header(lv_obj_t *parent, const char *text, int idx)
{
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, 230, HEADER_H);
    lv_obj_set_style_radius(hdr, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(hdr, lv_color_make(35, 35, 60), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(hdr, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ind = lv_label_create(hdr);
    lv_label_set_text(ind, "[+]");
    lv_obj_set_style_text_color(ind, lv_color_make(150, 150, 200), LV_STATE_DEFAULT);
    lv_obj_align(ind, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *lbl = lv_label_create(hdr);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 44, 0);

    s_sections[idx].header = hdr;
    s_sections[idx].hdr_label = lbl;
    s_sections[idx].indicator = ind;
    s_sections[idx].cursor = NULL;
    s_sections[idx].sub_count = SUB_COUNTS[idx];
    s_sections[idx].expanded = 0;

    return hdr;
}

static void navigate(int dir)
{
    int prev = s_selected;
    s_selected += dir;
    if (s_selected < 0) s_selected = SECTION_COUNT - 1;
    if (s_selected >= SECTION_COUNT) s_selected = 0;

    set_header_style(s_sections[prev].header, 0);
    set_header_style(s_sections[s_selected].header, 1);
    /* 滚动到选中表头可见 */
    lv_obj_scroll_to_y(s_page, s_selected * (HEADER_H + ITEM_GAP) - 20, LV_ANIM_ON);
}

/* ===== 塌缩弧进度指示 ===== */
static void set_opa(lv_obj_t *obj, int16_t v)
{
    lv_obj_set_style_opa(obj, v, LV_STATE_DEFAULT);
}

static void collapse_arc_ready(lv_anim_t *a)
{
    (void)a;
    s_col_animating = false;
    /* 弧完成时，如果on_destroy已经跑过了（s_page=NULL），什么都不做 */
    if (!s_page) return;
    ui_pop_state();  // STATE_SUB → STATE_ROOT
    /* 执行塌缩 */
    section_t *sec = &s_sections[s_selected];
    update_expand(sec, 0);
    s_sub_selected = 0;
    s_skip_press = 1;
    /* '>' 从子项移回表头 */
    for (int j = 0; j < sec->sub_count; j++)
        set_sub_style(sec->sub_items[j], 0);
    set_header_style(sec->header, 1);
    if (s_col_overlay) {
        lv_obj_del(s_col_overlay);
        s_col_overlay = NULL;
        s_col_arc = NULL;
    }
}

/* 更新 WiFi Info 段显示（连接状态变化时调用） */
void update_wifi_info(void)
{
    section_t *sec = &s_sections[WIFI_SEC_IDX];  // WiFi Info
    if (!sec->header) return;

    if (s_wifi_connected) {
        /* 删除 items[1+]（彻底消除任何占位可能） */
        for (int j = 1; j < sec->sub_count; j++) {
            if (sec->sub_items[j]) {
                lv_obj_del(sec->sub_items[j]);
                sec->sub_items[j] = NULL;
            }
        }
        /* 更新第0项文字和Y位置 */
        if (sec->sub_items[0]) {
            lv_obj_t *lbl = lv_obj_get_child(sec->sub_items[0], 0);
            if (lbl) lv_label_set_text(lbl, "Status:  Connected");
            int hy = lv_obj_get_y(sec->header);
            lv_obj_set_y(sec->sub_items[0], hy + HEADER_H + ITEM_GAP);
            lv_obj_clear_flag(sec->sub_items[0], LV_OBJ_FLAG_HIDDEN);
        }
        sec->sub_count = 1;
        /* 重排所有段 Y 位置（WiFi 项数变了，后续段需上移） */
        for (int i = 0; i < SECTION_COUNT; i++)
            if (s_sections[i].header)
                update_expand(&s_sections[i], s_sections[i].expanded);
    } else {
        /* 断开时重建被删的子项 */
        sec->sub_count = 3;
        int hy = lv_obj_get_y(sec->header);
        for (int j = 0; j < 3; j++) {
            if (!sec->sub_items[j]) {
                sec->sub_items[j] = create_sub_item(s_page,
                    WIFI_ITEMS_DISCONNECTED[j],
                    hy + HEADER_H + ITEM_GAP + j * (SUB_H + ITEM_GAP));
            }
        }
        for (int j = 0; j < 3; j++)
            if (sec->sub_items[j])
                lv_obj_clear_flag(sec->sub_items[j], LV_OBJ_FLAG_HIDDEN);
    }
}

static void start_collapse_arc(void)
{
    if (s_col_animating) return;
    s_col_animating = true;

    section_t *sec = &s_sections[s_selected];
    int hy = lv_obj_get_y(sec->header);
    int hw = lv_obj_get_width(sec->header);

    s_col_overlay = lv_obj_create(s_page);
    lv_obj_set_size(s_col_overlay, hw, HEADER_H);
    lv_obj_set_pos(s_col_overlay, 5, hy);
    lv_obj_set_style_bg_color(s_col_overlay, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_col_overlay, LV_OPA_40, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_col_overlay, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_col_overlay, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_col_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_opa(s_col_overlay, LV_OPA_0, LV_STATE_DEFAULT);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_col_overlay);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)set_opa);
    lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&a, 80);
    lv_anim_start(&a);

    s_col_arc = lv_arc_create(s_col_overlay);
    lv_obj_set_size(s_col_arc, 28, 28);
    lv_obj_align(s_col_arc, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_arc_set_bg_angles(s_col_arc, 0, 360);
    lv_arc_set_rotation(s_col_arc, 270);
    lv_arc_set_range(s_col_arc, 0, 100);
    lv_arc_set_value(s_col_arc, 0);
    lv_obj_set_style_arc_color(s_col_arc, lv_color_make(140, 140, 255), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_col_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_col_arc, 4, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_col_arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t arc_anim;
    lv_anim_init(&arc_anim);
    lv_anim_set_var(&arc_anim, s_col_arc);
    lv_anim_set_exec_cb(&arc_anim, (lv_anim_exec_xcb_t)lv_arc_set_value);
    lv_anim_set_values(&arc_anim, 0, 100);
    lv_anim_set_time(&arc_anim, 400);
    lv_anim_set_ready_cb(&arc_anim, collapse_arc_ready);
    lv_anim_start(&arc_anim);
}

static void cancel_collapse_arc(int for_destroy)
{
    if (!s_col_animating) return;
    s_col_animating = false;
    if (s_col_overlay) {
        lv_anim_del(s_col_overlay, NULL);
        if (!for_destroy) lv_obj_add_flag(s_col_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_col_arc) lv_anim_del(s_col_arc, NULL);
    s_col_overlay = NULL;
    s_col_arc = NULL;
}

static void cleanup_arc_for_destroy(void)
{
    cancel_collapse_arc(1);
}

static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    s_first_event = 1;
    s_skip_press = 0;
    s_col_animating = false;
    s_col_overlay = NULL;
    s_col_arc = NULL;
    /* 全局栈: depth已=APP, 再push → SUB */
    ui_push_sub();
    lv_obj_set_style_bg_color(s_page, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(s_page, true, LV_STATE_DEFAULT);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_page, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* 创建折叠段 */
    memset(s_sections, 0, sizeof(s_sections));
    int y = BASE_Y;
    for (int i = 0; i < SECTION_COUNT; i++) {
        lv_obj_t *hdr = create_header(s_page, HEADER_NAMES[i], i);
        lv_obj_set_y(hdr, y);
        y += HEADER_H + ITEM_GAP;

        /* 创建子项 */
        section_t *sec = &s_sections[i];
        const char **items = SUB_ITEMS[i];
        int count = SUB_COUNTS[i];

        /* WiFi Info 段：断开状态固定3项，连接后由 update_wifi_info 处理 */
        if (i == WIFI_SEC_IDX) {
            items = WIFI_ITEMS_DISCONNECTED;
            count = 3;
        }

        sec->sub_count = count;
        for (int j = 0; j < count; j++) {
            sec->sub_items[j] = create_sub_item(s_page, items[j], y);
            y += SUB_H + ITEM_GAP;
        }
    }

    s_selected = 0;
    /* 强制所有段收起，清除任何残留展开状态 */
    for (int i = 0; i < SECTION_COUNT; i++)
        update_expand(&s_sections[i], 0);
    set_header_style(s_sections[0].header, 1);
}

static void on_destroy(void)
{
    cleanup_arc_for_destroy();
    memset(s_sections, 0, sizeof(s_sections));
    s_selected = 0;
    s_page = NULL;
}

static void on_joystick(joystick_evt_t evt)
{
    if (s_first_event) { s_first_event = 0; return; }

    int depth = ui_get_depth();  // 1=APP级, 2+=SUB级
    int top = ui_peek_state();   // UI_STATE_APP / UI_STATE_SUB

    /* 塌缩弧动画中: 任何操作取消弧 */
    if (s_col_animating) {
        if (evt == JOY_EVT_LEFT || evt == JOY_EVT_RIGHT || evt == JOY_EVT_PRESS) {
            cancel_collapse_arc(0);
            return;
        }
    }

    /* Joystick Report 段展开时：每次事件更新实时值 */
    if (s_selected == 2 && s_sections[2].expanded) {
        char buf[40];
        float x = joystick_read_y();
        float y = joystick_read_x();
        int sw = joystick_read_sw();
        snprintf(buf, sizeof(buf), "X:%.2f  Y:%.2f  SW:%d", x, y, sw ? 0 : 1);
        if (s_sections[2].sub_items[0]) {
            lv_obj_t *lbl = lv_obj_get_child(s_sections[2].sub_items[0], 0);
            if (lbl) lv_label_set_text(lbl, buf);
        }
    }

    switch (evt) {
    case JOY_EVT_LEFT:
    case JOY_EVT_RIGHT: {
        if (top == UI_STATE_SUB) {
            section_t *sec = &s_sections[s_selected];
            lv_obj_t *old = sec->sub_items[s_sub_selected];
            if (old) set_sub_style(old, 0);
            s_sub_selected += (evt == JOY_EVT_LEFT) ? 1 : -1;
            if (s_sub_selected >= sec->sub_count) s_sub_selected = 0;
            if (s_sub_selected < 0) s_sub_selected = sec->sub_count - 1;
            lv_obj_t *new = sec->sub_items[s_sub_selected];
            if (new) set_sub_style(new, 1);
            int sy = BASE_Y + s_selected * (HEADER_H + ITEM_GAP)
                     + s_sub_selected * (SUB_H + ITEM_GAP) - 40;
            if (s_page) lv_obj_scroll_to_y(s_page, sy > 0 ? sy : 0, LV_ANIM_ON);
        } else {
            navigate(evt == JOY_EVT_LEFT ? 1 : -1);
        }
        break;
    }

    case JOY_EVT_PRESS:
        if (s_skip_press) {
            s_skip_press = 0;
            break;
        }
        if (top == UI_STATE_SUB) {
            /* 子项级: PRESS不做任何事（只能LONG_PRESS返回） */
            break;
        }
        /* APP级: PRESS展开段并压入子项状态 */
        {
            section_t *sec = &s_sections[s_selected];
            update_expand(sec, 1);
            for (int i = 0; i < SECTION_COUNT; i++)
                if (i != s_selected && s_sections[i].expanded)
                    { s_sections[i].expanded = 0; update_expand(&s_sections[i], 0); }
            /* '>' 从表头移到子项 */
            set_header_style(sec->header, 0);
            s_sub_selected = 0;
            set_sub_style(sec->sub_items[0], 1);
            ui_push_sub();
        }
        ESP_LOGI(TAG, "Selected: %s", HEADER_NAMES[s_selected]);
        break;

    case JOY_EVT_LONG_PRESS:
        if (top == UI_STATE_SUB) {
            /* 子项级: 启动塌缩弧，消费事件阻止退出APP */
            s_app_handled = true;
            start_collapse_arc();
        }
        /* APP级(depth=1): 不消费事件 → ui_main触发退出APP */
        break;

    default: break;
    }
}

app_t app_settings = {
    .name = "Settings",
    .icon = "S",
    .color = 0x00B894,
    .on_create = on_create,
    .on_destroy = on_destroy,
    .on_joystick = on_joystick,
    .page = NULL,
};