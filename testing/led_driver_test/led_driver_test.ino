#include <Arduino.h>

#include <Adafruit_PWMServoDriver.h>

#include "src/blub_station.h"
#include "src/tagged_logging.h"

static const TaggedLoggingContext TL_CONTEXT("led_driver_test");
static Adafruit_PWMServoDriver pwm;

void loop() {
  const int t = millis() % 0x600;
  if (t < 0x100) {
    pwm.setPin(0, 0xFFF); pwm.setPin(1, 0); pwm.setPin(2, (0xFF - t) * 0x10);
  } else if (t < 0x200) {
    pwm.setPin(0, 0xFFF); pwm.setPin(1, (t - 0x100) * 0x10); pwm.setPin(2, 0);
  } else if (t < 0x300) {
    pwm.setPin(0, (0x2FF - t) * 0x10); pwm.setPin(1, 0xFFF); pwm.setPin(2, 0);
  } else if (t < 0x400) {
    pwm.setPin(0, 0); pwm.setPin(1, 0xFFF); pwm.setPin(2, (t - 0x300) * 0x10);
  } else if (t < 0x500) {
    pwm.setPin(0, 0); pwm.setPin(1, (0x4FF - t) * 0x10); pwm.setPin(2, 0xFFF);
  } else if (t < 0x600) {
    pwm.setPin(0, (t - 0x500) * 0x10); pwm.setPin(1, 0); pwm.setPin(2, 0xFFF);
  } else {
    pwm.setPin(0, 0);
    pwm.setPin(1, 0);
    pwm.setPin(2, 0);
  }
  delay(10);
}

void setup() {
  blub_station_init("led_driver_test");
  while (!pwm.begin()) {
    TL_PROBLEM("PCA9685 initialization failed...");
    delay(500);
  }

  for (int p = 0; p < 16; ++p) pwm.setPin(0, 0);
}
