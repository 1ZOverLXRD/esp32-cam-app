idf_component_register(SRCS "ui_main.c" "ui_anim.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl display_lcd ui_lvgl input_joystick app_launcher app_settings app_weather app_camera app_trash)