# ESP32-S3 对讲机固件

基于 ESP-IDF 的 LVGL 对讲机应用，源码在 `main/`：

| 文件 | 作用 |
|---|---|
| `main.c` | 入口：显示/触摸初始化、I2C 扫描、任务循环 |
| `app_ui.c/.h` | 240×320 竖屏 UI、横屏键盘、NODES/SETTINGS 页 |
| `net_link.c/.h` | ESP-NOW 帧协议、节点发现、多跳中继 |
| `voice.c/.h` | PTT 语音收发任务（IMA ADPCM） |
| `audio.c/.h` + `es8311.*` | I2S / ES8311 codec、蜂鸣 |
| `power.c/.h` | 背光 PWM、电池 ADC |
| `adpcm.c/.h` | ADPCM 编解码 |

## 构建

在 ESP-IDF 5.5.x 终端中：

```bash
cd firmware/esp32s3
idf.py build
idf.py -p COM8 flash
```

注意使用 **纯 ASCII 路径**；中文路径会导致 ESP-IDF 工具链报错。

依赖组件：
- `components/lvgl`：LVGL 8.4（已精简为 `src/` + 构建文件）
- `components/lvgl_esp32_drivers`：ILI9341/FT6x36 驱动
- `components/lvgl_porting`：显示/触摸移植层（含横竖屏切换）
