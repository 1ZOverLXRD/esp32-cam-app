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



static void discovery_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "Socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8081),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening for UDP discovery on port 8081");

    uint8_t buf[32];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            ESP_LOGD(TAG, "recvfrom error: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        buf[n] = '\0';

        if (n >= 6 && memcmp(buf, "ESP32?", 6) == 0) {
            char resp[32];
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
            char ip_str[16] = "0.0.0.0";
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            }
            int rlen = snprintf(resp, sizeof(resp), "ESP32:%s", ip_str);
            sendto(fd, resp, rlen, 0,
                   (struct sockaddr *)&from, fromlen);
            ESP_LOGI(TAG, "Discovery from %s -> responded: %s",
                     inet_ntoa(from.sin_addr), ip_str);
        }
    }
}

void discovery_start(void)
{
    xTaskCreate(discovery_task, "discovery", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Discovery task created");
}