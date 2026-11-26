# 硬件说明

## 主控板（ESP32-S3 对讲机）

- 型号：**ES3C28P / ES3N28P** 2.8 寸 IPS 开发板
- MCU：ESP32-S3，16MB Flash，8MB Octal PSRAM
- 屏幕：ILI9341（SPI2），240×320
- 触摸：FT6x36（I2C，地址 0x38）
- 音频：ES8311 codec（I2C 地址 0x18）+ 功放
- 充电：TP4054（R12=3.3kΩ → 约 300mA；最大 500mA）
- 电池检测：ADC1_CH8（GPIO9），分压比 2.0

## 引脚

### LCD（SPI，ILI9341）

| 信号 | GPIO | 配置项 |
|---|---|---|
| SCLK | 12 | `CONFIG_LV_DISP_SPI_CLK` |
| MOSI | 11 | `CONFIG_LV_DISP_SPI_MOSI` |
| MISO | 13 | `CONFIG_LV_DISP_SPI_MISO` |
| CS | 10 | `CONFIG_LV_DISP_SPI_CS` |
| DC | 46 | `CONFIG_LV_DISP_PIN_DC` |
| RST | 18 | `CONFIG_LV_DISP_PIN_RST` |
| 背光 | 45 | `CONFIG_LV_DISP_PIN_BCKL`（LEDC PWM） |

### 触摸（I2C0，FT6x36 @0x38）

| 信号 | GPIO |
|---|---|
| SDA | 16（`CONFIG_I2C_MANAGER_0_SDA`） |
| SCL | 15（`CONFIG_I2C_MANAGER_0_SCL`） |
| INT | 未使用 |

### ES8311 音频

| 信号 | GPIO |
|---|---|
| I2C | 共用 I2C0（地址 0x18） |
| MCLK | 4 |
| BCLK | 5 |
| WS (LRCK) | 7 |
| DOUT（ESP→codec） | 8 |
| DIN（codec→ESP） | 6 |
| PA 使能 | 1（低有效） |

- 采样率 16kHz、16bit、立体声（codec 侧），MCLK = 384×fs
- 开机 200ms 1kHz 蜂鸣提示

### 背光与电池

| 功能 | GPIO | 说明 |
|---|---|---|
| 背光 | 45 | LEDC PWM，默认 70%，设置页可调 |
| 电池 ADC | 9 | 12dB 衰减，分压 2.0 |

## 电源

- 电池：3.7V 锂电（必须带保护板），4000mAh 实测可运行 20~30 小时
- 充电：TP4054，默认约 300mA（4000mAh 约 14~17 小时）
- 可用 5V/1A 以上充电器；注意 JP1 极性

## ESP8266 节点

- Wemos D1 mini（4MB Flash，FT232 串口）
- 仅 USB 供电即可；无需 OLED/外设
- 网页 AP：`ESP-NOW-CHAT` / `12345678`，IP `192.168.4.1`

## 三节点连接

```
ESP32-S3 (NODE-2EC0) ── ESP-NOW 信道6 ── ESP8266 (8266-1)
                                        └── ESP8266 (WEB-164636)
```
