#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "lcd_st7789.h"
#include "joystick.h"
#include "ui_lvgl.h"
#include "ui_main.h"
#include "comms_server.h"
#include "web_config_server.h"
#include "wifi_config.h"
// Network init
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"

static const char *TAG = "MAIN";
static void joy_event_task(void *arg);
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data);

static void wifi_ap_init(void)
{
    ESP_LOGI(TAG, "Init WiFi AP...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = 0,
            .password = WIFI_AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started: " WIFI_AP_SSID " / " WIFI_AP_PASS);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
        &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
        &wifi_event_handler, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-CAM Smart Desktop === v3");

    if (esp_psram_get_size() == 0) {
        ESP_LOGE(TAG, "PSRAM NOT DETECTED! Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    ESP_LOGI(TAG, "PSRAM: %d KB", esp_psram_get_size() / 1024);

    // ---- LCD ----
    ESP_LOGI(TAG, "Init LCD...");
    ESP_ERROR_CHECK(lcd_st7789_init());
    ESP_LOGI(TAG, "LCD OK");

    // ---- Joystick ----
    ESP_LOGI(TAG, "Init Joystick...");
    ESP_ERROR_CHECK(joystick_init());
    ESP_LOGI(TAG, "Joystick OK");

    // ---- LVGL ----
    ESP_LOGI(TAG, "Init LVGL...");
    ESP_ERROR_CHECK(ui_lvgl_init());
    ESP_LOGI(TAG, "LVGL OK");

    // ---- Poll ----
    ESP_LOGI(TAG, "Start joystick poll...");
    ESP_ERROR_CHECK(joystick_start_poll_task());

    // ---- UI ----
    ESP_LOGI(TAG, "Init UI menu...");
    ui_main_menu_init();
    ui_lvgl_start_task();  // 卡片创建完成后才启动 LVGL 刷新任务
    ESP_LOGI(TAG, "UI menu OK");

    // ---- 摇杆事件任务（提前启动，不等WiFi） ----
    xTaskCreatePinnedToCore(joy_event_task, "joy_evt", 4096, NULL, 4, NULL, 1);

    // ---- WiFi AP ----
    wifi_ap_init();
    ESP_LOGI(TAG, "WiFi OK");

    // ---- Comms ----
    ESP_LOGI(TAG, "Init comms...");
    ESP_ERROR_CHECK(comms_server_init());
    ESP_LOGI(TAG, "Comms OK");

    // ---- Web config ----
    ESP_LOGI(TAG, "Init web config...");
    ESP_ERROR_CHECK(web_config_server_start());
    ESP_LOGI(TAG, "All init OK!");

    // ---- Event task ----
    xTaskCreatePinnedToCore(joy_event_task, "joy_evt", 4096, NULL, 4, NULL, 1);

    while (1) {
        ESP_LOGI(TAG, "Main alive");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void async_update_wifi(void *arg) {
    (void)arg;
    extern void update_wifi_info(void);
    update_wifi_info();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
        lv_async_call(&async_update_wifi, NULL);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "STA disconnected");
        s_wifi_connected = false;
        lv_async_call(&async_update_wifi, NULL);
    }
}

static void joy_event_task(void *arg)
{
    joystick_evt_t evt;
    while (1) {
        if (xQueueReceive(joystick_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            ui_lvgl_lock();
            ui_main_handle_joystick(evt);
            ui_lvgl_unlock();
        }
    }
}