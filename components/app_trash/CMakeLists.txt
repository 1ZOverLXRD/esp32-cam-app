idf_component_register(SRCS "app_trash.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl app_launcher comms_server cam_config esp32-camera)