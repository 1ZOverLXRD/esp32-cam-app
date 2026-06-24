#ifndef APP_T_H
#define APP_T_H

#include "lvgl.h"
#include "joystick.h"

typedef struct {
    const char *name;       // 显示名称 (中文)
    const char *icon;        // 图标 (Unicode字符)
    uint32_t color;         // 卡片颜色 (RGB565: 0xRRGGBB)
    void (*on_create)(lv_obj_t *parent);
    void (*on_destroy)(void);
    void (*on_joystick)(joystick_evt_t evt);
    lv_obj_t *page;         // App LVGL 根对象
} app_t;

#endif