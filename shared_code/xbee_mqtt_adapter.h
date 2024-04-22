// Provides network access for the MQTT-C library using the XBee radio.

#pragma once

#include <functional>

#include <mqtt.h>

#include "xbee_api.h"

class XBeeMQTTAdapter {
 public:
  virtual ~XBeeMQTTAdapter() {}

  virtual bool process_frame(
      XBeeAPI::Frame const& incoming,
      int outgoing_space, XBeeAPI::Frame* outgoing) = 0;

  virtual void set_socket(int socket) = 0;

  virtual mqtt_client* client() = 0;
};

XBeeMQTTAdapter* make_xbee_mqtt_adapter(
    int send_buffer_size, int receive_buffer_size,
    std::function<void(mqtt_response_publish const&)> const& message_callback);
