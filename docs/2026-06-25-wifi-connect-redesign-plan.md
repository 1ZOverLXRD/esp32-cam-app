# WiFi 连接机制重设计 — 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ESP32 AP 模式供客机配置 → 测试 STA 连接 → 成功后切纯 STA 模式，APP 门禁同步解锁。

**Architecture:** AP 模式保持不变，测试阶段临时切 APSTA 双模，成功后用延迟定时器切换到纯 STA。APP 门禁由全局 `s_wifi_connected` 控制，Settings 中的 WiFi Info 段根据连接状态动态显隐子项。

**Tech Stack:** ESP-IDF v5.5.2, LVGL 8.4, ESP32-S3

**Platform:** Embedded (ESP-IDF, no pytest), build verification = test

## 全局约束

- NVS 不保存 STA 凭据，掉电恢复 AP 模式
- AP SSID = `ESP32-CAM`, AP 密码 = `12345678`, AP IP = `192.168.4.1`
- `httpd_stop()` 禁止在 HTTP handler 内直接调用 — 使用延迟定时器
- 子项左对齐 x=8px，启用 `LV_LABEL_LONG_WRAP` 多行换行
- 所有 E符号（extern）放在对应 .h 文件，不要跨文件直接 extern

---

## 文件结构

| 文件 | 状态 | 职责 |
|------|------|------|
| `components/wifi_config/wifi_config.h` | **新建** | AP SSID/Password/IP 常量宏 |
| `components/web_config_server/web_config.h` | 修改 | 简化 HTML 页面 |
| `components/web_config_server/web_config_server.c` | 修改 | 删 `/api/wifi/save`；/test 改为 APSTA 双模 + 延迟切换 |
| `main/main.c` | 修改 | WiFi 事件回调；设置 s_wifi_connected |
| `components/ui_system/ui_main.c` | 修改 | 加 s_wifi_connected/s_sta_ip 标志；APP 门禁 + 全屏阻断叠层 |
| `components/ui_system/ui_main.h` | 修改 | 导出 s_wifi_connected 和 s_sta_ip |
| `components/app_settings/app_settings.c` | 修改 | WiFi Info 动态子项；左对齐；多行换行 |

---

### Task 1: 新建 wifi_config.h 共享常量

**Files:**
- Create: `components/wifi_config/wifi_config.h`
- Modify: `components/web_config_server/CMakeLists.txt`（注册新 component）

**Interfaces:**
- Consumes: (nothing)
- Produces: `#define WIFI_AP_SSID "ESP32-CAM"`, `WIFI_AP_PASS "12345678"`, `WIFI_AP_IP "192.168.4.1"`

- [ ] **Step 1: Create wifi_config.h**

```c
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define WIFI_AP_SSID    "ESP32-CAM"
#define WIFI_AP_PASS    "12345678"
#define WIFI_AP_IP      "192.168.4.1"
#define WIFI_AP_PORT    80

#endif
```

- [ ] **Step 2: Create CMakeLists.txt**

```
idf_component_register(SRCS ""
                       INCLUDE_DIRS ".")
```

- [ ] **Step 3: Verify compilation**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -5
```
Expected: build succeeds, no errors.

- [ ] **Step 4: Commit**

```bash
cd /d/Project/ESP32/esp32-cam-app && git add components/wifi_config/ && git commit -m "feat: add wifi_config.h shared constants"
```

---

### Task 2: 简化 HTML 配置页

**Files:**
- Modify: `components/web_config_server/web_config.h`

**Interfaces:**
- Consumes: (self-contained HTML string change)
- Produces: Simplified HTML with SSID/Password/Connect only

- [ ] **Step 1: Rewrite web_config.h**

去除模式选择、保存按钮。替换为简化版本：

```c
#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

