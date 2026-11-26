# 多跳中继（Mesh）

三块板（1×ESP32-S3 + 2×ESP8266）均为**平级节点**，无中心、无主从。
每个节点都会转发 HELLO 和文字消息，用于扩展覆盖范围。

## 帧 flags 扩展

```
magic(2) ver(1) type(1) flags(1) seq(2) src(6) dst(6) len(2) payload(len) crc16(2)
```

| 位 | 含义 |
|---|---|
| bit0 | 语音 start（仅 MSG_VOICE） |
| bit1 | 语音 end（仅 MSG_VOICE） |
| bit2-5 | TTL（剩余可转发跳数 0~15） |
| bit6-7 | 保留 |

- HELLO / TEXT 发送时 `TTL = RELAY_MAX_HOPS (3)`
- VOICE / PTT 的 TTL = 0，**不参与中继**（语音保持单跳、低延迟）
- 转发时只改 flags（TTL-1），重算 CRC，src 保持原始发送者不变

## 中继算法

1. 校验 magic/ver/CRC/len 通过后，用 **(src MAC, seq)** 查去重表（32 项，30s 窗口）
   - 命中 → 丢弃（不显示、不转发）
2. `src == 本机` → 丢弃（防止自己转发的回环）
3. 业务处理：HELLO 更新节点表、TEXT 显示（带发送者名字）
4. 若类型为 HELLO/TEXT 且 TTL > 0：
   - 令牌桶限速 20 帧/秒
   - 入转发队列，随机延迟 5~25ms 后广播（降低碰撞）
   - 转发 TTL = TTL-1
5. TTL 归零后不再转发，传播自然终止

## 日志关键字（串口）

| 日志 | 含义 |
|---|---|
| `relay tx type=1 seq=N ttl=2` | 本机转发了一帧 |
| `rx relayed ... hop=N` | 收到经中继的帧 |
| `dedup drop ... seq=N` | 重复副本，已丢弃 |
| `[RELAY] ...` / `[DEDUP] ...` | ESP8266 端对应日志 |

## 验证结果（三节点）

- ESP32：`online peers (2): 8266-1 WEB-164636`，同时出现 `relay tx ttl=2` 和 `dedup drop`
- 8266：`[RELAY] type=1 seq=5 ttl=2` 后收到其他节点转发的同一 seq，打印 `[DEDUP]`
- 每个 HELLO 只被处理/显示一次，转发最多 3 跳，无无限循环

## 已知限制

- 只中继 HELLO 与 TEXT；语音 PTT 仍为单跳
- 中继来的 HELLO 在 NODES 页显示的 RSSI 是“到中继节点”的信号，不是端到端信号
- 旧固件节点仍能显示中继消息，但自身不转发、不去重
- 所有节点固定信道 6，广播通信，无加密
- 去重窗口 30s；文字消息频率下 seq 回绕风险可忽略

## 参数

| 参数 | 默认值 | 位置 |
|---|---|---|
| RELAY_MAX_HOPS | 3 | `net_link.c` / `web_chat.ino` |
| DEDUP_SIZE / WINDOW | 32 / 30s | 同上 |
| RELAY_RATE_PER_SEC | 20 | 同上 |
| RELAY_JITTER | 5~25ms | 同上 |
