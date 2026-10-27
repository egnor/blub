#include <Adafruit_ADS1X15.h>
#include <Arduino.h>
#include <ok_little_layout.h>
#include <ok_logging.h>
#include <ok_micro_dock.h>
#include <U8g2lib.h>

#include <blub_warnings.h>

static const OkLoggingContext OK_CONTEXT("adc_bench");
static Adafruit_ADS1115 adc_a;
static Adafruit_ADS1115 adc_b;

void loop() {
  delay(200);
  int16_t a[4], b[4];
  for (int i = 0; i < 4; ++i) {
    a[i] = adc_a.readADC_SingleEnded(i);
    b[i] = adc_b.readADC_SingleEnded(i);
  }
  OK_NOTE(
    "A: %+5d %+5d %+5d %+5d B: %+5d %+5d %+5d %+5d",
    a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]
  );
  for (int i = 0; i < 4; ++i) {
    ok_dock_layout->line_printf(i + 1, "\f6A%d:%4d\tB%d:%4d", i, a[i], i, b[i]);
  }
}

void setup() {
  ok_serial_begin();
  OK_NOTE("BLUB ADC Bench Test");
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f7\1ADC Bench");

  while (!adc_a.begin(0x48)) {
    OK_ERROR("ADS1115[A] initialization failed...");
    delay(500);
  }
  while (!adc_b.begin(0x49)) {
    OK_ERROR("ADS1115[B] initialization failed...");
    delay(500);
  }
}
