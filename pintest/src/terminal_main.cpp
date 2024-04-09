#include <algorithm>

#include <Arduino.h>

long blink_until_millis = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial.dtr()) delay(1);
  delay(100);

  Serial.printf("▶️ STARTING RESET\n");
  pinMode(11, OUTPUT);
  pinMode(12, OUTPUT);
  digitalWrite(11, LOW);
  digitalWrite(12, LOW);
  delay(100);
  digitalWrite(11, HIGH);
  Serial.printf("♻️ RESET COMPLETE\n");
  delay(100);
  pinMode(12, INPUT);
  Serial.printf("💥 BREAK COMPLETE\n");

  Serial1.setRX(13);
  Serial1.setTX(12);
  Serial1.begin(9600);
}

void loop() {
  int const host_n = std::min(Serial1.available(), Serial.availableForWrite());
  for (int i = 0; i < host_n; ++i) {
    auto const ch = Serial1.read();
    Serial.printf("R   %d\n", ch);
    // Serial.write(ch);
  }

  int const modem_n = std::min(Serial.available(), Serial1.availableForWrite());
  for (int i = 0; i < modem_n; ++i) {
    auto const ch = Serial.read();
    Serial.printf("T %d\n", ch);
    Serial1.write(ch);
  }

  if (host_n > 0 || modem_n > 0) {
    // digitalWrite(LED_BUILTIN, HIGH);
    blink_until_millis = millis() + 20;
  } else if (millis() > blink_until_millis) {
    // digitalWrite(LED_BUILTIN, LOW);
  }
}
