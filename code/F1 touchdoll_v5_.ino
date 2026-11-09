// TouchDoll 成品固件 v5（手感翻译下沉到板子） —— 不含任何 WiFi 名、密码、IP
// 上电流程：先从 NVS 读上次配好的 WiFi；连不上就自己开热点等人来配。
// 配好以后热点自动关掉，板子接入用户自家网络。

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>

// ================== 出厂常量，客户信息一概不写在这 ==================
const char* AP_PREFIX = "TouchDoll-";     // 热点名前缀，后面自动跟 MAC 后四位
const char* AP_PASS   = "touchdoll";      // 配网热点的固定密码，印在说明书上
// ==================================================================

Preferences prefs;
WebServer   srv(80);
DNSServer   dns;
WiFiUDP     udp;

bool   configMode = false;
bool   routesUp   = false;      // 路由只注册一次
bool   tryNow     = false;      // 配网页刚存完，等 loop 里去连
String pendSsid, pendPass;
String apSsid;

const int   PINS[4]     = {4, 5, 6, 7};
const char* KEYS[4]     = {"top", "cheek", "chest", "back"};
const int   GNDPOSTS[4] = {15, 18, 17, 16};

// 触发线：都不是绝对值，一律按"相对基线抬了多少"算。
// 原版用绝对阈值 20，可静态偏置就有 2500，于是每一帧每一路都超线，
// 串口被 EV 刷爆、loop 卡死、Web 服务抢不到时间，配网页当场打不开。
const int    TH_ON   = 40;   // 抬升超过这个才可能算摸到（约 1% 满量程）
const int    TH_OFF  = 20;   // 落回这个以下才算松手（滞回，防止边沿抖）
const int    CONFIRM = 3;    // 连续几次采样都超线才认，滤掉尖峰噪声
const int    EVMAX = 256;
struct Ev { unsigned long id; unsigned long t; uint8_t pin; uint16_t v; };
Ev buf[EVMAX];
int  head = 0, tail = 0;
unsigned long seq = 0;

unsigned long reqCount = 0;
unsigned long lastBC = 0;
unsigned long lastHB = 0;
unsigned long offlineSince = 0;

// 射频一动（开热点、切 AP_STA、连路由器扫描）ADC 会整体跳，跳完再回落。
// 所以每次切换完不许马上判触摸：先喂一段基线，喂完才开门。
unsigned long settleUntil = 0;
const unsigned long SETTLE_MS = 300;
bool wasSettling = false;   // 静默期收尾用：开门那刻已经按着的，算静默的账

// 同一路刚报过「按下」，250ms 内的抖动不再重复报，免得连成一串。
unsigned long lastEvAt[4] = {0, 0, 0, 0};
const unsigned long DEBOUNCE_MS = 250;

// ---------------------------------------------------------------- 采样

void pushEv(uint8_t pin, uint16_t v) {
  seq++;
  Serial.print("EV ");
  Serial.print(seq);
  Serial.print("  ");
  Serial.print(KEYS[pin]);
  Serial.print("  ");
  Serial.println(v);
  buf[head] = {seq, millis(), pin, v};
  head = (head + 1) % EVMAX;
  if (head == tail) tail = (tail + 1) % EVMAX;
}
int evCount() {
  return (head >= tail) ? (head - tail) : (EVMAX - tail + head);
}


// ---------------------------------------------------------------- 手感翻译
// 把触摸事件翻成人话，全部在板子上做。客户端只负责取走。

const unsigned long TAP_MS   = 400;
const unsigned long SHORT_MS = 800;
const float LIGHT_MAX = 0.035f;
const unsigned long SYNC_MS = 500;
const int TXMAX = 24;

const char* CMN[4] = {"头顶", "脸颊", "前胸", "后背"};

