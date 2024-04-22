// Provides network access for the MQTT-C library using the XBee radio.

#pragma once

#include <functional>

#include <mqtt.h>

#include "xbee_api.h"

class XBeeMQTTAdapter {
 public:
  virtual ~XBeeMQTTAdapter() {}

  virtual bool handle_and_maybe_emit_frame(
      XBeeAPI::Frame const& handle,
      int available, XBeeAPI::Frame* emit) = 0;

  virtual void start_mqtt(
      XBeeAPI::TransmitIP const& server,
      std::function<void(mqtt_response_publish const&)> message_callback) = 0;

  virtual mqtt_client* client() = 0;
};

XBeeMQTTAdapter* make_xbee_mqtt_adapter(
    int send_buffer_size, int receive_buffer_size);
