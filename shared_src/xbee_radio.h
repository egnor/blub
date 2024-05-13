// Driver for a Digi XBee to put it in API mode and send/receive frames.
// For details of actual operation, see xbee_api.h and xbee_status_monitor.h.

#pragma once

#include <stdint.h>

#include "xbee_api.h"

namespace arduino { class HardwareSerial; }

class XBeeRadio {
 public:
  virtual ~XBeeRadio() = default;
  virtual int outgoing_space() const = 0;
  virtual void add_outgoing(XBeeAPI::Frame const&) = 0;
  virtual bool poll_for_frame(XBeeAPI::Frame*) = 0;
  virtual arduino::HardwareSerial* raw_serial() const = 0;
};

XBeeRadio* make_xbee_radio(arduino::HardwareSerial*);
