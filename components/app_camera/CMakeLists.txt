idf_component_register(SRCS "app_camera.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl__lvgl ui_system esp32-camera)
