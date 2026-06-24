idf_component_register(SRCS "web_config_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server json nvs_flash)