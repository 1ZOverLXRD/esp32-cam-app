#include "ui_anim.h"

/* LVGL v8 包装：3参数→2参数动画回调 */
static void set_opa(lv_obj_t *obj, int16_t v)
{
    lv_obj_set_style_opa(obj, v, LV_STATE_DEFAULT);
}

void ui_anim_app_open(lv_obj_t *target, lv_anim_ready_cb_t cb)
{
    if (!target) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)set_opa);
    lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&a, 400);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, cb);
    lv_anim_start(&a);
}

void ui_anim_app_close(lv_obj_t *target, lv_anim_ready_cb_t cb)
{
    if (!target) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)set_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_0);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, cb);
    lv_anim_start(&a);
}

void ui_anim_card_bounce(lv_obj_t *card)
{
    if (!card) return;
    /* bounce effect via y offset */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(card) + 5, lv_obj_get_y(card));
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_bounce);
    lv_anim_start(&a);
}

void ui_anim_cards_appear(lv_obj_t **cards, int count)
{
    if (!cards || count <= 0) return;

    for (int i = 0; i < count; i++) {
        if (!cards[i]) continue;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, cards[i]);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)set_opa);
        lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
        lv_anim_set_time(&a, 300);
        lv_anim_set_delay(&a, i * 40);
        lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
        lv_anim_start(&a);
    }
}