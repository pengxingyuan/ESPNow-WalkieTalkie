/*
 * web_chat.ino - ESP8266 (Wemos D1 mini) web chat node
 *
 * 与 ESP32-S3 对讲机板共用同一套 ESP-NOW 帧协议：
 *   magic(2) ver(1) type(1) flags(1) seq(2) src(6) dst(6) len(2) payload(len) crc16(2)
 *
 * 功能：
 *   - SoftAP "ESP-NOW-CHAT"（信道 6，和 ESP32 板同信道）
 *   - 手机连接后打开 http://192.168.4.1 输入文字，经 ESP-NOW 广播给所有节点
 *   - 收到 ESP32 / 其他节点的文本会显示在网页上
 *   - 每 3 秒广播 HELLO，ESP32 板会显示 FOUND WEB-XXXX
 *
 * 编译：arduino-cli compile --fqbn esp8266:esp8266:d1_mini firmware/web_chat
 * 烧录：arduino-cli upload -p COMx --fqbn esp8266:esp8266:d1_mini firmware/web_chat
 */
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <espnow.h>
#include <user_interface.h>

/* ---------------- config ---------------- */
#define AP_SSID        "ESP-NOW-CHAT"
#define AP_PASS        "12345678"
#define ESPNOW_CHANNEL 6

#define FRAME_MAGIC    0xE5A5
#define FRAME_VER      1
#define MSG_HELLO      1
#define MSG_TEXT       2

#define NAME_LEN       24
#define MSG_TEXT_LEN   96
#define MAX_MSG        40
#define MAX_PEERS      8

/* relay (multi-hop mesh) */
#define FLAG_TTL_SHIFT      2
#define FLAG_TTL_MASK       0x3C
#define RELAY_MAX_HOPS      3
#define RELAY_QUEUE_LEN     8
#define RELAY_RATE_PER_SEC  20
#define RELAY_JITTER_MIN_MS 5
#define RELAY_JITTER_MAX_MS 25
#define DEDUP_SIZE          32
#define DEDUP_WINDOW_MS     30000

/* ---------------- frame ---------------- */
struct __attribute__((packed)) FrameHdr {
    uint16_t magic;
    uint8_t ver;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t len;
};

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---------------- state ---------------- */
struct ChatMsg {
    uint32_t seq;
    bool out;
    char from[NAME_LEN];
    char text[MSG_TEXT_LEN + 1];
};

static ChatMsg msgs[MAX_MSG];
static uint8_t msgCount = 0;
static uint32_t msgSeq = 0;

struct Peer {
    uint8_t mac[6];
    char name[NAME_LEN];
    uint32_t lastSeen;
};

static Peer peers[MAX_PEERS];
static uint8_t peerCount = 0;

struct DedupEntry {
    uint8_t mac[6];
    uint16_t seq;
    uint32_t seen;
    bool used;
};

static DedupEntry dedup[DEDUP_SIZE];
static uint8_t dedupNext = 0;

struct RelayItem {
    uint8_t buf[sizeof(FrameHdr) + 202];
    uint16_t len;
    uint32_t due;
};

static RelayItem relayQ[RELAY_QUEUE_LEN];
static uint8_t relayNum = 0;
static uint32_t relayWin = 0;
static uint8_t relayWinCount = 0;

static char nodeName[NAME_LEN];
static uint16_t txSeq = 0;
static uint32_t rxCount = 0;
static uint32_t txCount = 0;
static uint32_t lastHello = 0;

static ESP8266WebServer server(80);

/* ---------------- protocol helpers ---------------- */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool dedupSeen(const uint8_t *mac, uint16_t seq)
{
    uint32_t now = millis();
    for (uint8_t i = 0; i < DEDUP_SIZE; i++) {
        if (!dedup[i].used || dedup[i].seq != seq) continue;
        if (memcmp(dedup[i].mac, mac, 6) == 0 && (now - dedup[i].seen) < DEDUP_WINDOW_MS) {
            return true;
        }
    }
    DedupEntry *e = &dedup[dedupNext];
    dedupNext = (dedupNext + 1) % DEDUP_SIZE;
    memcpy(e->mac, mac, 6);
    e->seq = seq;
    e->seen = now;
    e->used = true;
    return false;
}

