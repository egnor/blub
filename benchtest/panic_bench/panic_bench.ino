void setup() {
  Serial.begin(115200);
  Serial.println("Hello, Raspberry Pi Pico!");
  Serial.println("I'm about to panic...");
  for (int i = 5; i >= 0; --i) {
    Serial.print("Panic in ");
    Serial.println(i);
    delay(1000);
  }
  panic("OMG I AM PANICKING");
}

void loop() {
  Serial.println("Looping...");
  delay(500);
}
