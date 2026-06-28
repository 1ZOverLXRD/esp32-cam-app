#include "comms_server.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

static const char *TAG = "COMMS";
static int s_server_fd = -1;
static int s_client_fd = -1;

uint32_t g_android_ip = 0;
uint16_t g_android_port = 0;

#define PORT 8080

/* 读一个完整二进制包，返回0=成功, -1=断开 */
static int read_packet(int fd, uint8_t *cmd, uint16_t *seq,
                        uint8_t *buf, uint32_t *len)
{
    uint8_t header[7];
    int n = read(fd, header, 7);
    if (n <= 0) return -1;
    if (n < 7) return -1;

    uint32_t pktlen = (uint32_t)header[0]
                    | ((uint32_t)header[1] << 8)
                    | ((uint32_t)header[2] << 16)
                    | ((uint32_t)header[3] << 24);
    *cmd = header[4];
    *seq = (uint16_t)header[5] | ((uint16_t)header[6] << 8);

    if (pktlen > 3) {
        uint32_t to_read = pktlen - 3;
        uint32_t offset = 0;
        while (offset < to_read) {
            n = read(fd, buf + offset, to_read - offset);
            if (n <= 0) return -1;
            offset += n;
        }
        *len = to_read;
    } else {
        *len = 0;
    }
    return 0;
}

/* 发响应包 */
static void send_response(int fd, uint8_t cmd, uint16_t seq,
                           const uint8_t *payload, uint32_t plen)
{
    uint32_t total = 3 + plen;  // CMD + SEQ + payload
    uint8_t header[7];
    header[0] = total & 0xFF;
    header[1] = (total >> 8) & 0xFF;
    header[2] = (total >> 16) & 0xFF;
    header[3] = (total >> 24) & 0xFF;
    header[4] = cmd;
    header[5] = seq & 0xFF;
    header[6] = (seq >> 8) & 0xFF;

    write(fd, header, 7);
    if (plen > 0) write(fd, payload, plen);
}

static void send_error(int fd, uint16_t seq, uint8_t orig_cmd, uint8_t err)
{
    uint8_t payload[2] = {orig_cmd, err};
    send_response(fd, 0xFF, seq, payload, 2);
}

/* 命令分发 */
static void handle_packet(int fd, uint8_t cmd, uint16_t seq,
                           const uint8_t *payload, uint32_t plen)
{
    switch (cmd) {
    case 0x01: // IsValid
        send_response(fd, 0x01, seq, NULL, 0);
        ESP_LOGI(TAG, "IsValid");
        break;

    case 0x02: { // GetWeather
        uint8_t resp[36];
        memset(resp, 0, sizeof(resp));
        resp[0] = 0xC4; resp[1] = 0x09;  // temp=25.00°C
        resp[2] = 60;                      // humidity=60%
        resp[3] = 0;                       // code=clear
        strcpy((char*)resp + 4, "Zhuzhou");
        send_response(fd, 0x02, seq, resp, sizeof(resp));
        ESP_LOGI(TAG, "GetWeather: 25°C 60%%");
        break;
    }

    case 0x03: // GetSingleImage — not yet implemented
        send_error(fd, seq, cmd, 0x03); // resource unavailable
        break;

    case 0x04: { // CityInfo
        uint8_t city[32] = {0};
        strcpy((char*)city, "Zhuzhou");
        send_response(fd, 0x04, seq, city, 32);
        ESP_LOGI(TAG, "CityInfo: Zhuzhou");
        break;
    }

    case 0x10: // StartCamera
        send_response(fd, 0x10, seq, NULL, 0);
        ESP_LOGI(TAG, "StartCamera (stub)");
        break;

    case 0x11: // StopCamera
        send_response(fd, 0x11, seq, NULL, 0);
        ESP_LOGI(TAG, "StopCamera (stub)");
        break;

    case 0x20: { // StreamStartUdp
        if (plen >= 2) {
            uint16_t android_port = payload[0] | (payload[1] << 8);
            struct sockaddr_in client;
            socklen_t client_len = sizeof(client);
            if (getpeername(fd, (struct sockaddr *)&client, &client_len) == 0) {
                g_android_ip = client.sin_addr.s_addr;
                g_android_port = android_port;
                ESP_LOGI(TAG, "StreamStartUdp -> %s:%u",
                         inet_ntoa(client.sin_addr), android_port);
            }
            uint8_t info[5];
            info[0] = 640 & 0xFF; info[1] = (640 >> 8) & 0xFF;
            info[2] = 480 & 0xFF; info[3] = (480 >> 8) & 0xFF;
            info[4] = 12;
            send_response(fd, 0x20, seq, info, 5);
        }
        break;
    }

    case 0x21: // StreamStopUdp
        send_response(fd, 0x21, seq, NULL, 0);
        ESP_LOGI(TAG, "StreamStopUdp");
        break;

    default:
        ESP_LOGW(TAG, "Unknown cmd: 0x%02X", cmd);
        send_error(fd, seq, cmd, 0x01);
        break;
    }
}

/* TCP Server 任务 */
static void tcp_server_task(void *arg)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval timeout = {10, 0};
    setsockopt(s_server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed");
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_server_fd, 1) < 0) {
        ESP_LOGE(TAG, "Listen failed");
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening on port %d", PORT);

    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        s_client_fd = accept(s_server_fd, (struct sockaddr *)&client, &client_len);

        if (s_client_fd < 0) {
            ESP_LOGE(TAG, "Accept failed");
            continue;
        }

        ESP_LOGI(TAG, "Android connected");
        uint8_t cmd, buf[512];
        uint16_t seq;
        uint32_t plen;

        while (read_packet(s_client_fd, &cmd, &seq, buf, &plen) == 0)
            handle_packet(s_client_fd, cmd, seq, buf, plen);

        ESP_LOGI(TAG, "Android disconnected");
        close(s_client_fd);
        s_client_fd = -1;
    }
}

esp_err_t comms_server_init(void)
{
    xTaskCreatePinnedToCore(tcp_server_task, "comms", 4096, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "Comms server started (binary protocol)");
    return ESP_OK;
}