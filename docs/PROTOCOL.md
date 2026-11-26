# 通信协议

ESP32-S3 与 ESP8266 使用完全相同的 ESP-NOW 帧格式，Wi-Fi 信道固定 **6**，全部为广播
（`FF:FF:FF:FF:FF:FF`），不加密。

## 帧格式

```
offset  size  field
0       2     magic = 0xE5A5 (little-endian)
2       1     ver = 1
3       1     type (1=HELLO, 2=TEXT, 3=PTT, 4=VOICE)
4       1     flags
5       2     seq (little-endian, 每帧递增)
7       6     src MAC
13      6     dst MAC (广播)
19      2     payload len (little-endian)
21      N     payload
21+N    2     CRC16-CCITT (poly 0x1021, init 0xFFFF, little-endian)
```

单帧 payload 最大 200 字节。

## flags 位定义

| 位 | 含义 |
|---|---|
| bit0 | VOICE start（仅 type=4） |
| bit1 | VOICE end（仅 type=4） |
| bit2-5 | TTL：剩余可中继跳数（0~15） |
| bit6-7 | 保留，发送置 0，接收忽略 |

- HELLO / TEXT 发送时 `TTL = 3`
- VOICE / PTT 发送时 `TTL = 0`（不参与中继）
- 中继转发时只把 TTL 减 1，并重算 CRC；`src` 保持原发送者，`seq` 不变

## 消息类型

### 1. HELLO
- payload：节点名（UTF-8 + NUL），如 `NODE-2EC0` / `8266-1`
- 每 **3 秒**广播一次
- 收到后更新节点表（MAC、名字、RSSI）；**10 秒**无 HELLO 视为离线
- 改名后立即发一次 HELLO

### 2. TEXT
- payload：UTF-8 文本 + NUL（≤200B）
- 收到后显示：接收方按 `src MAC` 查节点名，气泡显示 `名字: 内容`
- 参与多跳中继（见 MESH.md）

### 3. PTT
- payload：简短事件文本（调试用），不参与中继

### 4. VOICE
- payload：`seq(2 LE) + flags(1) + ADPCM 数据`
- flags bit0=START、bit1=END
- IMA ADPCM 4-bit、单声道、16kHz、每帧 320 samples（20ms）→ 160B；加 3B 头共 163B
- 仅 ESP32-S3 之间使用；ESP8266 无音频硬件，忽略该类型
- 当前 **不中继**（单跳）

## 去重

- 去重键：`(src MAC, seq)`
- 窗口 **30 秒**，容量 32 项（环形覆盖）
- 命中后丢弃：不显示、不转发
- 语音不参与去重

## 中继

- 只中继 HELLO / TEXT
- 收到 TTL>0 的帧 → 限速 20 帧/秒、随机 5~25ms 抖动后广播 TTL-1
- 细节见 [MESH.md](MESH.md)

## 节点名

- ESP32-S3：`NODE-xxxx`（MAC 后 2 字节），存 NVS，可在 NODES 页编辑
- ESP8266：`WEB-xxxx`（chip id），存 EEPROM，可在网页 `SET NAME` 编辑
- 最长 23 字符

## 兼容性

- 旧固件（flags=0、无 TTL）仍能接收并显示中继帧（忽略高位），但自身不转发、不去重
- 建议所有节点使用 v1.0.0 及以上固件