struct DualText { uint8_t mask; const char* txt; };
const DualText DUALS[] = {
  { 0x1 | 0x2,   "捏住了你的头" },
  { 0x4 | 0x8,   "抱住了你" },
  { 0x1 | 0x4,   "把你的头按进怀里" },
  { 0x1 | 0x8,   "从背后圈住你" },
  { 0x2 | 0x4,   "抱着你捧你脸" },
  { 0x2 | 0x8,   "从背后捏住你的脸" },
};
const int DUALN = sizeof(DUALS) / sizeof(DUALS[0]);

struct TxLine { unsigned long id; char txt[80]; };
TxLine txbuf[TXMAX];
int txh = 0, txt = 0;
unsigned long txSeq = 0;
unsigned long lastRead = 0;

struct St { bool touch; unsigned long pressStart; float peak; int cnt; };
St st[4] = {{false,0,0,0},{false,0,0,0},{false,0,0,0},{false,0,0,0}};
float base[4];
bool baseSet[4] = {false, false, false, false};
bool eaten[4]  = {false, false, false, false};

void pushLine(const String& s) {
  txSeq++;
  txbuf[txh].id = txSeq;
  strncpy(txbuf[txh].txt, s.c_str(), 79);
  txbuf[txh].txt[79] = 0;
  txh = (txh + 1) % TXMAX;
  if (txh == txt) txt = (txt + 1) % TXMAX;
  Serial.print("TX ");
  Serial.print(txSeq);
  Serial.print("  ");
  Serial.println(s);
}

// 每次射频状态变化后都要调一次：丢掉手里的旧状态、重新学基线、静默一小会儿。
// 不这么做的话，AP→AP_STA 那一下 ADC 整体抬升，四路会一起「按下」，
// 接着再一起「松手」，串口和客户端就看出一连串假数值。
void resetTouch(bool wipeEv) {
  for (int i = 0; i < 4; i++) {
    baseSet[i] = false;       // 让下面重新学一次基线
    st[i].touch = false;
    st[i].cnt = 0;
    st[i].pressStart = 0;
    st[i].peak = 0;
    eaten[i] = true;          // 不欠报
    lastEvAt[i] = 0;
  }
  if (wipeEv) { head = tail = 0; }   // 启动前攒下的假事件一并清掉
  settleUntil = millis() + SETTLE_MS;
  Serial.print("重置基线，静默 ");
  Serial.print(SETTLE_MS);
  Serial.println("ms");
}

String sec1(unsigned long ms) {
  char b[12];
  snprintf(b, sizeof(b), "%.1f", ms / 1000.0);
  return String(b);
}

String classify(int i, unsigned long dur, float peak) {
  String p = CMN[i];
  if (dur <= TAP_MS)  return "戳了戳你的" + p;
  if (dur <= SHORT_MS) {
    return (peak <= LIGHT_MAX) ? ("摸了摸你的" + p) : ("打了你一下的" + p);
  }
  return (peak <= LIGHT_MAX) ? ("揉了揉你的" + p)
                             : ("按住你的" + p + "没放（" + sec1(dur) + " 秒）");
}

String dualText(uint8_t mask) {
  for (int i = 0; i < DUALN; i++) {
    if (DUALS[i].mask == mask) return String(DUALS[i].txt);
  }
  return String("");
}

void releaseOne(int i, unsigned long dur) {
  int mate = -1;
  for (int j = 0; j < 4; j++) {
    if (j == i || eaten[j]) continue;
    if (st[j].pressStart == 0) continue;
    long d = (long)st[j].pressStart - (long)st[i].pressStart;
    if (d < 0) d = -d;
    if (d <= (long)SYNC_MS) { mate = j; break; }
  }
  if (mate >= 0) {
    uint8_t mask = (1 << i) | (1 << mate);
    String comb = dualText(mask);
    if (comb.length()) {
      pushLine(comb);
      eaten[i] = true;
      eaten[mate] = true;
      st[i].peak = 0;
      st[mate].peak = 0;
      return;
    }
    // 组合表里没有：报最强的那一下
    int strongest = (st[i].peak >= st[mate].peak) ? i : mate;
    pushLine(classify(strongest, dur, st[strongest].peak));
    eaten[i] = true;
    eaten[mate] = true;
    st[i].peak = 0;
    st[mate].peak = 0;
    return;
  }
  pushLine(classify(i, dur, st[i].peak));
  eaten[i] = true;
  st[i].peak = 0;
}

