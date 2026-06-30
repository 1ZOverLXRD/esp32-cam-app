#ifndef COMMS_SERVER_H
#define COMMS_SERVER_H

#include "esp_err.h"
#include <stdint.h>

esp_err_t comms_server_init(void);

/** 向已连接的 Android 客户端发送数据包（cmd + payload）。
 *  若无客户端连接，静默忽略。非线程安全，仅在 LVGL task 上下文中调用。*/
void comms_server_send_packet(uint8_t cmd, const uint8_t *payload, uint32_t plen);

/** Android 客户端连接状态 */
extern volatile int g_android_connected;

/** Android 客户端 IP 和 UDP 端口（StreamStartUdp 时设置） */
extern uint32_t g_android_ip;
extern uint16_t g_android_port;

/** 注册APP命令回调（CMD 0x30-0x4F 路由到此回调）。传入NULL取消注册。 */
void comms_server_set_app_handler(void (*handler)(uint8_t cmd, uint16_t seq,
                                                    const uint8_t *payload, uint32_t plen));

#endif