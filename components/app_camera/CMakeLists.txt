idf_component_register(SRCS "app_camera.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl__lvgl ui_main esp32-camera)