void samplePins() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    int v = analogRead(PINS[i]);

    if (!baseSet[i]) {
      base[i] = v; baseSet[i] = true;
      Serial.print("BASE "); Serial.print(KEYS[i]); Serial.print("="); Serial.println(v);
      continue;
    }

    int diff = v - (int)base[i];   // 相对基线抬了多少，这才是"摸"

    // 静默期：射频刚动过（开热点、切 AP_STA、扫信道），ADC 整体在跳。
    // 这段时间只喂基线、快速跟上来，一律不判按下。
    if ((long)(now - settleUntil) < 0) {
      base[i] = base[i] * 0.5f + v * 0.5f;
      st[i].cnt = 0;
      wasSettling = true;
      continue;
    }

    if (!st[i].touch) {
      if (diff > TH_ON) {
        if (++st[i].cnt >= CONFIRM) {          // 连中几次才认，尖峰滤掉
          st[i].cnt = 0;
          if (millis() - lastEvAt[i] < DEBOUNCE_MS) continue;   // 刚报过，这点抖不算
          lastEvAt[i] = millis();
          st[i].touch = true;
          st[i].pressStart = now;
          st[i].peak = diff / 4095.0f;
          eaten[i] = false;
          pushEv(i, (uint16_t)v);              // 只在确认按下时报一次，不刷屏
          Serial.print(">>> "); Serial.print(KEYS[i]); Serial.println(" 按下");
        }
      } else {
        st[i].cnt = 0;
        base[i] = base[i] * 0.98f + v * 0.02f; // 手没在，基线慢慢跟着走，抗温飘
      }
    } else {
      float net = diff / 4095.0f;
      if (net > st[i].peak) st[i].peak = net;
      if (diff < TH_OFF) {                     // 滞回：落够了才算松手
        st[i].touch = false;
        unsigned long dur = now - st[i].pressStart;
        Serial.print("<<< "); Serial.print(KEYS[i]); Serial.println(" 松手");
        if (!eaten[i]) releaseOne(i, dur);
      }
    }
  }

  // 静默期收尾：开门那刻手已经压在板上的，先记成「正按着、不报」。
  // 否则基线被她的手带着跑，之后按多少下都够不到线 —— 表现就是一条触觉都没有。
  if (wasSettling) {
    wasSettling = false;
    for (int i = 0; i < 4; i++) {
      if (!st[i].touch && (analogRead(PINS[i]) - (int)base[i]) > TH_ON) {
        st[i].touch = true;
        st[i].pressStart = now;
        st[i].peak = 0;
        eaten[i] = true;          // 这一次算静默的账，松手不报
      }
    }
  }
}

String liveLines() {
  String out = "";
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    if (st[i].touch) {
      out += String(CMN[i]) + "：正被按着，还没松（已经 " + sec1(now - st[i].pressStart) + " 秒）\n";
    }
  }
  return out;
}

// ---------------------------------------------------------------- 公共响应头

void addHead() {
  srv.sendHeader("Cache-Control", "no-store");
  srv.sendHeader("Access-Control-Allow-Origin", "*");
}

void logReq(const char* tag) {
  reqCount++;
  Serial.print("[REQ] ");
  Serial.print(tag);
  Serial.print(" from ");
  Serial.println(srv.client().remoteIP());
}

// ---------------------------------------------------------------- 配网页面

