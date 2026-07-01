/**
 * Simple UDP discovery for ESP32 Camera
 *
 * Listens on UDP port 8081 for broadcast "ESP32?"
 * Responds with: "ESP32:xxx.xxx.xxx.xxx"
 */

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DISCOVERY";
static TaskHandle_t s_disco_task = NULL;
static volatile bool s_disco_stop = false;

static void discovery_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "Socket create failed");
        s_disco_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8081),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(fd);
        s_disco_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening for UDP discovery on port 8081");

    uint8_t buf[32];
    while (!s_disco_stop) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (s_disco_stop) break;
            ESP_LOGW(TAG, "recvfrom error: %d, retrying...", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        buf[n] = '\0';

        if (n >= 6 && memcmp(buf, "ESP32?", 6) == 0) {
            char resp[32];
            char ip_str[16] = "0.0.0.0";
            for (esp_netif_t *ni = esp_netif_next(NULL); ni; ni = esp_netif_next(ni)) {
                const char *key = esp_netif_get_ifkey(ni);
                if (key && strstr(key, "AP")) continue;
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(ni, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    break;
                }
            }
            int rlen = snprintf(resp, sizeof(resp), "ESP32:%s", ip_str);
            sendto(fd, resp, rlen, 0,
                   (struct sockaddr *)&from, fromlen);
            ESP_LOGI(TAG, "Discovery from %s -> responded: %s",
                     inet_ntoa(from.sin_addr), ip_str);
        }
    }

    close(fd);
    s_disco_task = NULL;
    ESP_LOGI(TAG, "Discovery task stopped");
    vTaskDelete(NULL);
}

void discovery_start(void)
{
    if (s_disco_task) return;  // already running
    s_disco_stop = false;
    xTaskCreate(discovery_task, "discovery", 3072, NULL, 4, &s_disco_task);
    ESP_LOGI(TAG, "Discovery task created");
}

void discovery_restart(void)
{
    ESP_LOGI(TAG, "Restarting discovery...");
    if (s_disco_task) {
        s_disco_stop = true;
        vTaskDelay(pdMS_TO_TICKS(200));  // wait for old task to exit
    }
    s_disco_stop = false;
    xTaskCreate(discovery_task, "discovery", 3072, NULL, 4, &s_disco_task);
    ESP_LOGI(TAG, "Discovery task restarted");
}