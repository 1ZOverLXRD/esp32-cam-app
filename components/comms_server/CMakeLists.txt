idf_component_register(SRCS "comms_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES lwip esp_timer app_launcher)