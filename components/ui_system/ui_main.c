#include "ui_main.h"
#include "ui_anim.h"
#include "app_t.h"
#include "esp_log.h"
#include <string.h>

/* APP 实例（由各 app_xxx.c 定义） */
extern app_t app_settings;

static const char *TAG = "UI_MAIN";

static void set_opa(lv_obj_t *obj, int16_t v)
{
    lv_obj_set_style_opa(obj, v, LV_STATE_DEFAULT);
}

#define APP_COUNT 4
static const char *APP_NAMES[APP_COUNT] = {"Settings", "Trash", "Weather", "Camera"};
static const uint32_t APP_COLORS[APP_COUNT] = {
    0x00B894, 0xE17055, 0x0984E3, 0x6C5CE7,
};

#define TOTAL_CARDS APP_COUNT
static lv_obj_t *s_cards[TOTAL_CARDS] = {NULL};
static int s_center_idx = 0;
static bool s_is_open = false;
static TickType_t s_open_time = 0;
static lv_obj_t *s_app_page = NULL;
static app_t *s_active_app = NULL;
static bool s_exit_locked = false;

/* WiFi 门禁 */
bool s_wifi_connected = false;
char s_sta_ip[16] = "0.0.0.0";
bool s_wifi_switching = false;  // 切模式中，忽略虚假断开

/* WiFi UI 待更新标志 + LVGL 定时器 */
volatile bool s_wifi_ui_pending = false;
static lv_timer_t *s_wifi_timer = NULL;

static void wifi_poll_cb(lv_timer_t *t)
{
    (void)t;
    if (s_wifi_ui_pending) {
        s_wifi_ui_pending = false;
        update_wifi_info();
    }
}

/* ===== 全局状态栈 =====
 * 每层存储实际状态值: MAINMENU=0, APP=1, SUB=2 ...
 * push(值)存入，pop弹出顶部，peek查看当前层
 */
#define UI_STACK_MAX 8
static int s_state_stack[UI_STACK_MAX];
static int s_state_depth = 0;
static void ui_push_explicit(int val);  // 前向声明

int ui_get_depth(void) { return s_state_depth; }
void ui_push_state(void) { ui_push_explicit(UI_STATE_APP); }
void ui_push_sub(void)   { ui_push_explicit(UI_STATE_SUB); }
void ui_pop_state(void)
{
    if (s_state_depth > 1) s_state_depth--;  // 保留MAINMENU
}
int ui_peek_state(void) {
    return s_state_depth > 0 ? s_state_stack[s_state_depth - 1] : -1;
}
static void ui_push_explicit(int val) {
    if (s_state_depth < UI_STACK_MAX)
        s_state_stack[s_state_depth++] = val;
}

/* 长按退出叠层 */
static lv_obj_t *s_exit_overlay = NULL;
static lv_obj_t *s_exit_arc = NULL;
static bool s_exit_animating = false;
volatile bool s_exit_cleanup_pending = false;
volatile bool s_app_handled = false;  // APP是否消费了事件

/* 前向声明 */
static void close_current_app(void);
static void cancel_exit(void);

/* 布局 */
#define CARD_BIG_W   140
#define CARD_BIG_H   150
#define CARD_SMALL_W 100
#define CARD_SMALL_H 120
#define CENTER_X     50
#define STEP_X       160
#define CARD_TOP     30

/* 动画完成回调：只设标志不做删除 */
static void exit_arc_ready_cb(lv_anim_t *a)
{
    (void)a;
    s_exit_animating = false;
    s_exit_cleanup_pending = true;
}

/* 取消长按退出 - 淡出回调（只设标志不做删除） */
static void cancel_exit_fade_done(lv_anim_t *a)
{
    (void)a;
    s_exit_animating = false;
}

/* 取消长按退出 */
static void cancel_exit(void)
{
    if (!s_exit_animating) return;
    s_exit_animating = false;
    if (s_exit_overlay) {
        lv_obj_del(s_exit_overlay);
        s_exit_overlay = NULL;
        s_exit_arc = NULL;
    }
}

/* 启动长按退出动画 */
static void start_exit_animation(void)
{
    if (s_exit_animating) return;
    s_exit_animating = true;

    s_exit_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_exit_overlay, 240, 240);
    lv_obj_set_style_bg_color(s_exit_overlay, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_exit_overlay, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_exit_overlay, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_exit_overlay, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_exit_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_exit_arc = lv_arc_create(s_exit_overlay);
    lv_obj_set_size(s_exit_arc, 80, 80);
    lv_obj_center(s_exit_arc);
    lv_arc_set_bg_angles(s_exit_arc, 0, 360);
    lv_arc_set_rotation(s_exit_arc, 270);
    lv_arc_set_range(s_exit_arc, 0, 100);
    lv_arc_set_value(s_exit_arc, 0);
    lv_obj_set_style_arc_color(s_exit_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_exit_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_exit_arc, 6, LV_PART_INDICATOR);

    lv_obj_set_style_opa(s_exit_overlay, LV_OPA_0, LV_STATE_DEFAULT);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_exit_overlay);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)set_opa);
    lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&a, 150);
    lv_anim_start(&a);

    lv_anim_t arc_anim;
    lv_anim_init(&arc_anim);
    lv_anim_set_var(&arc_anim, s_exit_arc);
    lv_anim_set_exec_cb(&arc_anim, (lv_anim_exec_xcb_t)lv_arc_set_value);
    lv_anim_set_values(&arc_anim, 0, 100);
    lv_anim_set_time(&arc_anim, 400);  // 弧400ms填满（长按800ms+弧400ms≈1.2s总时长）
    lv_anim_set_ready_cb(&arc_anim, exit_arc_ready_cb);
    lv_anim_start(&arc_anim);
}

