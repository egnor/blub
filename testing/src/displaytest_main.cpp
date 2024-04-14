#include <Arduino.h>
#include <Wire.h>

#include <U8g2lib.h>

using DisplayDriver = U8G2_SSD1306_128X64_NONAME_F_HW_I2C;

constexpr int I2C_ADDR = 0x3D;
constexpr int NRESET_PIN = 10;

DisplayDriver* driver;

void loop() {
  Serial.printf("UPDATING\n");
  driver->clearBuffer();
  driver->setDrawColor(1);
  driver->drawBox(10, 10, 20, 20);
  driver->updateDisplay();
  delay(100);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("### 🖵  DISPLAY TEST ###\n");

  Serial.printf("Resetting display...\n");
  pinMode(NRESET_PIN, OUTPUT);
  digitalWrite(NRESET_PIN, LOW);
  delay(50);
  digitalWrite(NRESET_PIN, HIGH);
  delay(200);

  Serial.printf("Initializing I2C...\n");
  if (!Wire.setSDA(SDA) || !Wire.setSCL(SCL)) {
    Serial.printf("*** I2C init failed (SCL=%d, SDA=%d)\n", SCL, SDA);
    while (true) {}
  }
  Wire.begin();

  Serial.printf("Testing display presence...\n");
  Wire.beginTransmission(I2C_ADDR);
  auto const i2c_status = Wire.endTransmission();
  if (i2c_status != 0) {
    Serial.printf("*** I2C probe 0x%02X failed (e=%d)\n", I2C_ADDR, i2c_status);
    while (true) {}
  }

  // See https://github.com/olikraus/u8g2/issues/2425
  Serial.printf("Creating display...\n");
  driver = new DisplayDriver(U8G2_R0, NRESET_PIN);
  driver->setI2CAddress(I2C_ADDR << 1);
  Serial.printf("Initializing display...\n");
  driver->begin();

  Serial.printf("Starting screen refresh...\n");
}
