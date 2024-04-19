// Put a Digi XBee into API mode and communicate with it

#pragma once

#include <stdint.h>

namespace arduino { class HardwareSerial; }

class XBeeRadio {
 public:
  static constexpr int MAX_API_PAYLOAD = 1600;  // Can hold max packet size

  struct ApiFrame {
    int type, size;  // Size is without header, type, or checksum
    uint8_t payload[MAX_API_PAYLOAD];
  };

  virtual ~XBeeRadio() = default;

  virtual bool poll_api(ApiFrame* out) = 0;
  virtual bool api_ready() = 0;
  virtual void send_api_frame(ApiFrame const&) = 0;
  virtual arduino::HardwareSerial* raw_serial() const = 0;
};

XBeeRadio* make_xbee_radio(arduino::HardwareSerial*);
