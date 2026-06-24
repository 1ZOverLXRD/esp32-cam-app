#ifndef UI_LVGL_H
#define UI_LVGL_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_lvgl_init(void);
void ui_lvgl_start_task(void);
lv_disp_t *ui_lvgl_get_display(void);

#ifdef __cplusplus
}
#endif

#endif