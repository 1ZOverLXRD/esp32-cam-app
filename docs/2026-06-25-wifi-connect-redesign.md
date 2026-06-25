# WiFi 连接机制重设计 — Spec

## 概述

ESP32 上电默认 AP 模式供客机连接配置，客机通过 HTML 配置页输入目标 WiFi 凭据，ESP32 测试连接成功后一次性切换到纯 STA 模式重启失效。非 Settings APP 在未连接前被门禁阻挡。

## 状态机

```
上电 → AP 模式 (SSID=ESP32-CAM / 12345678)
  ├── 客机连上 AP → 配置页可用
  ├── 主菜单: 仅 Settings 可打开
  └── 其他 APP: 弹全屏提示 "请先配置 WiFi"

用户在配置页输入目标 WiFi SSID + 密码 → Connect
  ├── 成功 → 切纯 STA 模式（一次性，掉电恢复 AP）
  │         APP 门禁解锁
  │         Android 通过 mDNS 或已知 IP 发现 ESP32
  └── 失败 → 保留 AP，配置页显示错误
```

## WiFi Info（Settings 段）

连接前展开 3 个子项：
```
WiFi Info
  Status:  AP Mode / 192.168.4.1
  SSID:    ESP32-CAM
  Password: 12345678
```

连接成功后仅 1 个子项：
```
WiFi Info
  Status:  Connected (IP: 192.168.x.x)
```
（SSID / Password 隐藏）

**动态更新机制**：三个字符串值硬编码在 `wifi_config.h`（新文件），Settings 通过外部变量访问。连接成功后由 WiFi 事件回调触发 UI 刷新（若 Settings 当前打开，隐藏第2/3行并显示 STA IP；若未打开，下次 on_create 时根据 `s_wifi_connected` 决定显示行数）。刷新通过 `update_wifi_info()` 函数实现，在 `s_wifi_connected` 变化时由 `wifi_evt_task` 回调调用。

**IP 获取**：AP 模式下固定 `192.168.4.1`。纯 STA 模式下通过 DHCP 获得，`esp_netif_get_ip_info()` 读取。Status 行实时显示当前 IP。

## 后端 API

保留 2 个端点，删除 `/api/wifi/save`：

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/wifi/status` | GET | `{"mode":"AP"|"STA", "ip":"...", "ssid":"...", "connected":bool}` |
| `/api/wifi/test` | POST | 输入 `{"ssid":"...","password":"..."}` → 测试连接 → 成功则原地切纯 STA |

### 测试 + 切换流程

`/api/wifi/test` handler 内：

```
POST /api/wifi/test {"ssid":"家里WiFi","password":"***"}
  │
  ├─ 1. esp_wifi_set_mode(WIFI_MODE_APSTA)     ← 临时双模，保留AP供客机接收响应
  ├─ 2. esp_wifi_set_config(WIFI_IF_STA, &cfg)  ← 配置目标WiFi
  ├─ 3. esp_wifi_connect()                      ← 发起连接
  ├─ 4. 等待 WIFI_EVENT_STA_CONNECTED + IP_EVENT_STA_GOT_IP（超时10秒）
  │
  ├── 失败/超时 → esp_wifi_disconnect()
  │               esp_wifi_set_mode(WIFI_MODE_AP)  ← 回退纯AP
  │               httpd_resp_send 错误JSON
  │
  └── 成功 → httpd_resp_send {"ok":true,"ip":"192.168.x.x"}  ← 先发完响应
             创建单次 FreeRTOS 定时器（延迟 500ms）           ← 不能在handler内stop
             定时器回调:
               httpd_stop(s_server)               ← 关配置页
               esp_wifi_set_mode(WIFI_MODE_STA)    ← 切纯STA
               esp_wifi_set_config(WIFI_IF_STA, &cfg)
               esp_wifi_connect()
               设 s_wifi_connected = true
               通知 comms_server 刷新 IP 绑定     ← 可选，取决于comms架构