static void relayMaybe(const uint8_t *data, uint16_t len, uint8_t ttl)
{
    if (ttl == 0 || len < sizeof(FrameHdr) + 2) return;

    uint32_t now = millis();
    if (now - relayWin >= 1000) {
        relayWin = now;
        relayWinCount = 0;
    }
    if (relayWinCount >= RELAY_RATE_PER_SEC) {
        Serial.println("[RELAY] rate limit");
        return;
    }
    if (relayNum >= RELAY_QUEUE_LEN) {
        Serial.println("[RELAY] queue full");
        return;
    }

    RelayItem *r = &relayQ[relayNum++];
    memcpy(r->buf, data, len);
    r->len = len;

    FrameHdr *h = (FrameHdr *)r->buf;
    h->flags = (uint8_t)((h->flags & ~FLAG_TTL_MASK) | ((ttl - 1) << FLAG_TTL_SHIFT));

    uint16_t crc = crc16_ccitt(r->buf, len - 2);
    r->buf[len - 2] = (uint8_t)(crc & 0xFF);
    r->buf[len - 1] = (uint8_t)(crc >> 8);

    uint32_t jitter = RELAY_JITTER_MIN_MS + (random(RELAY_JITTER_MAX_MS - RELAY_JITTER_MIN_MS + 1));
    r->due = now + jitter;

    Serial.printf("[RELAY] type=%u seq=%u ttl=%u\n", h->type, h->seq, (unsigned)(ttl - 1));
}

static void relayProcess(void)
{
    uint32_t now = millis();
    for (uint8_t i = 0; i < relayNum; ) {
        if (now < relayQ[i].due) {
            i++;
            continue;
        }
        esp_now_send((u8 *)BCAST, relayQ[i].buf, relayQ[i].len);
        relayWinCount++;
        relayQ[i] = relayQ[--relayNum];
    }
}

static void pushMessage(bool out, const char *text, const char *from)
{
    uint8_t idx;
    if (msgCount < MAX_MSG) {
        idx = msgCount;
        msgCount++;
    } else {
        /* ring: shift out the oldest */
        memmove(&msgs[0], &msgs[1], sizeof(ChatMsg) * (MAX_MSG - 1));
        idx = MAX_MSG - 1;
    }
    msgs[idx].seq = ++msgSeq;
    msgs[idx].out = out;
    strncpy(msgs[idx].from, (from != NULL) ? from : "", NAME_LEN - 1);
    msgs[idx].from[NAME_LEN - 1] = '\0';
    strncpy(msgs[idx].text, text, MSG_TEXT_LEN);
    msgs[idx].text[MSG_TEXT_LEN] = '\0';
}

static void updatePeer(const uint8_t *mac, const char *name)
{
    for (uint8_t i = 0; i < peerCount; i++) {
        if (memcmp(peers[i].mac, mac, 6) == 0) {
            strncpy(peers[i].name, name, NAME_LEN - 1);
            peers[i].name[NAME_LEN - 1] = '\0';
            peers[i].lastSeen = millis();
            return;
        }
    }
    if (peerCount >= MAX_PEERS) return;
    memcpy(peers[peerCount].mac, mac, 6);
    strncpy(peers[peerCount].name, name, NAME_LEN - 1);
    peers[peerCount].name[NAME_LEN - 1] = '\0';
    peers[peerCount].lastSeen = millis();
    peerCount++;
}

