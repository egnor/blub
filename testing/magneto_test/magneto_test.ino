#include <Arduino.h>

#include <Adafruit_ADS1X15.h>
#include <ok_device_id.h>
#include <ok_logging.h>

#include "src/blub_station.h"

static const OkLoggingContext OK_CONTEXT("magneto_test");
static Adafruit_ADS1115 adc_a;
static Adafruit_ADS1115 adc_b;

void loop() {
  delay(200);
  int16_t a[4], b[4];
  for (int i = 0; i < 4; i++) {
    a[i] = adc_a.readADC_SingleEnded(i);
    b[i] = adc_b.readADC_SingleEnded(i);
  }
  OK_NOTE(
    "A: %+5d %+5d %+5d %+5d B: %+5d %+5d %+5d %+5d",
    a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]
  );
}

void setup() {
  blub_station_init("magneto_test");
  while (!adc_a.begin(0x48)) {
    OK_ERROR("ADS1115[A] initialization failed...");
    delay(500);
  }
  while (!adc_b.begin(0x49)) {
    OK_ERROR("ADS1115[B] initialization failed...");
    delay(500);
  }
}
