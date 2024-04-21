#include <Arduino.h>

#include "blub_station.h"
#include "chatty_logging.h"
#include "little_status.h"
#include "xbee_api.h"
#include "xbee_radio.h"
#include "xbee_monitor.h"

XBeeMonitor* monitor = nullptr;

void loop() {
  using namespace XBeeApi;
  static Frame frame;

  while (xbee_radio->poll_api(&frame)) {
    if (monitor->maybe_handle_frame(frame)) continue;

    int extra_size;
    if (auto* modem = frame.decode_as<ModemStatus>()) {
      auto const* text = modem->status_text();
      CL_NOTICE("ModemStatus: %s", text);
      status_screen->line_printf(3, "\f9Modem: %s", text);
    } else {
      CL_NOTICE("Other payload type (0x%02x)", frame.type);
    }
  }

  while (monitor->maybe_emit_frame(xbee_radio->send_available(), &frame)) {
    xbee_radio->send_api_frame(frame);
  }

  if (xbee_radio->send_available() > 0) {
    status_screen->line_printf(2, "\f9Radio API ready");
  }

}

void setup() {
  while (!Serial.dtr() && millis() < 2000) delay(10);
  blub_station_init("BLUB XBee Test");

  if (xbee_radio->raw_serial()) {
    status_screen->line_printf(1, "\f9Radio found");
  } else {
    status_screen->line_printf(1, "\f9Radio NOT found");
  }

  monitor = make_xbee_monitor();
}
