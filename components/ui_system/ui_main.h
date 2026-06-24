#ifndef UI_MAIN_H
#define UI_MAIN_H

#include "esp_err.h"
#include "joystick.h"

void ui_main_menu_init(void);
void ui_main_handle_joystick(joystick_evt_t evt);

#endif