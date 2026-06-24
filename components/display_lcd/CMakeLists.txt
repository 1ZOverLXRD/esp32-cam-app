idf_component_register(SRCS "lcd_st7789.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lcd esp_timer)