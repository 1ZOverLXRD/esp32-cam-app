idf_component_register(SRCS "cam_config.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_netif esp32-camera)