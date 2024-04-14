#include <memory>

#include <Arduino.h>
#include <Wire.h>

#include <U8g2lib.h>

#include "little_status.h"

constexpr int I2C_ADDR = 0x3D;
constexpr int NRESET_PIN = 10;

using DisplayDriver = U8G2_SSD1306_128X64_NONAME_F_HW_I2C;

std::unique_ptr<LittleStatus> status;

void loop() {
  Serial.printf("UPDATING\n");
  status->line_printf(0, "\f6\b6 points of \f15text\f6\b and what do you get");
  status->line_printf(1, "\f7Slightly bigger \b7 pixel text");
  status->line_printf(2, "\f8This is some \b8 pixel text");
  status->line_printf(3, "\f9And now \b9 pixels\b high");
  status->line_printf(4, "\f11This one \bgoes to 11\b");
  status->line_printf(5, "\f12\b12 noon\b or midnight");
  status->line_printf(6, "\f14\bFourteen\b is a lot");
  delay(100);
}

void setup() {
  Serial.begin(115200);
  while (!Serial.dtr()) {}
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
  auto driver = std::make_unique<DisplayDriver>(U8G2_R0, NRESET_PIN);
  driver->setI2CAddress(I2C_ADDR << 1);
  Serial.printf("Initializing display driver...\n");
  driver->begin();

  Serial.printf("Setting up status output...\n");
  status = make_little_status(std::move(driver));

  Serial.printf("Starting screen refresh...\n");
}