static const char WEB_CONFIG_HTML[] =
"<!DOCTYPE html>"
"<html lang='zh-CN'>"
"<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-CAM WiFi</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box;font-family:sans-serif}"
"body{background:#0f0f23;color:#fff;display:flex;justify-content:center;align-items:center;min-height:100vh;padding:20px}"
".c{background:#1a1a3e;border-radius:16px;padding:30px;width:100%;max-width:400px}"
"h1{text-align:center;font-size:20px;margin-bottom:20px}"
"label{font-size:13px;color:#aaa;display:block;margin:8px 0 4px}"
"input{width:100%;padding:12px;border-radius:10px;border:1px solid #333;background:#0f0f23;color:#fff;font-size:15px;margin-bottom:12px}"
".btn{width:100%;padding:14px;border:none;border-radius:12px;font-size:16px;cursor:pointer;margin:6px 0;background:#6c5ce7;color:#fff}"
".st{padding:12px;border-radius:10px;margin:12px 0;font-size:14px;display:none}"
".ok{border:1px solid #00b894;display:block}"
".er{border:1px solid #e17055;display:block}"
".wa{border:1px solid #fdcb6e;display:block}"
"</style>"
"</head><body>"
"<div class='c'>"
"<h1>连接 WiFi</h1>"
"<div id='s' class='st'></div>"
"<label>WiFi SSID</label>"
"<input id='ssid' placeholder='输入 WiFi 名称' />"
"<label>密码</label>"
"<input id='pwd' type='password' placeholder='输入 WiFi 密码' />"
"<button class='btn' onclick='connect()'>Connect</button>"
"<div style='margin-top:16px;padding-top:12px;border-top:1px solid #333'>"
"<p style='font-size:12px;color:#666;text-align:center' id='info'>等待连接...</p>"
"</div>"
"</div>"
"<script>"
"function st(m,t){var e=document.getElementById('s');e.textContent=m;e.className='st '+t}"
"async function connect(){"
"var ss=document.getElementById('ssid').value;"
"var pw=document.getElementById('pwd').value;"
"if(!ss){st('请输入 WiFi 名称','er');return}"
"st('连接中...','wa');"
"try{"
"var r=await fetch('/api/wifi/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ss,password:pw})});"
"var j=await r.json();"
"if(j&&j.ok){st('连接成功！正在切换到新网络...','ok')}"
"else{st('连接失败: '+(j.msg||'未知错误'),'er')}"
"}catch(e){st('请求失败: '+e.message,'er')}"
"}"
"async function load(){"
"try{"
"var r=await fetch('/api/wifi/status');"
"var j=await r.json();"
"if(j)document.getElementById('info').textContent='模式: '+(j.mode||'--')+' | IP: '+(j.ip||'--')"
"}catch(e){}"
"}"
"load()"
"</script>"
"</body></html>";

#endif
```

- [ ] **Step 2: Verify compilation**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -5
```

- [ ] **Step 3: Commit**

```bash
git add components/web_config_server/web_config.h && git commit -m "feat: simplify WiFi config HTML page"
```

---

### Task 3: 重构 web_config_server — 删 /api/wifi/save + APSTA 双模测试 + Deferred 切换

**Files:**
- Modify: `components/web_config_server/web_config_server.c`

**Interfaces:**
- Consumes: `WIFI_AP_SSID`, `WIFI_AP_PASS` from wifi_config.h
- Consumes: `extern bool s_wifi_connected` from ui_main.h
- Produces: `/api/wifi/test` handler with APSTA dual-mode + deferred STA switch

- [ ] **Step 1: Rewrite web_config_server.c**

```c
#include "web_config_server.h"
#include "web_config.h"
#include "wifi_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "WEB_CFG";
static httpd_handle_t s_server = NULL;
extern bool s_wifi_connected;
extern char s_sta_ip[16];

/* 延迟切换纯 STA — 不能在 HTTP handler 内调 httpd_stop */
static void sta_switch_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGI(TAG, "Switching to pure STA mode...");

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_connect();

    s_wifi_connected = true;

    /* 获取 STA IP */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(netif, &ip);
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ip.ip));
    }

    ESP_LOGI(TAG, "STA mode active, IP: %s", s_sta_ip);
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
    bool connected = s_wifi_connected;

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        snprintf(json, sizeof(json),
            "{\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"connected\":%s}",
            mode_str, WIFI_AP_IP, WIFI_AP_SSID,
            connected ? "true" : "false");
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

    /* 等待连接结果（最多10秒） */
    int wait_count = 100;
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

        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"ip\":\"%s\"}", ip_str);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, -1);   // 先发响应

        /* 创建延迟定时器切换纯 STA */
        TimerHandle_t timer = xTimerCreate("sta_sw", pdMS_TO_TICKS(500), pdFALSE,
                                            NULL, sta_switch_timer_cb);
        if (timer) xTimerStart(timer, 0);
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
```

- [ ] **Step 2: Verify compilation**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -10
```

- [ ] **Step 3: Commit**

```bash
git add components/web_config_server/web_config_server.c && git commit -m "feat: rewrite WiFi test with APSTA dual-mode + deferred STA switch"
```

---

### Task 4: WiFi 事件回调 + 全局标志 ui_main.c / ui_main.h

**Files:**
- Modify: `components/ui_system/ui_main.h`
- Modify: `components/ui_system/ui_main.c`

**Interfaces:**
- Consumes: (nothing external)
- Produces: `extern bool s_wifi_connected`, `extern char s_sta_ip[16]`
- Produces: 门禁逻辑（`open_current_app` 中判断）+ 全屏阻断叠层

- [ ] **Step 1: 修改 ui_main.h，导出全局标志**

```c
#ifndef UI_MAIN_H
#define UI_MAIN_H

#include "esp_err.h"
#include "joystick.h"