static const char *peerNameByMac(const uint8_t *mac)
{
    for (uint8_t i = 0; i < peerCount; i++) {
        if (memcmp(peers[i].mac, mac, 6) == 0 && peers[i].name[0] != '\0') {
            return peers[i].name;
        }
    }
    return "?";
}

static void sendFrame(uint8_t type, const void *payload, uint16_t len)
{
    if (len > 200) return;

    uint8_t buf[sizeof(FrameHdr) + 200 + 2];
    FrameHdr *hdr = (FrameHdr *)buf;

    uint8_t selfMac[6];
    WiFi.macAddress(selfMac);

    hdr->magic = FRAME_MAGIC;
    hdr->ver = FRAME_VER;
    hdr->type = type;
    hdr->flags = 0;
    if (type == MSG_HELLO || type == MSG_TEXT) {
        hdr->flags = (uint8_t)(RELAY_MAX_HOPS << FLAG_TTL_SHIFT);
    }
    hdr->seq = ++txSeq;
    memcpy(hdr->src, selfMac, 6);
    memcpy(hdr->dst, BCAST, 6);
    hdr->len = len;
    if (len > 0) memcpy(buf + sizeof(FrameHdr), payload, len);

    size_t total = sizeof(FrameHdr) + len;
    uint16_t crc = crc16_ccitt(buf, total);
    buf[total++] = (uint8_t)(crc & 0xFF);
    buf[total++] = (uint8_t)(crc >> 8);

    int ret = esp_now_send((u8 *)BCAST, buf, total);
    if (ret == 0) {
        txCount++;
    } else {
        Serial.printf("[TXERR] ret=%d type=%d len=%d\n", ret, (int)type, (int)total);
    }
}

static void sendHello()
{
    int before = txCount;
    sendFrame(MSG_HELLO, nodeName, strlen(nodeName) + 1);
    if (txCount != before) {
        Serial.printf("[TX] HELLO ok (total tx=%lu)\n", (unsigned long)txCount);
    } else {
        Serial.println("[TX] HELLO failed");
    }
}

static void sendText(const char *text)
{
    if (text == NULL || text[0] == '\0') return;
    size_t n = strlen(text);
    if (n > MSG_TEXT_LEN) n = MSG_TEXT_LEN;
    sendFrame(MSG_TEXT, text, (uint16_t)n + 1);
    pushMessage(true, text, nodeName);
}

/* ---------------- ESP-NOW receive ---------------- */
static void onSent(u8 *mac, u8 status)
{
    (void)mac;
    if (status != 0) {
        Serial.printf("[TXCB] fail status=%d\n", (int)status);
    }
}

static void onRecv(u8 *mac, u8 *data, u8 len)
{
    Serial.printf("[RX] len=%d\n", (int)len);
    (void)mac;
    if (len < sizeof(FrameHdr) + 2) return;
    FrameHdr *hdr = (FrameHdr *)data;
    if (hdr->magic != FRAME_MAGIC || hdr->ver != FRAME_VER) return;

    int payloadLen = (int)len - (int)sizeof(FrameHdr) - 2;
    if (payloadLen < 0 || (uint16_t)payloadLen != hdr->len) return;

    uint16_t crcRx = (uint16_t)(data[len - 2] | (data[len - 1] << 8));
    if (crc16_ccitt(data, len - 2) != crcRx) return;

    uint8_t selfMac[6];
    WiFi.macAddress(selfMac);
    if (memcmp(hdr->src, selfMac, 6) == 0) return; /* our own frame */

    uint8_t ttl = (uint8_t)((hdr->flags & FLAG_TTL_MASK) >> FLAG_TTL_SHIFT);

    if (dedupSeen(hdr->src, hdr->seq)) {
        Serial.printf("[DEDUP] type=%u seq=%u\n", hdr->type, hdr->seq);
        return; /* already displayed / forwarded */
    }

    rxCount++;

    char text[MSG_TEXT_LEN + 1];
    int n = payloadLen;
    if (n > MSG_TEXT_LEN) n = MSG_TEXT_LEN;
    memcpy(text, data + sizeof(FrameHdr), n);
    text[n] = '\0';

    if (hdr->type == MSG_HELLO) {
        updatePeer(hdr->src, text);
    } else if (hdr->type == MSG_TEXT && text[0] != '\0') {
        updatePeer(hdr->src, "?");
        pushMessage(false, text, peerNameByMac(hdr->src));
    }

    if ((hdr->type == MSG_HELLO || hdr->type == MSG_TEXT) && ttl > 0) {
        relayMaybe(data, len, ttl);
    }
}