```

> ⚠ handler 内不能调 `httpd_stop` — 会导致请求处理线程死锁。使用 Deferred 定时器。

### IP 地址适配

纯 STA 后 IP 由 DHCP 分配，不硬编码。`/api/wifi/status` 中读取 `esp_netif_get_ip_info()` 获取当前 IP。WiFi Info Status 行同理。mDNS 建议后续添加，当前暂不实现（Android 可通过已知 Hostname/TCP 发现）。

## APP 门禁

- `ui_main.c` 新增全局标志 `s_wifi_connected`，初始 false
- `open_current_app()` 中判断 `real_idx != 0 && !s_wifi_connected`：
  - true → 在 APP page 上创建全屏提示叠层（纯 label，无交互对象）
  - false → 正常打开 APP
- 退出提示：沿用标准 LONG_PRESS 退出弧流程（`close_current_app` 清理提示叠层）
- WiFi 连接成功 → `wifi_event_handler` 通过 extern 变量或回调函数设 `s_wifi_connected = true`
- STA 断开/超时 → 设回 false，下次进入主菜单时生效
- 退出提示画面仍然走 LONG_PRESS 退出弧，PRESS 不退出

## HTML 配置页（简化）

仅三个元素：
```
SSID:    [________________]       ← 支持中文 UTF-8
Password: [________________]
        [  Connect  ]
        状态行（连接中/成功/失败）
```

去除模式选择、保存按钮。Connect 按钮调用 `/api/wifi/test`。成功后前端显示 "连接成功，正在切换..."，HTTP 连接断开。

## 子项左对齐 + 多行

- `create_sub_item()` 中 `lv_obj_set_x(item, 8)`（统一距左 8px）
- `lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP)` 启用换行
- 子项宽度 210px，高度 32px/行（静态，不动态调高——数据短时无影响）
- 所有段子项统一左对齐，不再有居中残留

## 新文件 / 新增符号

| 符号/文件 | 类型 | 说明 |
|-----------|------|------|
| `components/wifi_config/wifi_config.h` | 新文件 | `#define AP_SSID "ESP32-CAM"` / `AP_PASS "12345678"`，供 main.c 和 app_settings.c 共用 |
| `extern bool s_wifi_connected` | ui_main.c 全局 | WiFi 连接状态标志 |
| `extern char s_sta_ip[16]` | ui_main.c 全局 | 当前 STA IP 字符串，用于 WiFi Info 显示 |
| `void update_wifi_info(void)` | app_settings.c | 根据 `s_wifi_connected` 更新 WiFi Info 段子项显隐+内容 |
| `wifi_evt_handler()` | main.c | 连接成功/断开事件回调，设标志 + 调用 `update_wifi_info()` |

## 影响文件清单

| 文件 | 变更 |
|------|------|
| `main/main.c` | WiFi 初始化不变（AP 优先）；加 STA 事件回调；连接成功后设标志 + 通知 UI |
| `components/wifi_config/wifi_config.h` | **新建** — AP SSID/Password 常量 |
| `components/ui_system/ui_main.c` | 加 `s_wifi_connected` + `s_sta_ip` + 门禁逻辑 + 全屏提示创建/清除 |
| `components/app_settings/app_settings.c` | WiFi Info 子项动态显隐；左对齐 + 多行；暴露 `update_wifi_info()` |
| `components/web_config_server/web_config_server.c` | 删 `/api/wifi/save`；`/api/wifi/test` 测试逻辑改为 APSTA 双模 + Deferred 切换 |
| `components/web_config_server/web_config.h` | 简化 HTML，只剩 SSID/Password/Connect + 状态行 |
| `components/comms_server/comms_server.c` | 可选：纯 STA 后刷新 IP 绑定 |

## 未完成项（后续跟踪）

- mDNS 服务发现（暂不实现，Android 端可先通过 TCP 扫描或手动输入 IP 发现）
- STA 断开自动回退 AP（当前仅在 WiFi 事件回调中设标志，不自动重启 Wi-Fi 模式）
- 多 WiFi 配置记忆（不保存 NVS，一次性）