const char CONFIG_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TouchDoll 配网</title></head>
<body style="font-family:system-ui,sans-serif;padding:22px;max-width:430px;margin:auto;line-height:1.6">
<h2 style="margin-bottom:4px">TouchDoll</h2>
<p style="color:#666;margin-top:0">让它连上你家的 WiFi，只需填一次。</p>
<p id="tip" style="color:#2a7">知道 WiFi 名字的话，直接往下填，不用扫。</p>
<form method="GET" action="/save">
  <button type="button" onclick="doScan()"
    style="width:100%;padding:10px;font-size:15px;margin-bottom:12px">扫一下附近有哪些网（可选）</button>
  <label>🏠 扫到了就选一个；没扫就直接手填下面<br>
    <select id="sel" onchange="document.getElementById('ssid').value=this.value"
      style="width:100%;padding:10px;margin-top:6px;box-sizing:border-box">
      <option value="">— 没扫 —</option>
    </select>
  </label>
  <br><br>
  <label>WiFi 名称<br>
    <input id="ssid" name="ssid" required style="width:100%;padding:10px;margin-top:6px;box-sizing:border-box">
  </label>
  <br><br>
  <label>WiFi 密码<br>
    <input name="pass" type="password" style="width:100%;padding:10px;margin-top:6px;box-sizing:border-box">
  </label>
  <br><br>
  <button style="width:100%;padding:13px;font-size:16px">保存并重启</button>
</form>
<p style="color:#999;font-size:13px">保存后这个热点会自动关闭，板子去连你的路由器。<br>
要重配：在同一网络里用浏览器开 <b>板子地址/reset</b>；换了路由器的话，板子连不上会在 15 秒后自己回到这个配网模式。</p>
<script>
function doScan(){
  const tip=document.getElementById('tip');
  tip.textContent='正在扫，最多两三秒…';
  fetch('/scan').then(r=>r.json()).then(d=>{
    if(d.msg){tip.textContent=d.msg;return;}
    const sel=document.getElementById('sel');
    sel.innerHTML='<option value="">— 没扫 —</option>';
    (d.nets||[]).forEach(n=>{
      const o=document.createElement('option');
      o.value=n.ssid; o.textContent=n.ssid+(n.rssi?'  ('+n.rssi+'dBm)':'');
      sel.appendChild(o);
    });
    tip.textContent='找到 '+((d.nets||[]).length)+' 个网络，选一个或者直接手填';
  }).catch(()=>{tip.textContent='扫描失败，直接手填也能用';});
}
</script></body></html>)HTML";

// ---------------------------------------------------------------- 运行页

const char NAV_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>TouchDoll</title></head>
<body style="font-family:system-ui,sans-serif;padding:22px;max-width:430px;margin:auto;line-height:1.9">
<h2>TouchDoll 在线</h2>
<p style="background:#f4f4f4;padding:10px 12px;border-radius:8px;font-size:15px">
本机地址：<b>{{IP}}</b><br>
<span style="color:#888;font-size:13px">连热点时用 http://192.168.4.1 也能进来</span></p>
<p><a href="/ping">/ping</a> 活着<br>
<a href="/latest">/latest</a> 此刻四路压力<br>
<a href="/events?since=0">/events</a> 事件队列<br>
<a href="/touch?clear=1">/touch</a> 取走最近手感（人话）<br>
<a href="/status">/status</a> 全部状态</p>
<p style="color:#999;font-size:13px"><a href="/reset">重新配网</a></p>
</body></html>)HTML";

// ---------------------------------------------------------------- 路由

void handleRoot() {
  logReq("/");
  addHead();
  // 只有真在配网态才给配网页。跑起来以后就算从热点进来也只看运行页，
  // 因为配网页会触发扫描，S3 单射频一扫热点就掉，手机就连不上来。
  if (configMode) {
    srv.send_P(200, "text/html; charset=utf-8", CONFIG_PAGE);
  } else {
    String page = String(NAV_PAGE);
    page.replace("{{IP}}", WiFi.localIP().toString());
    srv.send(200, "text/html; charset=utf-8", page);
  }
}

