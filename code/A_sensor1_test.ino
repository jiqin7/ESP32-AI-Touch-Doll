void setup() {
  Serial.begin(115200);
  // 等待串口连接，最多等5秒，防止跑太快看不到输出
  unsigned long start = millis();
  while (!Serial && (millis() - start < 5000)) {
    delay(10);
  }

  // 设置 GPIO 15 为接地桩，提供稳定的 GND
  pinMode(15, OUTPUT);
  digitalWrite(15, LOW);
  
  analogReadResolution(12); // 设置 ADC 分辨率为 12 位 (0-4095)
  Serial.println("系统准备完毕，开始测试头顶传感器...");
}

void loop() {
  int val = analogRead(4); // 读取 GPIO 4 的模拟值
  
  Serial.print("头顶力度: ");
  Serial.println(val);
  
  delay(100); // 每 100 毫秒读取一次
}