void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && (millis() - start < 5000)) delay(10);

  // 设置接地桩：头顶用 GPIO 15，脸颊用 GPIO 18（配合你现在的接线）
  pinMode(15, OUTPUT); digitalWrite(15, LOW);
  pinMode(18, OUTPUT); digitalWrite(18, LOW);
  
  analogReadResolution(12);
  Serial.println("开始测试头顶和脸颊...");
}

void loop() {
  int topVal = analogRead(4);   // 头顶，对应 GPIO 4
  int cheekVal = analogRead(5); // 脸颊，对应 GPIO 5
  
  Serial.print("头顶: ");
  Serial.print(topVal);
  Serial.print("  |  脸颊: ");
  Serial.println(cheekVal);
  
  delay(200);
}