void handleScan() {
  logReq("/scan");
  // 只在配网态扫。跑起来以后一律不扫：S3 是单射频，扫的时候热点必掉。
  if (!configMode) {
    addHead();
    srv.send(200, "application/json", "{\"nets\":[],\"msg\":\"板子已经跑起来了，不扫。要换 WiFi 先开 板子地址/reset\"}");
    return;
  }
  // 不全信道跑：被动听 1/6/11 三个常用信道，每信道 120ms，三次加起来不到半秒。
  const uint8_t CHS[3] = {1, 6, 11};
  String j = "{\"nets\":[";
  bool first = true;
  for (int c = 0; c < 3; c++) {
    int n = WiFi.scanNetworks(false, false, true, 120, CHS[c]);
    for (int i = 0; i < n && i < 12; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      if (s.indexOf('"') >= 0) continue;
      if (j.indexOf("\"" + s + "\"") >= 0) continue;   // 三个信道会扫到重复的，只列一次
      if (!first) j += ",";
      j += "{\"ssid\":\"" + s + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
      first = false;
    }
    WiFi.scanDelete();
  }
  j += "]}";
  addHead();
  srv.send(200, "application/json", j);
}

void handleSave() {
  logReq("/save");
  String s = srv.arg("ssid");
  String p = srv.arg("pass");
  s.trim();                       // 手填很容易带个尾空格，直接连不上
  if (s.length() == 0) {
    addHead();
    srv.send(400, "text/plain; charset=utf-8", "没填 WiFi 名称");
    return;
  }
  size_t w1 = prefs.putString("ssid", s);
  size_t w2 = prefs.putString("pass", p);
  // 写完立刻回读 + 键存在 + 写入字节数：三个都对，还是重启就读空，
  // 那就是 NVS 分区本身没写进去（分区表选错 / NVS 分区坏）。
  Serial.print("写入字节 ssid="); Serial.print(w1);
  Serial.print(" pass="); Serial.println(w2);
  Serial.print("回读 ssid=["); Serial.print(prefs.getString("ssid", ""));
  Serial.print("] 键存在="); Serial.println(prefs.isKey("ssid"));
  // 密码不打印，只打长度：能看出有没有被截断或漏字
  Serial.print("已保存 WiFi: ["); Serial.print(s);
  Serial.print("]  名称 "); Serial.print(s.length());
  Serial.print(" 字  密码 "); Serial.print(p.length()); Serial.println(" 字");
  addHead();
  srv.send(200, "text/html; charset=utf-8",
    "<meta charset=utf-8><body style='font-family:sans-serif;padding:24px'>"
    "<h3>存好了</h3><p>板子这就去连 <b>" + s + "</b>，<b>不重启、热点不断</b>。</p>"
    "<p style='color:#666'>连上会切到运行态；二十秒后刷新这个页面，"
    "<b>还看到配网页就是没连上</b>，原因打在串口里。</p>"
    "<p style='color:#999;font-size:13px'>板子只认 2.4G 的 WiFi，5G 连不上。</p>");
  delay(300);
  // 不重启！这块板子的 NVS 写进去读不回来，一重启就等于把刚填的忘干净。
  // 就在当前进程里直接连：热点一直挂着，手机不会掉，连上再切运行态。
  pendSsid = s;
  pendPass = p;
  tryNow   = true;
}

void handleReset() {
  logReq("/reset");
  prefs.remove("ssid");
  prefs.remove("pass");
  addHead();
  srv.send(200, "application/json", "{\"ok\":1,\"msg\":\"已清空，重启进入配网\"}");
  delay(800);
  ESP.restart();
}

void handlePing() {
  logReq("/ping");
  addHead();
  srv.send(200, "application/json", "{\"ok\":1,\"name\":\"touchdoll\"}");
}

void handleLatest() {
  logReq("/latest");
  String j = "{\"seq\":" + String(seq) + ",";
  for (int i = 0; i < 4; i++) {
    j += String("\"") + KEYS[i] + "\":" + String(analogRead(PINS[i]));
    if (i < 3) j += ",";
  }
  j += "}";
  addHead();
  srv.send(200, "application/json", j);
}

