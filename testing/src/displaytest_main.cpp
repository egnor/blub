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
  status->set_line_size(0, 6);
  status->line_printf(0, "Six points of text and what do you get");
  status->set_line_size(1, 7);
  status->line_printf(1, "Slightly bigger 7 pixel text");
  status->set_line_size(2, 8);
  status->line_printf(2, "This is some 8 pixel text");
  status->set_line_size(3, 9);
  status->line_printf(3, "And now 9 pixels high");
  status->set_line_size(4, 11);
  status->line_printf(4, "This one goes to 11");
  status->set_line_size(5, 12);
  status->line_printf(5, "12 noon or midnight");
  status->set_line_size(6, 14);
  status->line_printf(6, "Fourteen is a lot");
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
