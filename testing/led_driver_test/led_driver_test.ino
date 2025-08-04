#include <Arduino.h>

#include <Adafruit_PWMServoDriver.h>

#include "src/blub_station.h"
#include "src/tagged_logging.h"

static const TaggedLoggingContext TL_CONTEXT("led_driver_test");
static Adafruit_PWMServoDriver pwm;

void loop() {
  const int l = (millis() % 1000) * 4;
  for (int p = 0; p < 16; ++p) pwm.setPin(0, l);
  delay(10);
}

void setup() {
  blub_station_init("led_driver_test");
  while (!pwm.begin()) {
    TL_PROBLEM("PCA9685 initialization failed...");
    delay(500);
  }
}
