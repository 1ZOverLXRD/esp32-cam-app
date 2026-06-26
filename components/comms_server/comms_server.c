#include "comms_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "COMMS";
static int s_server_fd = -1;
static int s_client_fd = -1;

#define PORT 8080
#define BUF_SIZE 512

/* 收到天气数据 */
static void handle_weather(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *tmp = cJSON_GetObjectItem(root, "tmp");
    cJSON *hum = cJSON_GetObjectItem(root, "hum");
    cJSON *cd = cJSON_GetObjectItem(root, "cd");
    cJSON *wnd = cJSON_GetObjectItem(root, "wnd");

    float temp = tmp ? (float)tmp->valuedouble : 0;
    int humidity = hum ? hum->valueint : 0;
    int code = cd ? cd->valueint : 0;
    float wind = wnd ? (float)wnd->valuedouble : 0;

    ESP_LOGI(TAG, "Weather: %.1f°C, %d%%, code=%d, wind=%.1f", temp, humidity, code, wind);

    /* TODO: 更新天气 App 显示 */
    cJSON_Delete(root);
}

/* 收到垃圾识别结果 */
static void handle_trash(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *cat = cJSON_GetObjectItem(root, "c");
    cJSON *prob = cJSON_GetObjectItem(root, "p");

    const char *category = cat ? cat->valuestring : "未知";
    float confidence = prob ? (float)prob->valuedouble : 0;

    ESP_LOGI(TAG, "Trash: %s (%.1f%%)", category, confidence);

    /* TODO: 更新垃圾识别 App 显示 */
    cJSON_Delete(root);
}

/* 处理接收到的 JSON 消息 */
static void process_message(const char *msg)
{
    cJSON *root = cJSON_Parse(msg);
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON: %s", msg);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "t");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "weather") == 0) {
        handle_weather(msg);
    } else if (strcmp(type->valuestring, "trash") == 0) {
        handle_trash(msg);
    } else {
        ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
    }

    cJSON_Delete(root);
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

    struct timeval timeout = {10, 0}; // recv 10秒超时
    setsockopt(s_server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_server_fd, 1) < 0) {
        ESP_LOGE(TAG, "Socket listen failed");
        close(s_server_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on port %d", PORT);

    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        s_client_fd = accept(s_server_fd, (struct sockaddr *)&client, &client_len);

        if (s_client_fd < 0) {
            ESP_LOGE(TAG, "Accept failed");
            continue;
        }

        char buf[BUF_SIZE];
        int len;

        ESP_LOGI(TAG, "Android connected");

        while ((len = read(s_client_fd, buf, sizeof(buf) - 1)) > 0) {
            buf[len] = '\0';
            /* 可能有多行 JSON */
            char *line = buf;
            char *nl;
            while ((nl = strchr(line, '\n')) != NULL) {
                *nl = '\0';
                if (strlen(line) > 0) {
                    process_message(line);
                }
                line = nl + 1;
            }
            /* 剩余不完整行留在 buffer 中（简化处理） */
        }

        ESP_LOGI(TAG, "Android disconnected");
        close(s_client_fd);
        s_client_fd = -1;
    }
}

esp_err_t comms_server_init(void)
{
    xTaskCreatePinnedToCore(tcp_server_task, "comms", 4096, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "Comms server started");
    return ESP_OK;
}