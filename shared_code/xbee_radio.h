// Put a Digi XBee into API mode and communicate with it

#pragma once

#include <stdint.h>

namespace arduino { class HardwareSerial; }

class XBeeRadio {
 public:
  static constexpr int MAX_API_PAYLOAD = 1600;  // Can hold max packet size

  struct ApiFrame {
    uint8_t type;
    int size;
    uint8_t const* payload;  // Not including header or type
  };

  virtual ~XBeeRadio() = default;

  virtual ApiFrame const* poll_api() = 0;
  virtual bool api_ready() = 0;
  virtual void send_api_frame(ApiFrame const&) = 0;
  virtual arduino::HardwareSerial* raw_serial() const = 0;
};

XBeeRadio* make_xbee_radio(arduino::HardwareSerial*);