void handleEvents() {
  logReq("/events");
  unsigned long since = 0;
  if (srv.hasArg("since")) since = strtoul(srv.arg("since").c_str(), NULL, 10);
  String j = "{\"seq\":" + String(seq) + ",\"ev\":[";
  bool first = true;
  for (int i = tail; i != head; i = (i + 1) % EVMAX) {
    if (buf[i].id == 0 || buf[i].id <= since) continue;
    if (!first) j += ",";
    j += "[" + String(buf[i].id) + "," + String(buf[i].t) + "," +
         String(buf[i].pin) + "," + String(buf[i].v) + "]";
    first = false;
  }
  j += "]}";
  addHead();
  srv.send(200, "application/json", j);
}

void handleTouch() {
  logReq("/touch");
  bool clear = srv.hasArg("clear");
  String body = "";
  for (int i = txt; i != txh; i = (i + 1) % TXMAX) {
    if (txbuf[i].id > lastRead) {
      body += String(txbuf[i].txt) + "\n";
    }
  }
  if (clear) lastRead = txSeq;
  body += liveLines();
  addHead();
  srv.send(200, "text/plain; charset=utf-8", body);
}

void handleStatus() {
  logReq("/status");
  String j = "{\"ok\":1,\"mode\":\"" + String(configMode ? "config" : "run") + "\"";
  j += ",\"ip\":\"" + (configMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"";
  if (!configMode) {
    j += ",\"gw\":\"" + WiFi.gatewayIP().toString() + "\"";
    j += ",\"rssi\":" + String(WiFi.RSSI());
  }
  j += ",\"uptime\":" + String(millis() / 1000);
  j += ",\"seq\":" + String(seq);
  j += ",\"queued\":" + String(evCount());
  j += ",\"req\":" + String(reqCount);
  j += "}";
  addHead();
  srv.send(200, "application/json", j);
}

void handleNotFound() {
  String u = srv.uri();

  // 各家系统都会偷偷发一个"这网有没有外网"的探针。不哄它，安卓就判定
  // 此热点没有互联网，把浏览器的流量切回 4G —— 表现就是 192.168.4.1
  // 蓝条跑一点、等半天、无法访问，而串口连一个 [REQ] 都不出现。
  if (u.indexOf("generate_204") >= 0 || u.indexOf("gen_204") >= 0 ||
      u.indexOf("connectivitycheck") >= 0 || u.indexOf("clients3") >= 0) {
    srv.send(204, "text/plain", "");
    return;
  }
  if (u.indexOf("hotspot-detect") >= 0 || u.indexOf("ncsi") >= 0 ||
      u.indexOf("captive") >= 0 || u.indexOf("library/test") >= 0) {
    srv.send(200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    return;
  }

  logReq("NOTFOUND");
  Serial.print("   ↳ 它要的是 [");
  Serial.print(u);
  Serial.println("]");
  addHead();
  srv.send(404, "application/json",
    "{\"error\":\"no such path\",\"try\":[\"/ping\",\"/latest\",\"/events?since=0\",\"/status\"]}");
}

// ---------------------------------------------------------------- 两种模式

void registerRoutes() {
  if (routesUp) return;         // 只注册一次，重复 srv.begin() 会让路由表错乱
  routesUp = true;
  srv.on("/",        handleRoot);
  srv.on("/scan",    handleScan);
  srv.on("/save",    handleSave);
  srv.on("/reset",   handleReset);
  srv.on("/ping",    handlePing);
  srv.on("/latest",  handleLatest);
  srv.on("/events",  handleEvents);
  srv.on("/touch",   handleTouch);
  srv.on("/status",  handleStatus);
  srv.onNotFound(handleNotFound);
  srv.begin();
}

void broadcastSelf() {
  IPAddress bc = WiFi.localIP();
  bc[3] = 255;
  udp.beginPacket(bc, 8888);
  udp.print("TOUCHDOLL:");
  udp.print(WiFi.localIP().toString());
  udp.endPacket();
}

void heartbeat() {
  if (millis() - lastHB < 10000) return;
  lastHB = millis();
  Serial.print("[HB] ");
  Serial.print(millis() / 1000);
  Serial.print("s  mode=");
  Serial.print(configMode ? "config" : "run");
  Serial.print("  ip=");
  Serial.print(configMode ? WiFi.softAPIP() : WiFi.localIP());
  Serial.print("  seq=");
  Serial.print(seq);
  Serial.print("  queued=");
  Serial.print(evCount());
  Serial.print("  req=");
  Serial.print(reqCount);
  Serial.print("  v=");
  for (int i = 0; i < 4; i++) {
    Serial.print(analogRead(PINS[i]));
    Serial.print(i < 3 ? "/" : "");
  }
  Serial.println();
}

void ensureApSsid() {
  if (apSsid.length()) return;
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  apSsid = String(AP_PREFIX) + mac.substring(mac.length() - 4);
}

void startConfig() {
  configMode = true;

  // 配网态只做热点。原本是 AP_STA，STA 那边还挂着一路"连路由器"的重试，
  // S3 单射频被它一分，热点就时有时无。
  // 注意别用 WiFi.disconnect(true)：第二个参数是关射频，关完再开 AP，
  // DHCP 可能起不来，手机会关联成功但拿不到 192.168.4.x，请求全部超时。
  WiFi.mode(WIFI_AP);
  // 网段和网关固定死，DHCP 池才不会飘
  IPAddress apIP(192, 168, 4, 1);
  IPAddress apMask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apIP, apMask);
  ensureApSsid();

  WiFi.softAP(apSsid.c_str(), AP_PASS);
  delay(300);
  resetTouch(true);   // 开热点这一下 ADC 会整体跳，先静默一段再开口

  dns.start(53, "*", WiFi.softAPIP());   // 连上就弹页面

  registerRoutes();

  Serial.println("========== 配网模式 ==========");
  Serial.print("热点名 : "); Serial.println(apSsid);
  Serial.print("密码   : "); Serial.println(AP_PASS);
  Serial.print("页面   : http://"); Serial.println(WiFi.softAPIP());
  Serial.println("==============================");
}

void startRun() {
  configMode = false;
  // 刚连上路由器，射频状态整个变了。先把手里的触摸状态清掉、基线重学，
  // 不然 AP→AP_STA 这一下会把四路一起顶成「按下」，客户端收到一串假数据。
  resetTouch(true);
  registerRoutes();
  MDNS.begin("touchdoll");
  udp.begin(8889);
  // 热点常驻：随时能连回来重配，客户不用记任何地址
  ensureApSsid();
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);   // 连路由和开热点要同时，别让它落在纯 STA
  // 热点已经开着就别再 softAP 一次：重开等于射频重启，ADC 又跳一轮。
  if (WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
    WiFi.softAP(apSsid.c_str(), AP_PASS);
  }
  // 运行模式不劫持 DNS：手机连上热点不会每次都弹验证页、也不会被自动拽去配网页
  // （要配网页就手动开 192.168.4.1/reset）。原来这里一行 dns.start，注释掉了。
  // dns.start(53, "*", WiFi.softAPIP());
  Serial.println("========== 运行模式 ==========");
  Serial.print("我的地址 : "); Serial.println(WiFi.localIP());
  Serial.print("网关     : "); Serial.println(WiFi.gatewayIP());
  Serial.print("重配热点 : "); Serial.print(apSsid);
  Serial.print(" / "); Serial.print(AP_PASS);
  Serial.println("  → 连上开 192.168.4.1");
  Serial.println("==============================");
}

