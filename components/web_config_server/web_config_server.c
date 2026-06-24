#include "web_config_server.h"
#include "web_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "WEB_CFG";
static httpd_handle_t s_server = NULL;

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, WEB_CONFIG_HTML, -1);
    return ESP_OK;
}

static esp_err_t handler_wifi_status(httpd_req_t *req)
{
    char json[128];
    snprintf(json, sizeof(json),
        "{\"mode\":\"AP\",\"ip\":\"192.168.4.1\",\"ssid\":\"ESP32-CAM\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

static esp_err_t handler_wifi_save(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "", -1);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid JSON", -1);
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");
    cJSON *mode = cJSON_GetObjectItem(root, "mode");

    if (ssid && cJSON_IsString(ssid)) {
        nvs_handle_t nvs;
        if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "ssid", ssid->valuestring);
            if (password && cJSON_IsString(password))
                nvs_set_str(nvs, "password", password->valuestring);
            if (mode && cJSON_IsString(mode))
                nvs_set_str(nvs, "mode", mode->valuestring);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Config saved: SSID=%s", ssid->valuestring);
        }
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    return ESP_OK;
}

esp_err_t web_config_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 6;
    config.server_port = 80;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return ret;
    }

    httpd_uri_t uri[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_index},
        {.uri = "/api/wifi/status", .method = HTTP_GET, .handler = handler_wifi_status},
        {.uri = "/api/wifi/save", .method = HTTP_POST, .handler = handler_wifi_save},
    };
    for (int i = 0; i < 3; i++)
        httpd_register_uri_handler(s_server, &uri[i]);

    ESP_LOGI(TAG, "Web config: http://192.168.4.1/");
    return ESP_OK;
}