/* ---------------- web page ---------------- */
static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP-NOW Web Chat</title>
<style>
:root{--bg:#000;--bar:#080808;--line:#242424;--fg:#f2f2f2;--mut:#8a8a8a;--grn:#39ff14;}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 -apple-system,"Segoe UI",Roboto,"Noto Sans SC",sans-serif;}
header{position:sticky;top:0;background:var(--bar);border-bottom:1px solid var(--line);padding:10px 12px;display:flex;justify-content:space-between;align-items:center}
header b{font-size:15px;letter-spacing:.5px}
#st{color:var(--grn);font-size:12px}
#log{padding:12px 10px 90px;display:flex;flex-direction:column;gap:8px}
.m{max-width:82%;padding:8px 10px;border-radius:8px;word-break:break-word;white-space:pre-wrap}
.in{align-self:flex-start;background:#141414;border:1px solid #2e2e2e}
.from{display:block;font-size:11px;color:var(--grn);margin-bottom:2px;opacity:.85}
.out{align-self:flex-end;background:var(--grn);color:#000}
.sys{align-self:center;color:var(--mut);font-size:12px}
footer{position:fixed;left:0;right:0;bottom:0;background:var(--bar);border-top:1px solid var(--line);padding:8px 10px;display:flex;gap:8px;flex-wrap:wrap}
footer input{flex:1;min-width:120px;background:#0d0d0d;border:1px solid var(--line);color:var(--fg);padding:10px;border-radius:6px;font-size:15px}
footer button{background:#0d0d0d;border:1px solid var(--grn);color:var(--grn);border-radius:6px;padding:8px 12px;font-size:13px}
footer button.prim{background:var(--grn);color:#000;border-color:var(--grn)}
#hint{padding:4px 12px 0;color:var(--mut);font-size:11px}
#namebox{display:flex;gap:6px;padding:6px 10px}
#namebox input{flex:1;background:#0d0d0d;border:1px solid var(--line);color:var(--fg);padding:8px;border-radius:6px;font-size:13px}
#namebox button{background:#0d0d0d;border:1px solid var(--grn);color:var(--grn);border-radius:6px;padding:6px 10px;font-size:12px}
</style>
</head>
<body>
<header><b id="nm">ESP-NOW WEB CHAT</b><span id="st">...</span></header>
<div id="namebox"><input id="nmInp" placeholder="node name"><button onclick="setName()">SET NAME</button></div>
<div id="hint">AP: ESP-NOW-CHAT / 12345678 · 本页与 ESP32 对讲机互通</div>
<div id="log"></div>
<footer>
  <input id="inp" placeholder="输入文字…" onkeydown="if(event.key==='Enter')send()">
  <button class="prim" onclick="send()">SEND</button>
  <button onclick="quick('OK')">OK</button>
  <button onclick="quick('HI')">HI</button>
  <button onclick="quick('COMING')">COMING</button>
</footer>
<script>
let last=0, first=true;
const log=document.getElementById('log');
function add(from, text, cls){
  const d=document.createElement('div');
  d.className='m '+cls;
  if(cls==='in' && from && from!=='?'){
    const f=document.createElement('span'); f.className='from'; f.textContent=from; d.appendChild(f);
  }
  d.appendChild(document.createTextNode(text));
  log.appendChild(d);
  window.scrollTo(0, document.body.scrollHeight);
}
async function poll(){
  try{
    const r=await fetch('/api/messages?since='+last);
    const j=await r.json();
    document.getElementById('st').textContent=j.status;
    for(const m of j.msgs){ add(m[2], m[3], m[1]==='out'?'out':'in'); }
    document.getElementById('nm').textContent = j.name;
    const ni=document.getElementById('nmInp');
    if(document.activeElement!==ni) ni.value = j.name;
    last=j.next;
  }catch(e){ document.getElementById('st').textContent='offline'; }
  setTimeout(poll, 800);
}
async function send(){
  const t=document.getElementById('inp').value.trim();
  if(!t) return;
  document.getElementById('inp').value='';
  try{ await fetch('/api/send',{method:'POST',headers:{'Content-Type':'text/plain'},body:t}); }catch(e){}
}
function quick(t){ document.getElementById('inp').value=t; send(); }
async function setName(){
  const n=document.getElementById('nmInp').value.trim();
  if(!n) return;
  try{ await fetch('/api/name',{method:'POST',headers:{'Content-Type':'text/plain'},body:n}); }catch(e){}
}
poll();
</script>
</body>
</html>)HTML";

static String jsonEscape(const String &s)
{
    String o;
    for (const char *p = s.c_str(); *p; p++) {
        if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
        else if ((uint8_t)*p < 0x20) { o += ' '; }
        else o += *p;
    }
    return o;
}

static String statusText()
{
    String s = nodeName;
    s += " | peers ";
    s += peerCount;
    s += " | rx ";
    s += rxCount;
    s += " tx ";
    s += txCount;
    return s;
}

static void handleRoot()
{
    server.send_P(200, "text/html", PAGE);
}

static void handleStatus()
{
    String j = "{\"name\":\"" + jsonEscape(nodeName) + "\",\"peers\":[";
    uint32_t now = millis();
    bool firstPeer = true;
    for (uint8_t i = 0; i < peerCount; i++) {
        if (now - peers[i].lastSeen > 10000) continue;
        if (!firstPeer) j += ",";
        j += "\"" + jsonEscape(peers[i].name) + "\"";
        firstPeer = false;
    }
    j += "],\"rx\":" + String(rxCount) + ",\"tx\":" + String(txCount) + "}";
    server.send(200, "application/json", j);
}

static void handleMessages()
{
    uint32_t since = 0;
    if (server.hasArg("since")) since = (uint32_t)server.arg("since").toInt();

    String j = "{\"next\":" + String(msgSeq) + ",\"name\":\"" + jsonEscape(nodeName) + "\",\"status\":\"" + jsonEscape(statusText()) + "\",\"msgs\":[";
    bool firstMsg = true;
    for (uint8_t i = 0; i < msgCount; i++) {
        if (msgs[i].seq <= since) continue;
        if (!firstMsg) j += ",";
        j += "[" + String(msgs[i].seq) + ",\"" + (msgs[i].out ? "out" : "in") + "\",\"" + jsonEscape(msgs[i].from) + "\",\"" + jsonEscape(msgs[i].text) + "\"]";
        firstMsg = false;
    }
    j += "]}";
    server.send(200, "application/json", j);
}

static void handleSend()
{
    String text;
    if (server.hasArg("plain")) text = server.arg("plain");
    else if (server.hasArg("text")) text = server.arg("text");
    text.trim();
    if (text.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    if (text.length() > MSG_TEXT_LEN) text = text.substring(0, MSG_TEXT_LEN);
    sendText(text.c_str());
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleName()
{
    if (server.method() == HTTP_POST) {
        String nm = server.arg("plain");
        nm.trim();
        if (nm.length() == 0) {
            server.send(400, "application/json", "{\"ok\":false}");
            return;
        }
        if (nm.length() > NAME_LEN - 1) {
            nm = nm.substring(0, NAME_LEN - 1);
        }
        nm.toCharArray(nodeName, sizeof(nodeName));
        EEPROM.write(0, 'N');
        EEPROM.write(1, 'M');
        EEPROM.write(2, '1');
        for (uint8_t i = 0; i < NAME_LEN; i++) {
            EEPROM.write(4 + i, (i < (uint8_t)nm.length()) ? (uint8_t)nm[i] : 0);
        }
        EEPROM.commit();
        sendHello();
        Serial.printf("[NAME] -> %s\n", nodeName);
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        server.send(200, "application/json",
                    "{\"name\":\"" + jsonEscape(nodeName) + "\"}");
    }
}

/* ---------------- setup / loop ---------------- */
void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println("ESP-NOW web chat booting...");
    randomSeed(micros());

    snprintf(nodeName, sizeof(nodeName), "WEB-%04X", ESP.getChipId());
    EEPROM.begin(64);
    if (EEPROM.read(0) == 'N' && EEPROM.read(1) == 'M' && EEPROM.read(2) == '1') {
        char tmp[NAME_LEN];
        uint8_t i = 0;
        for (; i < NAME_LEN - 1; i++) {
            char c = (char)EEPROM.read(4 + i);
            if (c == 0 || c == 0xFF) {
                break;
            }
            tmp[i] = c;
        }
        tmp[i] = '\0';
        if (i > 0) {
            strncpy(nodeName, tmp, sizeof(nodeName) - 1);
            nodeName[sizeof(nodeName) - 1] = '\0';
        }
    }
    Serial.print("node name: ");
    Serial.println(nodeName);

    /* AP on the same channel as the ESP32 walkie-talkies */
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS, ESPNOW_CHANNEL);
    if (!wifi_set_channel(ESPNOW_CHANNEL)) {
        Serial.println("[WiFi] wifi_set_channel failed");
    }
    Serial.printf("[WiFi] channel now=%d\n", wifi_get_channel());
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    Serial.printf("AP channel=%d\n", WiFi.channel());
    Serial.print("AP: ");
    Serial.print(AP_SSID);
    Serial.print("  IP: ");
    Serial.println(WiFi.softAPIP());

    /* ESP-NOW */
    int e1 = esp_now_init();
    Serial.printf("esp_now_init=%d\n", e1);
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(onRecv);
    esp_now_register_send_cb(onSent);
    int e2 = esp_now_add_peer((u8 *)BCAST, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
    Serial.printf("add_peer=%d channel=%d\n", e2, WiFi.channel());

    /* web server */
    server.on("/", handleRoot);
    server.on("/api/status", handleStatus);
    server.on("/api/messages", handleMessages);
    server.on("/api/send", HTTP_POST, handleSend);
    server.on("/api/send", HTTP_GET, handleSend);
    server.on("/api/name", handleName);
    server.begin();
    Serial.println("web server started: http://192.168.4.1");

    pushMessage(false, "SYSTEM: web chat ready", "SYSTEM");
    sendHello();
    lastHello = millis();
}

void loop()
{
    server.handleClient();
    relayProcess();

    uint32_t now = millis();
    if (now - lastHello > 3000) {
        lastHello = now;
        sendHello();
    }

    static uint32_t lastStat = 0;
    if (now - lastStat > 5000) {
        lastStat = now;
        Serial.printf("[STAT] ch=%d rx=%lu tx=%lu peers=%d\n",
                      WiFi.channel(), (unsigned long)rxCount,
                      (unsigned long)txCount, (int)peerCount);
    }

    /* drop peers that stopped sending heartbeats */
    for (uint8_t i = 0; i < peerCount; ) {
        if (now - peers[i].lastSeen > 10000) {
            memmove(&peers[i], &peers[i + 1], sizeof(Peer) * (peerCount - i - 1));
            peerCount--;
        } else {
            i++;
        }
    }
}
