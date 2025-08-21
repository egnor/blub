#include <Arduino.h>

#include "src/blub_station.h"
#include "src/little_status.h"
#include "src/ok_logging.h"

static const OkLoggingContext OK_CONTEXT("display_test");

void loop() {
  OK_DETAIL("LOOP");
  status_screen->line_printf(1, "\f6six\b(6)\b \f7seven\b(7)\b \f8eight\b(8)");
  status_screen->line_printf(2, "\f9nine\b(9)\b \f11el.\b(11)\b \f12tw.\b(12)");
  status_screen->line_printf(3, "\f14fourt.\b(14)\b \f15fift.\b(15)");
  status_screen->line_printf(4, "\f12c\to\tl\tu(xyzzy)\tm\tn\ts");
  delay(100);
}

void setup() {
  while (!Serial.dtr() && millis() < 2000) delay(10);
  blub_station_init("BLUB Display Test");
}
