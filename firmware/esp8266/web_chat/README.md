# ESP8266 网页聊天节点

Wemos D1 mini + `web_chat.ino`，作为 ESP-NOW 网络中的平级节点：

- SoftAP：`ESP-NOW-CHAT` / `12345678`，AP 地址 `192.168.4.1`
- 网页收发文字，显示发送者名字，可在页面 `SET NAME` 修改节点名（存 EEPROM）
- 每 3 秒广播 HELLO；与 ESP32-S3 使用相同的 21 字节帧 + CRC16 协议
- 支持 HELLO / TEXT 的多跳中继（与 ESP32 同参数：TTL=3、去重 30s、限速 20 帧/秒）

## 编译 / 烧录

```bash
arduino-cli compile --fqbn esp8266:esp8266:d1_mini firmware/esp8266/web_chat
arduino-cli upload -p COM14 --fqbn esp8266:esp8266:d1_mini firmware/esp8266/web_chat
```

或使用 `tools/flash_esp8266.bat COM14`。

要求：ESP8266 Arduino core 3.1.1（无外部库依赖）。
