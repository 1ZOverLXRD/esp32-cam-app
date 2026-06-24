/*
 * joystick.c - QYF-860 PS2 摇杆驱动（ADC1 模式）
 *
 * 硬件连接：
 *   VRX → GPIO1（ADC1_CH0）
 *   VRY → GPIO2（ADC1_CH1）
 *   SW  → GPIO21（数字输入）
 *   VCC → 3.3V
 *   GND → GND
 */

#include "joystick.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "JOY";

/* GPIO 定义 */
#define PIN_VRX     GPIO_NUM_1      /* ADC1_CH0 */
#define PIN_VRY     GPIO_NUM_2      /* ADC1_CH1 */
#define PIN_SW      GPIO_NUM_21     /* 通用GPIO */

/* ADC 参数 */
#define ADC1_CHAN_X  ADC1_CHANNEL_0   /* GPIO1/VRX */
#define ADC1_CHAN_Y  ADC1_CHANNEL_1   /* GPIO2/VRY */
#define ADC_ATTEN    ADC_ATTEN_DB_12  /* 0-3.3V 全量程 */
#define ADC_WIDTH    ADC_WIDTH_BIT_12 /* 12bit: 0-4095 */

/* 方向判决阈值（12bit ADC） */
#define ADC_TH_HIGH  3000   /* 往一方向推到底 → 接近满幅 */
#define ADC_TH_LOW   800    /* 往反方向推到底 → 接近0 */
#define ADC_CENTER_L 1700   /* 中间范围下限 */
#define ADC_CENTER_H 2400   /* 中间范围上限 */

/* 事件队列 */
QueueHandle_t joystick_evt_queue = NULL;

/* 轮询任务句柄 */
static TaskHandle_t s_poll_task_handle = NULL;

/* 时间常量 */
#define PRESS_THRESHOLD_MS  80    // 按下阈值（降低到80ms提高响应）
#define LONG_PRESS_MS       800
#define POLL_INTERVAL_MS    30

esp_err_t joystick_init(void)
{
    /* 配置 SW 为数字输入 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // 上拉：松开=1，按下GND=0
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SW GPIO init failed");
        return err;
    }

    /* 配置 ADC1 通道 */
    adc1_config_width(ADC_WIDTH);
    adc1_config_channel_atten(ADC1_CHAN_X, ADC_ATTEN);
    adc1_config_channel_atten(ADC1_CHAN_Y, ADC_ATTEN);

    ESP_LOGI(TAG, "Joystick ADC init OK: X=GPIO1(ADC1_CH0), Y=GPIO2(ADC1_CH1), SW=GPIO21");

    joystick_evt_queue = xQueueCreate(10, sizeof(joystick_evt_t));
    if (!joystick_evt_queue) {
        ESP_LOGE(TAG, "Event queue create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* 读取ADC原始值 */
static void read_adc_raw(int *x_raw, int *y_raw)
{
    *x_raw = adc1_get_raw(ADC1_CHAN_X);
    *y_raw = adc1_get_raw(ADC1_CHAN_Y);
}

/* 根据ADC值判断方向 */
static int read_direction(void)
{
    int x, y;
    read_adc_raw(&x, &y);

    /* 调试：每30次打印一次ADC值（稍后改回poll task内） */
    // ESP_LOGI(TAG, "ADC raw: X=%d Y=%d", x, y);

    /* 中心检测：X和Y都在中间范围内 */
    if (x >= ADC_CENTER_L && x <= ADC_CENTER_H &&
        y >= ADC_CENTER_L && y <= ADC_CENTER_H) {
        return JOY_EVT_CENTER;
    }

    /* 方向判定：哪个轴偏离中心更多就判哪个方向 */
    int dx = x - (ADC_CENTER_L + ADC_CENTER_H) / 2;
    int dy = y - (ADC_CENTER_L + ADC_CENTER_H) / 2;

    if (abs(dx) > abs(dy)) {
        return (dx < 0) ? JOY_EVT_LEFT : JOY_EVT_RIGHT;
    } else {
        return (dy < 0) ? JOY_EVT_UP : JOY_EVT_DOWN;
    }
}

float joystick_read_x(void)
{
    int raw = adc1_get_raw(ADC1_CHAN_X);
    return (float)raw / 4095.0f;
}

float joystick_read_y(void)
{
    int raw = adc1_get_raw(ADC1_CHAN_Y);
    return (float)raw / 4095.0f;
}

int joystick_read_sw(void)
{
    return gpio_get_level(PIN_SW);
}

void joystick_send_event(joystick_evt_t evt)
{
    if (!joystick_evt_queue) return;
    xQueueSend(joystick_evt_queue, &evt, 0);
}

/* 轮询任务 */
static void joystick_poll_task(void *arg)
{
    (void)arg;

    int last_dir = JOY_EVT_CENTER;
    int sw_pressed = 0;
    TickType_t sw_press_ticks = 0;
    int long_press_sent = 0;
    int auto_repeat_count = 0;

    ESP_LOGI(TAG, "Poll task started");

    while (1) {
            /* 方向检测（ADC） */
            int dir = read_direction();
            if (dir != last_dir) {
                joystick_send_event(dir);
                last_dir = dir;
                auto_repeat_count = 0;
            } else if (dir != JOY_EVT_CENTER) {
                /* 按住方向时，每200ms重复发一次事件（持续移动） */
                auto_repeat_count++;
                if (auto_repeat_count >= 7) {  // 30ms × 7 ≈ 200ms
                    joystick_send_event(dir);
                    auto_repeat_count = 0;
                }
            }

            /* SW 按键（三采样消抖，低电平有效） */
        // debug_count++;
        // if (debug_count >= 100) {
        //     int x, y;
        //     read_adc_raw(&x, &y);
        //     ESP_LOGI(TAG, "ADC: X=%d Y=%d dir=%d sw=%d", x, y, dir, gpio_get_level(PIN_SW));
        //     debug_count = 0;
        // }

        /* SW 按键（三采样消抖，低电平有效） */
        int sw_samples = 0;
        for (int i = 0; i < 3; i++) {
            sw_samples += gpio_get_level(PIN_SW);
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        int sw_now = (sw_samples >= 2) ? 1 : 0; // 多数决：1=松开, 0=按下

        /* 按下检测（低电平有效） */
        if (sw_now == 0 && sw_pressed == 0) {
            sw_pressed = 1;
            sw_press_ticks = xTaskGetTickCount();
            long_press_sent = 0;
        }
        /* 松开检测（始终发送PRESS，不受long_press影响） */
        else if (sw_now == 1 && sw_pressed == 1) {
            sw_pressed = 0;
            TickType_t elapsed = xTaskGetTickCount() - sw_press_ticks;
            if ((elapsed * portTICK_PERIOD_MS) >= PRESS_THRESHOLD_MS)
                joystick_send_event(JOY_EVT_PRESS);
            long_press_sent = 0;
        }
        /* 长按检测 */
        else if (sw_now == 0 && sw_pressed == 1 && !long_press_sent) {
            TickType_t elapsed = xTaskGetTickCount() - sw_press_ticks;
            if ((elapsed * portTICK_PERIOD_MS) >= LONG_PRESS_MS) {
                joystick_send_event(JOY_EVT_LONG_PRESS);
                long_press_sent = 1;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t joystick_start_poll_task(void)
{
    if (s_poll_task_handle) {
        ESP_LOGW(TAG, "Already running");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(joystick_poll_task, "joy_poll", 3072, NULL, 5, &s_poll_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Poll task created");
    return ESP_OK;
}