static lv_obj_t *create_card(int real_idx)
{
    lv_obj_t *card = lv_obj_create(lv_scr_act());
    lv_obj_set_size(card, CARD_BIG_W, CARD_BIG_H);
    lv_obj_set_style_radius(card, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_COLORS[real_idx]), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(card, 0, LV_STATE_DEFAULT);
    lv_obj_set_y(card, CARD_TOP);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, APP_NAMES[real_idx]);
    lv_obj_set_style_text_color(name, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(name, lv_font_default(), LV_STATE_DEFAULT);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 0);

    return card;
}

static void update_card_appearance(void)
{
    for (int i = 0; i < TOTAL_CARDS; i++) {
        if (!s_cards[i]) continue;
        int offset = i - s_center_idx;
        if (offset < -1 || offset > 1) {
            lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
        int x = CENTER_X + offset * STEP_X;
        lv_obj_set_x(s_cards[i], x);
        lv_obj_set_y(s_cards[i], CARD_TOP);
        if (offset == 0) {
            lv_obj_set_size(s_cards[i], CARD_BIG_W, CARD_BIG_H);
        } else {
            lv_obj_set_size(s_cards[i], CARD_SMALL_W, CARD_SMALL_H);
        }
    }
}

static void animate_to_center(int new_center)
{
    s_center_idx = new_center;
    for (int i = 0; i < TOTAL_CARDS; i++) {
        if (!s_cards[i]) continue;
        int offset = i - s_center_idx;
        if (offset < -1 || offset > 1) {
            lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
        /* 位置动画 */
        int tx = CENTER_X + offset * STEP_X;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_cards[i]);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
        lv_anim_set_values(&a, lv_obj_get_x(s_cards[i]), tx);
        lv_anim_set_time(&a, 100);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        /* Y 位置直接设 */
        lv_obj_set_y(s_cards[i], CARD_TOP);
        /* 居中卡大，侧边卡小 */
        if (offset == 0) {
            lv_obj_set_size(s_cards[i], CARD_BIG_W, CARD_BIG_H);
        } else {
            lv_obj_set_size(s_cards[i], CARD_SMALL_W, CARD_SMALL_H);
        }
    }
    if (s_cards[s_center_idx]) ui_anim_card_bounce(s_cards[s_center_idx]);
}

static void navigate(int dir)
{
    if (s_is_open) return;
    s_exit_locked = false;  // 导航时解锁，允许下次PRESS进入APP
    int new_idx = s_center_idx + dir;

    if (new_idx > APP_COUNT - 1) {
        s_center_idx = 0;
        update_card_appearance();
        if (s_cards[s_center_idx]) ui_anim_card_bounce(s_cards[s_center_idx]);
        return;
    }
    if (new_idx < 0) {
        s_center_idx = APP_COUNT - 1;
        update_card_appearance();
        if (s_cards[s_center_idx]) ui_anim_card_bounce(s_cards[s_center_idx]);
        return;
    }
    animate_to_center(new_idx);
}

void ui_main_menu_init(void)
{
    ESP_LOGI(TAG, "Creating main menu carousel...");

    s_state_depth = 0;
    s_state_stack[0] = UI_STATE_MAINMENU;
    s_state_depth = 1;

    for (int i = 0; i < TOTAL_CARDS; i++) {
        s_cards[i] = create_card(i);
        lv_obj_set_x(s_cards[i], CENTER_X + (i - s_center_idx) * STEP_X);
    }
    ESP_LOGI(TAG, "cards created");

    update_card_appearance();
    ESP_LOGI(TAG, "appearance updated");

    /* 底部启动标签 */
    lv_obj_t *launch = lv_label_create(lv_scr_act());
    lv_label_set_text(launch, "Launch");
    lv_obj_align(launch, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_text_color(launch, lv_color_make(180, 180, 180), LV_STATE_DEFAULT);

    ESP_LOGI(TAG, "Main menu created");

    /* WiFi UI 轮询定时器（LVGL 任务上下文，安全） */
    s_wifi_timer = lv_timer_create(wifi_poll_cb, 100, NULL);
    lv_timer_set_repeat_count(s_wifi_timer, -1);
}

static int get_real_index(void)
{
    if (s_center_idx < 0) return 0;
    if (s_center_idx > APP_COUNT - 1) return APP_COUNT - 1;
    return s_center_idx;
}

static void close_current_app(void)
{
    if (!s_is_open || !s_app_page) return;
    s_is_open = false;
    s_exit_locked = true;

    /* 通知当前APP即将销毁 */
    if (s_active_app && s_active_app->on_destroy)
        s_active_app->on_destroy();
    s_active_app = NULL;

    /* 先取消隐藏卡片 */
    for (int i = 0; i < TOTAL_CARDS; i++)
        if (s_cards[i]) lv_obj_clear_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
    update_card_appearance();

    /* 直接删除APP页（淡出靠卡片自然显现） */
    lv_obj_del(s_app_page);
    s_app_page = NULL;

    if (s_exit_overlay) {
        lv_obj_del(s_exit_overlay);
        s_exit_overlay = NULL;
        s_exit_arc = NULL;
    }
    s_exit_animating = false;
    ui_pop_state();  // APP → MAINMENU
}

static void open_current_app(void)
{
    if (s_is_open) return;
    s_is_open = true;
    s_open_time = xTaskGetTickCount();
    int real_idx = get_real_index();

    s_app_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_app_page, 240, 240);
    lv_obj_set_style_radius(s_app_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_app_page, lv_color_hex(APP_COLORS[real_idx]), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_app_page, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_opa(s_app_page, LV_OPA_0, LV_STATE_DEFAULT);

    /* 查找并激活对应的APP实例 */
    s_active_app = NULL;
    if (real_idx == 0) s_active_app = &app_settings;

    if (real_idx != 0 && !s_wifi_connected) {
        /* 非 Settings APP + WiFi 未连接 → 显示阻断提示 */
        lv_obj_t *ov = lv_obj_create(s_app_page);
        lv_obj_set_size(ov, 240, 240);
        lv_obj_set_style_bg_color(ov, lv_color_make(10, 10, 30), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ov, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(ov, 0, LV_STATE_DEFAULT);
        lv_obj_clear_flag(ov, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *warn = lv_label_create(ov);
        lv_label_set_text(warn, LV_SYMBOL_WARNING "\nNo WiFi\n\nOpen browser to\nhttp://192.168.4.1\n\nConfigure WiFi\nto use this app");
        lv_obj_set_style_text_color(warn, lv_color_make(200, 200, 200), LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
        lv_obj_center(warn);
    } else if (s_active_app && s_active_app->on_create) {
        /* 由APP自行填充页面内容 */
        s_active_app->on_create(s_app_page);
    } else {
        /* 没有APP实例时，显示简单的名字标签 */
        lv_obj_t *label = lv_label_create(s_app_page);
        lv_label_set_text_fmt(label, "%s", APP_NAMES[real_idx]);
        lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_center(label);

        lv_obj_t *hint = lv_label_create(s_app_page);
        lv_label_set_text(hint, "Press to exit");
        lv_obj_set_style_text_color(hint, lv_color_make(200, 200, 200), LV_STATE_DEFAULT);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    /* 隐藏卡片后用full_refresh淡入 */
    for (int i = 0; i < TOTAL_CARDS; i++)
        if (s_cards[i]) lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);

    ui_anim_app_open(s_app_page, NULL);
    ui_push_state();  // MAINMENU → APP
}

void ui_main_handle_joystick(joystick_evt_t evt)
{
    /* 检测待处理的退出清理 */
    if (s_exit_cleanup_pending) {
        close_current_app();
        s_exit_cleanup_pending = false;
        return;
    }

    /* 在APP内时，将事件分发给当前APP（包括LONG_PRESS，让APP优先处理） */
    if (s_is_open) {
        s_app_handled = false;
        if (s_active_app && s_active_app->on_joystick)
            s_active_app->on_joystick(evt);

        /* APP消费了事件（如子项收起），不触发退出 */
        if (evt == JOY_EVT_LONG_PRESS && !s_exit_animating
            && !s_app_handled && ui_peek_state() == UI_STATE_APP) {
            start_exit_animation();
        }
        /* PRESS 在退出动画中取消，或depth>1时取消退出弧 */
        if (evt == JOY_EVT_PRESS && s_exit_animating) {
            cancel_exit();
        }
        return;
    }

    switch (evt) {
        case JOY_EVT_UP:
        case JOY_EVT_DOWN:
            if (s_exit_animating) { cancel_exit(); return; }
            navigate(evt == JOY_EVT_UP ? -1 : 1);
            break;
        case JOY_EVT_LEFT:    // 保留给未来功能
        case JOY_EVT_RIGHT:   // 保留给未来功能
        case JOY_EVT_PRESS:
            if (!s_is_open && !s_exit_locked) {
                s_exit_locked = false;
                open_current_app();
            } else {
                s_exit_locked = false;
            }
            break;
        case JOY_EVT_LONG_PRESS:
            if (s_is_open && !s_exit_animating) {
                start_exit_animation();
            }
            break;
        default:
            break;
    }
}