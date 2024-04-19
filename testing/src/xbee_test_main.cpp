#include <Arduino.h>

#include "blub_station.h"
#include "chatty_logging.h"
#include "little_status.h"
#include "xbee_radio.h"

void loop() {
  XBeeRadio::ApiFrame frame;
  if (xbee_radio->poll_api(&frame)) {
  }

  if (xbee_radio->raw_serial()) {
    status_screen->line_printf(1, "\f9Radio found");
  } else {
    status_screen->line_printf(1, "\f9Radio NOT found");
  }

  if (xbee_radio->api_ready()) {
    status_screen->line_printf(2, "\f9Radio API ready");
  }
}

void setup() {
  while (!Serial.dtr() && millis() < 2000) delay(10);
  blub_station_init("BLUB XBee Test");
}
