#include <Arduino.h>

#include "blub_station.h"
#include "chatty_logging.h"
#include "little_status.h"
#include "xbee_radio.h"

static long next_status_update = 0;

void loop() {
  XBeeRadio::IncomingFrame incoming;
  while (xbee_radio->poll_api(&incoming)) {
    switch (incoming.type) {
      case XBeeRadio::ModemStatus::TYPE: {
        auto const* text = incoming.payload.modem_status.status_text();
        CL_REMARK("ModemStatus: %s", text);
        status_screen->line_printf(3, "\f9Modem: %s", text);
        break;
      }
      case XBeeRadio::ATCommandResponse::TYPE: {
        auto const& atr = incoming.payload.at_command_response;
        CL_REMARK(
            "ATResponse (%c%c): %s = %d %d",
            atr.command[0], atr.command[1], atr.status_text(),
            incoming.extra_size >= 1 ? atr.value[0] : -1,
            incoming.extra_size >= 2 ? atr.value[1] : -1);
        break;
      }
      default:
        CL_SPAM("Other payload type (0x%02x)", PayloadType::TYPE);
        break;
    }
  }

  if (xbee_radio->send_space_available() > 0) {
    status_screen->line_printf(2, "\f9Radio API ready");
  }

  if (millis() > next_status_update) {
    next_status_update += 500;
    OutgoingFrame out = {};
    out.type = out.payload.at_command.TYPE;
    out.payload.at_command = {1, {'S', 'Q'}};
    if (sizeof(out.payload.at_command) <= xbee_radio->send_space_available()) {
      xbee_radio->send_api_frame(out);
    }
  }
}

void setup() {
  while (!Serial.dtr() && millis() < 2000) delay(10);
  blub_station_init("BLUB XBee Test");
  next_status_update = millis();

  if (xbee_radio->raw_serial()) {
    status_screen->line_printf(1, "\f9Radio found");
  } else {
    status_screen->line_printf(1, "\f9Radio NOT found");
  }
}
