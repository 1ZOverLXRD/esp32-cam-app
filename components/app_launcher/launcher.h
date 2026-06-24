#ifndef LAUNCHER_H
#define LAUNCHER_H

#include "esp_err.h"
#include "app_t.h"

esp_err_t launcher_init(void);
esp_err_t launcher_register_app(app_t *app);
int launcher_get_app_count(void);
app_t *launcher_get_app(int index);
void launcher_open_app(int index);
void launcher_close_app(void);
bool launcher_is_app_open(void);
void launcher_handle_joystick(joystick_evt_t evt);

#endif