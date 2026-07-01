#ifndef UI_MAIN_H
#define UI_MAIN_H

#include "esp_err.h"
#include "joystick.h"

/* ===== 全局状态栈 ===== */
#define UI_STATE_MAINMENU 0
#define UI_STATE_APP      1
#define UI_STATE_SUB      2

/* WiFi 连接状态（由 main.c 的 WiFi 事件回调设置） */
extern bool s_wifi_connected;
extern char s_sta_ip[16];
/* 重连中止标志 — 超时后停止 STA 自动重连 */
extern volatile bool s_reconnect_abort;

void ui_main_menu_init(void);
void ui_main_handle_joystick(joystick_evt_t evt);

int  ui_get_depth(void);
void ui_push_state(void);
void ui_push_sub(void);
void ui_pop_state(void);
int  ui_peek_state(void);

/* WiFi UI 更新（由摇杆任务调用，已在 LVGL 锁内） */
void update_wifi_info(void);

#endif