/* ===== 全局状态栈 ===== */
#define UI_STATE_MAINMENU 0
#define UI_STATE_APP      1
#define UI_STATE_SUB      2

/* WiFi 连接状态（由 main.c 的 WiFi 事件回调设置） */
extern bool s_wifi_connected;
extern char s_sta_ip[16];

void ui_main_menu_init(void);
void ui_main_handle_joystick(joystick_evt_t evt);

int  ui_get_depth(void);
void ui_push_state(void);
void ui_push_sub(void);
void ui_pop_state(void);
int  ui_peek_state(void);

#endif
```

- [ ] **Step 2: 修改 ui_main.c**

加入 `s_wifi_connected`、`s_sta_ip` 定义和门禁逻辑。

**在 static 变量区添加（加在 `static bool s_exit_locked = false;` 之后）：**

```c
/* WiFi 门禁 */
bool s_wifi_connected = false;
char s_sta_ip[16] = "0.0.0.0";
```

**在 `close_current_app()` 函数结尾（`ui_pop_state()` 之后、闭合大括号之前）添加提示叠层清理：**

不需要额外添加，`close_current_app` 通过 `lv_obj_del(s_app_page)` 会自然清除阻断叠层（阻断叠层是 s_app_page 的子对象）。

**在 `open_current_app()` 函数中，`s_is_open = true;` 之后添加门禁判断：**

找到 `s_active_app = &app_settings;` 之后的代码块，在创建 APP 内容后、隐藏卡片前插入门禁逻辑。修改后的 `open_current_app` 关键段：

找到：
```c
if (s_active_app && s_active_app->on_create) {
    s_active_app->on_create(s_app_page);
} else {
```

替换为：
```c
if (real_idx != 0 && !s_wifi_connected) {
    /* 非 Settings APP + WiFi 未连接 → 显示阻断提示 */
    lv_obj_t *ov = lv_obj_create(s_app_page);
    lv_obj_set_size(ov, 240, 240);
    lv_obj_set_style_bg_color(ov, lv_color_make(10, 10, 30), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ov, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ov, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *warn = lv_label_create(ov);
    lv_label_set_text(warn, "⚠\n请先配置 WiFi\n\n打开浏览器访问\nhttp://" WIFI_AP_IP "\n\n设置网络后使用此功能");
    lv_obj_set_style_text_color(warn, lv_color_make(200, 200, 200), LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_center(warn);
} else if (s_active_app && s_active_app->on_create) {
    s_active_app->on_create(s_app_page);
} else {
```

注意：文本中不能直接写 `WIFI_AP_IP` 宏在字符串中 — C 不会展开宏在字符串内。改为：

```c
lv_label_set_text(warn, "⚠\n请先配置 WiFi\n\n打开浏览器访问\nhttp://192.168.4.1\n\n设置网络后使用此功能");
```

- [ ] **Step 3: 修改 main.c 添加 WiFi 事件回调**

在 `main.c` 的 `wifi_ap_init()` 函数中注册事件回调。在 `esp_wifi_start()` 之后添加：

```c
/* 注册 STA 连接事件回调 */
esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
    &wifi_event_handler, NULL);
esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
    &wifi_event_handler, NULL);
esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
    &wifi_event_handler, NULL);
```

添加事件回调函数 `wifi_event_handler`（在 `joy_event_task` 之前）：

```c
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip);
        /* s_wifi_connected 由 sta_switch_timer_cb 设置 */
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "STA disconnected");
        /* 不做自动回退，标记断开 */
        s_wifi_connected = false;
    }
}
```

**需添加的头文件包含：** `#include "wifi_config.h"` 和 `#include "esp_event.h"`（已包含）

