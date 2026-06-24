#include "app_t.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_SETTINGS";
static lv_obj_t *s_page = NULL;
#define ITEM_COUNT 8

static int s_selected = 0;
static lv_obj_t *s_items[ITEM_COUNT] = {NULL};

static void set_item_style(int idx, int selected)
{
    if (!s_items[idx]) return;
    if (selected) {
        lv_obj_set_style_bg_color(s_items[idx], lv_color_make(100, 100, 200), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_items[idx], 2, LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_items[idx], lv_color_make(150, 150, 255), LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_bg_color(s_items[idx], lv_color_make(30, 30, 50), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_items[idx], 0, LV_STATE_DEFAULT);
    }
}

static const char *ITEM_NAMES[ITEM_COUNT] = {
    "Camera Config",
    "WiFi Mode",
    "Animation",
    "System Info",
    "Joystick Calib",
    "Display",
    "About - ColdFish",
    "Reset Defaults",
};

static void update_highlight(int old_idx, int new_idx)
{
    if (old_idx >= 0 && old_idx < ITEM_COUNT)
        set_item_style(old_idx, 0);
    if (new_idx >= 0 && new_idx < ITEM_COUNT)
        set_item_style(new_idx, 1);
}

static void on_create(lv_obj_t *parent)
{
    s_page = parent;
    lv_obj_set_style_bg_color(s_page, lv_color_make(30, 30, 50), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(s_page, true, LV_STATE_DEFAULT);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_page, LV_SCROLLBAR_MODE_OFF);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, lv_font_default(), LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* 创建8个设置项（用容器不用btn，btn内部布局触发递归崩溃） */
    for (int i = 0; i < ITEM_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(s_page);
        lv_obj_set_size(item, 210, 34);
        lv_obj_align(item, LV_ALIGN_TOP_MID, 0, 48 + i * 40);
        lv_obj_set_style_radius(item, 6, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(item, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(item, lv_color_make(40, 40, 70), LV_STATE_DEFAULT);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, ITEM_NAMES[i]);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(label, lv_font_default(), LV_STATE_DEFAULT);
        lv_obj_center(label);

        s_items[i] = item;
    }

    s_selected = 0;
    set_item_style(0, 1);  // 初始化高亮第一项

    ESP_LOGI(TAG, "Settings app created");
}

static void on_destroy(void)
{
    for (int i = 0; i < ITEM_COUNT; i++) s_items[i] = NULL;
    s_selected = 0;
    s_page = NULL;
}

static void on_joystick(joystick_evt_t evt)
{
    int prev = s_selected;
    switch (evt) {
        case JOY_EVT_LEFT:  // 物理推下 → 下一项
            s_selected++;
            if (s_selected >= ITEM_COUNT) s_selected = 0;
            update_highlight(prev, s_selected);
            int scroll_y = (s_selected > 2) ? (s_selected - 2) * 40 : 0;
            lv_obj_scroll_to_y(s_page, scroll_y, LV_ANIM_ON);
            break;
        case JOY_EVT_RIGHT:  // 物理推上 → 上一项
            s_selected--;
            if (s_selected < 0) s_selected = ITEM_COUNT - 1;
            update_highlight(prev, s_selected);
            int scroll_y2 = (s_selected > 2) ? (s_selected - 2) * 40 : 0;
            lv_obj_scroll_to_y(s_page, scroll_y2, LV_ANIM_ON);
            break;
        case JOY_EVT_PRESS:
            /* 闪烁反馈 */
            if (s_items[s_selected]) {
                lv_obj_set_style_bg_color(s_items[s_selected], lv_color_make(100, 100, 180), LV_STATE_DEFAULT);
            }
            set_item_style(s_selected, 1);
            ESP_LOGI(TAG, "Selected: %s", ITEM_NAMES[s_selected]);
            break;
        default:
            break;
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