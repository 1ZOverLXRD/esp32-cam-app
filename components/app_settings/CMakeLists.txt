idf_component_register(SRCS "app_settings.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl app_launcher wifi_config cam_config esp_wifi nvs_flash)