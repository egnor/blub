#include <Arduino.h>

#include <ok_device_id.h>
#include <ok_logging.h>

static const OkLoggingContext OK_CONTEXT("device_id_test");

extern char const* const ok_logging_config = "*=DETAIL";

void loop() {
  auto const devid = ok_device_id(Wire, true);
  OK_NOTE("LOOP devid: part=%d pins=%016llx", devid.part, devid.pins);
  delay(500);
}

void setup() {
  Serial.begin(115200);
  OK_NOTE("SETUP");
  Wire.begin();
  // pinMode(5, INPUT_PULLUP);
  // pinMode(17, INPUT_PULLUP);
  // Wire.begin(5, 17);
}
