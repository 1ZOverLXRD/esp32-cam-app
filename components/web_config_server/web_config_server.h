#ifndef WEB_CONFIG_SERVER_H
#define WEB_CONFIG_SERVER_H

#include "esp_err.h"

esp_err_t web_config_server_start(void);
void web_config_server_switch_to_sta(void);

#endif