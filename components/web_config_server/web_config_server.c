#include "web_config_server.h"
#include "web_config.h"
#include "wifi_config.h"
#include "ui_lvgl.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../comms_server/discovery.h"
#include <string.h>

static const char *TAG = "WEB_CFG";
static httpd_handle_t s_server = NULL;
extern bool s_wifi_connected;
extern char s_sta_ip[16];
extern void update_wifi_info(void);

/* 切换纯 STA（供外部调用，如 Settings 的 "Reconnect Last"） */
void web_config_server_switch_to_sta(void)
{
    ESP_LOGI(TAG, "Switching to pure STA mode...");

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    s_sta_ip[0] = '\0';
    /* 注意：不设 s_wifi_connected=true —— 重连场景下 WiFi 还没真实连上 */

    /* WiFi 模式切换后重启 UDP discovery（旧 socket 已失效） */
    discovery_restart();
}

/* 等待 DHCP 完成后才触发切换 （在独立任务运行，避免 Timer 任务栈溢出） */
static void sta_switch_delayed_task(void *arg)
{
    (void)arg;
    /* 最多等 5 秒让 DHCP 拿 IP */
    for (int i = 0; i < 25; i++) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
                ESP_LOGI(TAG, "DHCP IP obtained, switching...");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    /* 直接在任务上下文切换（栈充足），不再经过定时器（Tmr Svc 栈小） */
    web_config_server_switch_to_sta();
    vTaskDelete(NULL);
}

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, WEB_CONFIG_HTML, -1);
    return ESP_OK;
}

static esp_err_t handler_wifi_status(httpd_req_t *req)
{
    char json[256];
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    const char *mode_str = (mode == WIFI_MODE_STA) ? "STA" : "AP";

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        snprintf(json, sizeof(json),
            "{\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"connected\":%s}",
            mode_str, WIFI_AP_IP, WIFI_AP_SSID,
            s_wifi_connected ? "true" : "false");
    } else {
        snprintf(json, sizeof(json),
            "{\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"connected\":%s}",
            mode_str, s_sta_ip, "DHCP", "true");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

static esp_err_t handler_wifi_test(httpd_req_t *req)
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
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");

    if (!ssid || !cJSON_IsString(ssid) || strlen(ssid->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"SSID required\"}");
        return ESP_FAIL;
    }

    const char *ssid_str = ssid->valuestring;
    const char *pass_str = (password && cJSON_IsString(password)) ? password->valuestring : "";

    ESP_LOGI(TAG, "Testing STA connection to SSID=%s", ssid_str);

    /* 临时切 APSTA 双模测试 */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t sta_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strncpy((char *)sta_cfg.sta.ssid, ssid_str, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass_str, sizeof(sta_cfg.sta.password) - 1);

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    /* 等待连接结果（最多5秒） */
    int wait_count = 50;
    bool success = false;
    while (wait_count-- > 0) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            success = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (success) {
        /* 获取 STA IP */
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
        char ip_str[16] = "0.0.0.0";
        if (netif) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(netif, &ip) == ESP_OK)
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
        }

        /* 连接成功 → 将凭据写入 NVS 供下次重连 */
        {
            nvs_handle_t nvs;
            if (nvs_open("wifi_last", NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_str(nvs, "ssid", ssid_str);
                nvs_set_str(nvs, "password", pass_str);
                nvs_commit(nvs);
                nvs_close(nvs);
                ESP_LOGI(TAG, "Saved last WiFi credentials to NVS");
            }
        }

        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"ip\":\"%s\"}", ip_str);

        /* 连接成功 → 立刻持LVGL锁刷新UI */
        s_wifi_connected = true;
        s_sta_ip[0] = '\0';
        ui_lvgl_lock();
        update_wifi_info();
        ui_lvgl_unlock();

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, -1);   // 先发响应

        /* 用独立任务等待 DHCP IP 后切换纯 STA */
        xTaskCreate(sta_switch_delayed_task, "sta_sw", 3072, NULL, 5, NULL);
    } else {
        /* 失败：回退 AP 模式 */
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_AP);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Connection failed or timeout\"}");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t web_config_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.server_port = WIFI_AP_PORT;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return ret;
    }

    httpd_uri_t uri[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_index},
        {.uri = "/api/wifi/status", .method = HTTP_GET, .handler = handler_wifi_status},
        {.uri = "/api/wifi/test", .method = HTTP_POST, .handler = handler_wifi_test},
    };
    for (int i = 0; i < 3; i++)
        httpd_register_uri_handler(s_server, &uri[i]);

    ESP_LOGI(TAG, "Web config: http://" WIFI_AP_IP "/");
    return ESP_OK;
}