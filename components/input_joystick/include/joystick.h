#ifndef JOYSTICK_H
#define JOYSTICK_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JOY_EVT_NONE = 0,
    JOY_EVT_UP,
    JOY_EVT_DOWN,
    JOY_EVT_LEFT,
    JOY_EVT_RIGHT,
    JOY_EVT_PRESS,
    JOY_EVT_LONG_PRESS,
    JOY_EVT_CENTER,
} joystick_evt_t;

extern QueueHandle_t joystick_evt_queue;

esp_err_t joystick_init(void);
esp_err_t joystick_start_poll_task(void);
float joystick_read_x(void);
float joystick_read_y(void);
int joystick_read_sw(void);
void joystick_send_event(joystick_evt_t evt);

#ifdef __cplusplus
}
#endif

#endif