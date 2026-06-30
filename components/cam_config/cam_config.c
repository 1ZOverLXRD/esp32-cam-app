#include "cam_config.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "CAM_CFG";
#define NVS_NS "cam_cfg"

const cam_config_t CAM_CONFIG_DEFAULT = {
    .resolution = 8,    // FRAMESIZE_HD
    .quality = 15,
    .mirror = 1,        // ON (镜像显示)
    .flip = 0,
    .brightness = 0,
    .contrast = 0,
    .wb_mode = 0,       // Auto
    .ae_level = 0,
};

void cam_config_load(cam_config_t *cfg)
{
    *cfg = CAM_CONFIG_DEFAULT;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS open failed (%d), using defaults", err);
        return;
    }

    nvs_get_u8(h, "resolution", &cfg->resolution);
    nvs_get_u8(h, "quality", &cfg->quality);
    nvs_get_u8(h, "mirror", &cfg->mirror);
    nvs_get_u8(h, "flip", &cfg->flip);
    nvs_get_i8(h, "brightness", &cfg->brightness);
    nvs_get_i8(h, "contrast", &cfg->contrast);
    nvs_get_u8(h, "wb_mode", &cfg->wb_mode);
    nvs_get_i8(h, "ae_level", &cfg->ae_level);

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: res=%u qual=%u mir=%u flip=%u "
             "bri=%d con=%d wb=%u ae=%d",
             cfg->resolution, cfg->quality, cfg->mirror, cfg->flip,
             cfg->brightness, cfg->contrast, cfg->wb_mode, cfg->ae_level);
}

esp_err_t cam_config_save(const cam_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %d", err);
        return err;
    }

    nvs_set_u8(h, "resolution", cfg->resolution);
    nvs_set_u8(h, "quality", cfg->quality);
    nvs_set_u8(h, "mirror", cfg->mirror);
    nvs_set_u8(h, "flip", cfg->flip);
    nvs_set_i8(h, "brightness", cfg->brightness);
    nvs_set_i8(h, "contrast", cfg->contrast);
    nvs_set_u8(h, "wb_mode", cfg->wb_mode);
    nvs_set_i8(h, "ae_level", cfg->ae_level);

    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved");
    } else {
        ESP_LOGE(TAG, "NVS commit failed: %d", err);
    }
    return err;
}