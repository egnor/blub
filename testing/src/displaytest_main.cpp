#include <memory>

#include <Arduino.h>
#include <Wire.h>

#include <U8g2lib.h>

#include "little_status.h"

constexpr int I2C_ADDR = 0x3D;
constexpr int NRESET_PIN = 10;

using DisplayDriver = U8G2_SSD1306_128X64_NONAME_F_HW_I2C;
std::unique_ptr<DisplayDriver> driver;
std::unique_ptr<LittleStatus> status;

void loop() {
  Serial.printf("UPDATING\n");
  status->line_printf(0, "\f6six \b(6)\b \f7seven \b(7)\b \f8eight \b(8)");
  status->line_printf(1, "\f9nine \b(9)\b \f11el. \b(11)\b \f12tw. \b(12)");
  status->line_printf(2, "\f14fourt. \b(14)\b \f15fift. \b(15)");
  status->line_printf(3, "c\to\tl\tu(xyzzy)\tm\tn\ts");
  delay(100);
}

void setup() {
  Serial.begin(115200);
  while (!Serial.dtr() && millis () < 500) {}
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
  Serial.printf("Creating display driver...\n");
  driver = std::make_unique<DisplayDriver>(U8G2_R0, NRESET_PIN);
  driver->setI2CAddress(I2C_ADDR << 1);
  Serial.printf("Initializing display driver...\n");
  driver->begin();

  Serial.printf("Setting up status output...\n");
  status = make_little_status(driver->getU8g2());

  Serial.printf("Starting screen refresh...\n");
}
