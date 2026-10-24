#include <WiFi.h>
#include <HTTPClient.h>

unsigned long loopCount = 0;

// = = = = = = = = = = = = = = = = = = = = 需要你修改的地方 = = = = = = = = = = = = = = = = = = =

// ★★★ 请填入你自己的 WiFi 名称和密码 ★★★
const char* SSID = "YOUR_WIFI_SSID";
const char* PASS = "YOUR_WIFI_PASSWORD";

// ★★★ 填入运行 Python 服务的设备 IP 地址（电脑或手机） ★★★
const char* HOME = "http://你的IPv4地址/hello";

const int PINS[4] = {4, 5, 6, 7};
const char* KEYS[4] = {"top", "cheek", "chest", "hand"};
const int GNDPOSTS[4] = {15, 18, 17, 16};

void setup() {
  Serial.begin(115200);

  Serial.println("=== 我启动啦！ ===");

  unsigned long start = millis();
  while (!Serial && (millis() - start < 5000)) delay(10);

  for (int i = 0; i < 4; i++) {
    pinMode(GNDPOSTS[i], OUTPUT);
    digitalWrite(GNDPOSTS[i], LOW);
  }

  analogReadResolution(12);

  Serial.println("正在连接WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(SSID, PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println(".");
  }

  Serial.println("\nWiFi连接成功! IP地址:");
  Serial.println(WiFi.localIP());
}

void loop() {
  String j = "{";
  int m = 0;

  for (int i = 0; i < 4; i++) {
    int v = analogRead(PINS[i]);
    m = max(m, v);

    j += String("\"") + KEYS[i] + "\":" + String(v);
    if (i < 3) j += ",";
  }

  j += "}";

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient h;
    h.begin(HOME);
    h.addHeader("Content-Type", "application/json");

    int code = h.POST(j);
    h.end();

    loopCount++;

    if (loopCount % 20 == 0) {
      Serial.println("");
      Serial.println(loopCount);
      Serial.print("] POST=");
      Serial.print(code);
      Serial.print(" ");
      Serial.println(j);
    }
  }

  delay(10);
}
