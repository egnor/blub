#include <Arduino.h>

#include <Adafruit_PWMServoDriver.h>
#include <ok_logging.h>

#include "src/blub_station.h"

static const OkLoggingContext OK_CONTEXT("led_driver_test");
static Adafruit_PWMServoDriver pwm;

static void read_command() {
  Serial.print("PIN PERCENT> ");
  Serial.flush();

  char line[80] = "";
  int len = 0;
  while (true) {
    if (Serial.available()) {
      auto const ch = Serial.read();
      if (ch == '\n' || ch == '\r') break;
      if (len < sizeof(line) - 1) line[len++] = ch;
    }
  }
  line[len] = '\0';

  int pin = -1, percent = -1;
  if (sscanf(line, "%d%d", &pin, &percent) != 2) {
    Serial.printf("[%s] parse error\n", line);
  } else {
    if (pin < 0 || pin > 15) {
      Serial.printf("Bad pin: %d\n", pin);
    } else if (percent < 0 || percent > 100) {
      Serial.printf("Bad percent: %d\n", percent);
    } else {
      Serial.printf("OK pin=%d percent=%d\n", pin, percent);
      pwm.setPin(pin, percent * 0x1000 / 100);
    }
  }
}

void loop() {
  auto const start = millis();
  for (int p = 0; p < 16; ++p) {
    if (p % 8 == 0) Serial.printf("%.3fs ", start / 1000.0f);
    if (p % 8 > 0) Serial.print(" ");
    int const on = pwm.getPWM(p, false), off = pwm.getPWM(p, true);
    if (on == 0x1000 && off == 0) {
      Serial.printf("%2d:ON ", p);
    } else if (on == 0 && off == 0x1000) {
      Serial.printf("%2d:off", p);
    } else {
      Serial.printf("%2d:%02d%%", p, (off - on + 1) * 100 / 0x1000);
    }
    if (p % 8 == 7) Serial.println();
    if (p % 8 == 15) Serial.println();
  }

  while (millis() < start + 500) {
    if (Serial.available()) {
      read_command();
      break;
    }
    delay(1);
  }
}

void setup() {
  blub_station_init("led2Ax12_test");
  while (!pwm.begin()) {
    OK_ERROR("PCA9685 initialization failed...");
    delay(500);
  }

  pwm.setPWMFreq(2000);
  for (int p = 0; p < 16; ++p) pwm.setPin(0, 0);
  while (Serial.available()) Serial.read();
}
