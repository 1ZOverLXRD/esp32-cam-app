#pragma once
#include "stdint.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t resolution;   // framesize_t: 8=HD, 5=VGA, 4=QVGA
    uint8_t quality;      // JPEG quality 0-30
    uint8_t mirror;       // 0/1
    uint8_t flip;         // 0/1
    int8_t  brightness;   // -2~+2
    int8_t  contrast;     // -2~+2
    uint8_t wb_mode;      // 0=Auto 1=Sunny 2=Cloudy 3=Fluorescent
    int8_t  ae_level;     // -2~+2
} cam_config_t;

extern const cam_config_t CAM_CONFIG_DEFAULT;

void cam_config_load(cam_config_t *cfg);
esp_err_t cam_config_save(const cam_config_t *cfg);

#ifdef __cplusplus
}
#endif
