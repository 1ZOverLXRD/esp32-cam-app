#ifndef UI_ANIM_H
#define UI_ANIM_H

#include "lvgl.h"

void ui_anim_app_open(lv_obj_t *target, lv_anim_ready_cb_t cb);
void ui_anim_app_close(lv_obj_t *target, lv_anim_ready_cb_t cb);
void ui_anim_card_bounce(lv_obj_t *card);
void ui_anim_cards_appear(lv_obj_t **cards, int count);

#endif