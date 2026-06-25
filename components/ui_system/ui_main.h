#ifndef UI_MAIN_H
#define UI_MAIN_H

#include "esp_err.h"
#include "joystick.h"

/* ===== 全局状态栈 =====
 * 层级: MAINMENU(根,不可pop) → APP → SUB
 * 每层存实际状态值，由 ui_peek_state() / ui_push_state() 操作
 */
#define UI_STATE_MAINMENU 0
#define UI_STATE_APP      1
#define UI_STATE_SUB      2

/* WiFi 连接状态（由 main.c 的 WiFi 事件回调设置） */
extern bool s_wifi_connected;
extern char s_sta_ip[16];

void ui_main_menu_init(void);
void ui_main_handle_joystick(joystick_evt_t evt);

/* 外部 APP 可调用的更新接口 */
void update_wifi_info(void);

/* 状态栈操作 */
int  ui_get_depth(void);
void ui_push_state(void);  /* push APP */
void ui_push_sub(void);    /* push SUB */
void ui_pop_state(void);
int  ui_peek_state(void);  /* 查看当前顶层状态 */

#endif