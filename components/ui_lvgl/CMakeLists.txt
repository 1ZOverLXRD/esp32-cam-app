idf_component_register(SRCS "ui_lvgl.c" "cn_font_16.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl display_lcd esp_timer)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_DISP_DEF_REFR_PERIOD=20)