// ---------------------------------------------------------------- 连路由器
// 扫一眼 + 连 + 等，返回连没连上。连不上的原因全打在串口里，不让人猜。
// 配网态和 setup 都走这一个函数，别两处各写一套。
bool connectSta(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_AP_STA);        // 热点不能停：手机还连在上面看页面
  WiFi.setSleep(false);
  delay(200);

  int n = WiFi.scanNetworks();
  bool seen = false;
  Serial.print("扫到 "); Serial.print(n); Serial.println(" 个网络：");
  for (int i = 0; i < n; i++) {
    Serial.print("    "); Serial.print(WiFi.SSID(i));
    Serial.print("  ch="); Serial.print(WiFi.channel(i));
    Serial.print("  rssi="); Serial.println(WiFi.RSSI(i));
    if (WiFi.SSID(i) == ssid) seen = true;
  }
  WiFi.scanDelete();
  if (seen) Serial.println("目标网在列表里");
  else Serial.println("!! 列表里没有这个网名：名字打错了，或者路由器只开 5G");

  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t0 = millis();
  int lastSt = -1;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
    int cur = WiFi.status();
    if (cur != lastSt) { lastSt = cur; Serial.print("[st="); Serial.print(cur); Serial.print("]"); }
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("连上了，IP="); Serial.println(WiFi.localIP());
    return true;
  }

  int fst = WiFi.status();
  Serial.print("没连上，status="); Serial.print(fst);
  switch (fst) {
    case WL_NO_SSID_AVAIL:    Serial.println("  找不到这个网名"); break;
    case WL_CONNECT_FAILED:   Serial.println("  密码不对，或加密是 WPA3（板子只认 WPA2）"); break;
    case WL_CONNECTION_LOST:  Serial.println("  连上又掉，信号太弱"); break;
    case WL_DISCONNECTED:     Serial.println("  被断开，密码多半错了"); break;
    default:                  Serial.println("  超时"); break;
  }
  // 连不上就把 STA 关掉，别让它在后台一直重试抢射频（S3 单射频，会把热点拖没）
  WiFi.mode(WIFI_AP);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== TouchDoll 启动 ===");

  for (int i = 0; i < 4; i++) {
    pinMode(GNDPOSTS[i], OUTPUT);
    digitalWrite(GNDPOSTS[i], LOW);
  }
  analogReadResolution(12);

  bool nvsOk = prefs.begin("touchdoll", false);
  Serial.print("NVS begin="); Serial.print(nvsOk);
  Serial.print("  空位="); Serial.print(prefs.freeEntries());
  Serial.print("  有ssid键="); Serial.println(prefs.isKey("ssid"));
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.length() > 0) {
    Serial.print("记得的 WiFi: ["); Serial.print(ssid); Serial.println("]");
    if (connectSta(ssid, pass)) { startRun(); return; }
    Serial.println("转去配网");
  } else {
    Serial.println("还没配过 WiFi");
  }

  startConfig();
}

void loop() {
  samplePins();

  if (configMode) {
    // 配网页刚存完：在当前进程里直接去连，不重启，热点不断。
    if (tryNow) {
      tryNow = false;
      Serial.print("开始连路由器: ["); Serial.print(pendSsid); Serial.println("]");
      if (connectSta(pendSsid, pendPass)) {
        Serial.println("连上了，切到运行态");
        startRun();
      } else {
        Serial.println("没连上，留在配网态；改完再存一次");
      }
    }
    dns.processNextRequest();
    srv.handleClient();
    heartbeat();
    delay(5);
    return;
  }

  srv.handleClient();
  heartbeat();

  if (WiFi.status() == WL_CONNECTED) {
    offlineSince = 0;
    if (millis() - lastBC > 3000) {
      lastBC = millis();
      broadcastSelf();
    }
  } else {
    if (offlineSince == 0) offlineSince = millis();
    if (millis() - offlineSince > 60000) {
      Serial.println("断了太久，重启进配网");
      ESP.restart();
    }
  }

  delay(5);
}