- [ ] **Step 4: Verify compilation**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -10
```

- [ ] **Step 5: Commit**

```bash
git add components/ui_system/ui_main.h components/ui_system/ui_main.c main/main.c && git commit -m "feat: add WiFi gating and event handler"
```

---

### Task 5: Settings WiFi Info 动态子项 + 左对齐 + 多行

**Files:**
- Modify: `components/app_settings/app_settings.c`

**Interfaces:**
- Consumes: `s_wifi_connected`, `s_sta_ip`, `WIFI_AP_SSID`, `WIFI_AP_PASS`
- Produces: 动态 WiFi Info 段（连接前3项，连接后1项）

- [ ] **Step 1: 修改头文件包含**

```c
#include "app_t.h"
#include "ui_main.h"
#include "wifi_config.h"
#include "esp_log.h"
```

- [ ] **Step 2: 修改 `create_sub_item()` — 左对齐 + 多行**

```c
static lv_obj_t *create_sub_item(lv_obj_t *parent, const char *text, int y_pos)
{
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_size(item, 210, SUB_H);
    lv_obj_set_style_radius(item, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(item, lv_color_make(25, 25, 45), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(item, 0, LV_STATE_DEFAULT);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_y(item, y_pos);
    lv_obj_set_x(item, 8);  // 统一左偏移 8px

    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, lv_font_default(), LV_STATE_DEFAULT);
    lv_obj_set_width(label, 200);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_set_style_opa(item, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
    return item;
}
```

- [ ] **Step 3: 调整子项数量上限和 WiFi Items 定义**

`section_t` 中的 `sub_items[4]` 改为 `sub_items[6]`（WiFi Info 最多 3 项，但预留）：

```c
lv_obj_t *sub_items[6]; // 子项最多6个
```

修改 WiFi 相关 items（使用常量，非静态字符串数组，因为要动态变内容）：

```c
static const char *WIFI_ITEMS_DISCONNECTED[] = {"Status:  AP Mode / 192.168.4.1",
                                                 "SSID:    ESP32-CAM",
                                                 "Password: 12345678"};
static const char *WIFI_ITEMS_CONNECTED[] = {"Status:  Connected"};
```

- [ ] **Step 4: 修改 `on_create()` 中 WiFi Info 段的子项创建逻辑**

在创建 WiFi Info 段时（`i == 1`），根据 `s_wifi_connected` 选用不同的 items 列表和数量：

```c
/* 创建子项 */
section_t *sec = &s_sections[i];
const char **items = SUB_ITEMS[i];
int count = SUB_COUNTS[i];

/* WiFi Info 段：动态根据连接状态选择显示内容 */
if (i == 1) {
    if (s_wifi_connected) {
        items = WIFI_ITEMS_CONNECTED;
        count = 1;
    } else {
        items = WIFI_ITEMS_DISCONNECTED;
        count = 3;
    }
}

sec->sub_count = count;
for (int j = 0; j < count; j++) {
    sec->sub_items[j] = create_sub_item(s_page, items[j], y);
    y += SUB_H + ITEM_GAP;
}
```

- [ ] **Step 5: 添加 `update_wifi_info()` 函数**

在 `start_collapse_arc()` 之前添加：

```c
/* 更新 WiFi Info 段显示（连接状态变化时调用） */
void update_wifi_info(void)
{
    section_t *sec = &s_sections[1];  // WiFi Info 固定 index=1
    if (!sec->header) return;

    int new_count;
    const char **new_items;

    if (s_wifi_connected) {
        new_count = 1;
        new_items = WIFI_ITEMS_CONNECTED;
        /* Status 行动态带 IP */
        static char status_buf[48];
        snprintf(status_buf, sizeof(status_buf), "Status:  Connected (IP: %s)", s_sta_ip);
        /* 注意：snprintf 到 static buf，需要确保 WIFI_ITEMS_CONNECTED[0] 被覆盖 */
        // 直接更新第一个子项的文字
        if (sec->sub_items[0]) {
            lv_obj_t *lbl = lv_obj_get_child(sec->sub_items[0], 0);
            if (lbl) lv_label_set_text(lbl, status_buf);
        }
    } else {
        new_count = 3;
    }

    /* 显隐子项 */
    for (int j = 0; j < sec->sub_count; j++) {
        if (sec->sub_items[j]) {
            if (j < new_count)
                lv_obj_clear_flag(sec->sub_items[j], LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(sec->sub_items[j], LV_OBJ_FLAG_HIDDEN);
        }
    }

    sec->sub_count = new_count;
}
```

- [ ] **Step 6: 在 ui_main.h 中声明 `update_wifi_info()`**

```c
/* 外部 APP 可调用的更新接口 */
void update_wifi_info(void);
```

- [ ] **Step 7: 连接成功后调用 `update_wifi_info()`**

在 `sta_switch_timer_cb()`（web_config_server.c）中 `s_wifi_connected = true;` 之后添加：

```c
extern void update_wifi_info(void);
update_wifi_info();
```

- [ ] **Step 8: Verify compilation**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py build 2>&1 | tail -10
```

- [ ] **Step 9: Commit**

```bash
git add components/app_settings/app_settings.c components/ui_system/ui_main.h && git commit -m "feat: WiFi Info dynamic items, left-align, multi-line wrap"
```

---

## 编译验证（最终）

- [ ] **完整编译**

```bash
cd /d/Project/ESP32/esp32-cam-app && idf.py fullclean && idf.py build 2>&1 | grep -E "error:|BUILD_EXIT|Successfully"
```

Expected: `Project build complete` or `BUILD_EXIT:0`

- [ ] **sdkconfig 变更审查**

```bash
cd /d/Project/ESP32/esp32-cam-app && git diff build/config/sdkconfig.h | head -30
```
Expected: 没有意外变更（WiFi 相关增加是正常的，PSRAM/LVGL/Compiler 不应变更）

- [ ] **最终提交**

```bash
git add -A && git commit -m "chore: final build verification pass"
```
