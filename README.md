# ESP32-S3-CAM 智能垃圾桶终端

基于 ESP32-S3 的摄像头推流与垃圾分类显示终端，与 Android 端垃圾检测 App 配合使用。

## 硬件需求

| 组件 | 型号 | 数量 |
|------|------|------|
| 主控 | ESP32-S3-CAM (N16R8, 16MB Flash + 8MB PSRAM) | 1 |
| 摄像头 | OV5640 (板载) | 1 |
| 显示屏 | ST7789 1.54寸 240×240 TFT彩屏 | 1 |
| 摇杆 | PS2双轴摇杆 QYF-860 | 1 |

## 引脚接线

### OV5640 摄像头（板载，无需额外接线）

| 信号 | GPIO |
|------|------|
| XCLK | 15 |
| SIOD(SDA) | 4 |
| SIOC(SCL) | 5 |
| VSYNC | 6 |
| HREF | 7 |
| PCLK | 13 |
| D0-D7 | 11,9,8,10,12,18,17,16 |
| PWDN/RESET | -1 (板载已处理) |

### ST7789 显示屏（复用 SD 卡接口）

| 信号 | GPIO | 复用功能 |
|------|------|----------|
| SCLK | 39 | SD_CLK |
| MOSI | 40 | SD_DATA0 |
| DC | 41 | MTDI |
| CS | 38 | SD_CMD |
| RST | 42 | MTMS |
| BL | -1 | 3.3V 常亮 |

> **注意：显示屏复用了 SD 卡引脚，SD 卡和显示屏不能同时使用。如需使用 SD 卡，需修改引脚映射。**

### PS2 摇杆

| 信号 | GPIO |
|------|------|
| VRX (X轴) | 1 (ADC1_CH0) |
| VRY (Y轴) | 2 (ADC1_CH1) |
| SW (按键) | 21 (上拉输入，按下低电平) |
| VCC | 3.3V |
| GND | GND |

### 电源要求

ESP32-S3-CAM + OV5640 + ST7789 同时工作电流约 300-500mA，建议使用 5V/1A 以上电源供电。

## 开发环境

- **ESP-IDF**: v5.x (推荐 v5.1+)
- **工具链**: Xtensa ESP32-S3
- **LVGL**: v8.x (内置)

### Windows 编译

```bash
# 设置 ESP-IDF 环境
cd esp32-cam-app
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py flash -p COMx
```

## 首次使用

1. 烧录固件后，ESP32 进入 AP 模式
2. 搜索 WiFi `COLDFISHESP32`，密码 `12060606`
3. 连接到后打开浏览器访问 `http://192.168.4.1`
4. 配置 WiFi 连接到家庭路由器（STA 模式）
5. 重启后 ESP32 自动连接路由器
6. 启动 Android App，自动发现或手动输入 ESP32 IP 进行连接

## 工作模式

### 推流模式（Camera）
摄像头采集 MJPEG 视频流，通过 UDP 实时推送到 Android 端显示纯画面。

### 垃圾检测模式（Trash）
摄像头采集图像 → 推送到 Android → Android 本地 MNN 推理 → 结果通过 TCP 回传到 ESP32 → ST7789 屏幕显示分类结果。

### TFT 本地显示模式
摄像头以 QVGA(320×240) RGB565 格式直接在 ST7789 屏幕显示实时画面。

## 通信协议

- **TCP 控制**: 端口 8080，7 字节包头 + payload 的二进制协议
- **UDP 视频流**: 动态端口，MJPEG 分片传输（最大 1500 字节/包，含 12 字节帧头）
- **UDP 发现**: 端口 8081，广播 "ESP32?" → 响应 "ESP32:xxx.xxx.xxx.xxx"

## 配置调整

ESP32 Web 配置页面 (`http://192.168.4.1`) 可调整：

- WiFi 网络设置
- 摄像头分辨率 (QVGA/VGA/HD)
- JPEG 质量 (8-25)
- 镜像/翻转
- 亮度、对比度、白平衡、AE

## 注意事项

- OV5640 在 HD(1280×720) 分辨率下 JPEG quality 建议 8-15，过高会导致帧缓冲溢出
- 摇杆 ADC 采样使用 12 位精度，方向阈值：HIGH=3000, LOW=800, 中心=1700~2400
- 推流时不要同时启用 TFT 本地显示（两者共享摄像头资源）
- 首次使用必须先通过 Web 页面配置 WiFi，否则 Android 无法连接