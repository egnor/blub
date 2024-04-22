// Driver for a Digi XBee to put it in API mode and send/receive frames.
// For details of actual operation, see xbee_api.h and xbee_monitor.h.

#pragma once

#include <stdint.h>

#include "xbee_api.h"

namespace arduino { class HardwareSerial; }

class XBeeRadio {
 public:
  virtual ~XBeeRadio() = default;
  virtual int available_for_send() const = 0;
  virtual void send_frame(XBeeAPI::Frame const&) = 0;
  virtual bool maybe_receive_frame(XBeeAPI::Frame*) = 0;
  virtual arduino::HardwareSerial* raw_serial() const = 0;
};

XBeeRadio* make_xbee_radio(arduino::HardwareSerial*);
