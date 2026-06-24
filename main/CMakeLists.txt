idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES display_lcd input_joystick ui_lvgl ui_system app_launcher app_settings app_weather web_config_server comms_server esp_psram esp_lcd nvs_flash esp_netif esp_wifi esp_event)