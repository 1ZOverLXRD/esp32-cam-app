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
  │         Android 通过局域网发现 ESP32
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
  Status:  Connected
```
（SSID / Password 隐藏）

三个字符串值硬编码在代码中。

## APP 门禁

- `ui_main.c` 新增全局标志 `s_wifi_connected`，初始 false
- `open_current_app()` 中判断 `real_idx != 0 && !s_wifi_connected`：
  - true → 在 APP page 上创建全屏提示叠层（静态 label 布局，无交互对象）
  - false → 正常打开 APP
- 退出提示：沿用标准 LONG_PRESS 退出弧流程（`close_current_app` 清理提示叠层）
- WiFi 连接成功 → `wifi_event_handler` 回调设 `s_wifi_connected = true`
- STA 断开/超时 → 设回 false，下次进入主菜单时生效

## HTML 配置页（简化）

仅三个元素：
```
SSID:    [________________]       ← 支持中文
Password: [________________]
        [  Connect  ]
        状态行（连接中/成功/失败）
```

去除模式选择、保存按钮。

## 后端 API

保留 2 个端点，删除 `/api/wifi/save`：

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/wifi/status` | GET | `{"mode":"AP"|"STA", "ip":"...", "ssid":"...", "connected":bool}` |
| `/api/wifi/test` | POST | 输入 `{"ssid":"...","password":"..."}` → 测试连接 → 成功则原地切纯 STA |

**切换纯 STA 流程（`/api/wifi/test` 成功后）:**
1. 尝试 STA 连接并等待结果
2. 成功 → 发 HTTP 响应 `{"ok":true, "ip":"192.168.x.x"}`（先发完响应）
3. `httpd_stop(s_server)` — 关闭配置页，客机断开
4. `esp_wifi_set_mode(WIFI_MODE_STA)` — 切纯 STA
5. 设 `s_wifi_connected = true`
6. 不保存 NVS，掉电后恢复 AP 模式

> ⚠ 顺序重要：HTTP 响应必须在 `httpd_stop` 之前完成发送，否则客机收不到结果。

## 子项左对齐 + 多行

- `create_sub_item()` 中 `lv_obj_set_x(item, 8)`（统一距左 8px）
- `lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP)` 启用换行
- 子项宽度 210px，高度 32px/行（静态，不动态调高——数据短时无影响）

## 影响文件清单

| 文件 | 变更 |
|------|------|
| `main/main.c` | WiFi 初始化改为 AP 优先；STA 连接后通知 UI；删除 `web_config_server_start()` 前置条件 |
| `components/ui_system/ui_main.c` | 加 `s_wifi_connected` + 门禁逻辑 + 全屏提示创建/清除 |
| `components/app_settings/app_settings.c` | WiFi Info 子项改为 Status/SSID/PW；连接后仅显 Status；左对齐 + 多行 |
| `components/web_config_server/web_config_server.c` | 删 `/api/wifi/save`；`/api/wifi/test` 成功后切纯 STA + 关 HTTP Server |
| `components/web_config_server/web_config.h` | 简化 HTML，只剩 SSID/Password/Connect + 状态行 |
| `components/app_launcher/launcher.c` | SKIP（不涉及） |