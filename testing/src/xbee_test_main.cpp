#include <Arduino.h>

#include "blub_station.h"
#include "chatty_logging.h"
#include "little_status.h"
#include "xbee_radio.h"

static long next_status_update = 0;

void handle_payload(const XBeeRadio::ModemStatus& modem) {
  CL_REMARK("ModemStatus: %s", modem.status_text());
  status_screen->line_printf(3, "\f9Modem: %s", modem.status_text());
}

void handle_payload(const XBeeRadio::ATCommandResponse& atr) {
  CL_REMARK(
      "ATResponse (%c%c): %s = %d %d",
      atr.command[0], atr.command[1], atr.status_text(),
      atr.value[0], atr.value[1]);
}

template <typename PayloadType>
void handle_payload(PayloadType const& payload) {
  CL_SPAM("Other payload type (0x%02x)", PayloadType::TYPE);
}

void loop() {
  XBeeRadio::IncomingFrame frame;
  while (xbee_radio->poll_api(&frame)) {
    auto const handler = [](auto const& payload) { handle_payload(payload); };
    std::visit(handler, frame.payload);
  }

  if (xbee_radio->send_space_available() > 0) {
    status_screen->line_printf(2, "\f9Radio API ready");
  }

  if (millis() > next_status_update) {
    next_status_update += 500;
    XBeeRadio::ATCommand at_command{1, {'S', 'Q'}};
    if (sizeof(at_command) <= xbee_radio->send_space_available()) {
      xbee_radio->send_api_frame({at_command});
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
