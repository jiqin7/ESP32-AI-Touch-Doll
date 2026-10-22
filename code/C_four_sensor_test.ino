void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && (millis() - start < 5000)) delay(10);

  // 接地桩：头顶(15)、脸颊(18)、前胸(17)、后背(16)
  int gndPins[4] = {15, 18, 17, 16}; 
  for(int i = 0; i < 4; i++) {
    pinMode(gndPins[i], OUTPUT);
    digitalWrite(gndPins[i], LOW);
  }
  
  analogReadResolution(12);
  Serial.println("系统准备完毕，开始测试全部4个传感器...");
}

void loop() {
  int pins[4] = {4, 5, 6, 7};
  const char* keys[4] = {"头顶", "脸颊", "前胸", "后背"};
  
  for(int i = 0; i < 4; i++) {
    int v = analogRead(pins[i]);
    Serial.print(keys[i]);
    Serial.print(": ");
    Serial.print(v);
    Serial.print("  ");
  }
  Serial.println();
  delay(200);
}