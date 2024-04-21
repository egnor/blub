// Put a Digi XBee into API mode and communicate with it

#pragma once

#include <stdint.h>

#include "xbee_api.h"

namespace arduino { class HardwareSerial; }

class XBeeRadio {
 public:
  virtual ~XBeeRadio() = default;
  virtual bool poll_api(XBeeApi::Frame*) = 0;
  virtual void send_api_frame(XBeeApi::Frame const&) = 0;
  virtual int send_available() const = 0;
  virtual arduino::HardwareSerial* raw_serial() const = 0;
};

XBeeRadio* make_xbee_radio(arduino::HardwareSerial*);
