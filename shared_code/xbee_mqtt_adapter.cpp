#include "xbee_mqtt_adapter.h"

#include <algorithm>

#include <Arduino.h>

#include "tagged_logging.h"
#include "xbee_mqtt_pal.h"

static const TaggedLoggingContext TL_CONTEXT("xbee_mqtt_adapter");

using namespace XBeeAPI;

extern "C" { static void on_message(void**, struct mqtt_response_publish*); }

class XBeeMQTTAdapterDef : public XBeeMQTTAdapter {
 public:
  XBeeMQTTAdapterDef(
      int tx_size, int rx_size,
      std::function<void(mqtt_response_publish const&)> const& on_message) {
    TL_NOTICE("Starting: tx=%d, rx=%d", tx_size, rx_size);
    tx_buf = new uint8_t[tx_buf_size = tx_size];
    rx_buf = new uint8_t[rx_buf_size = rx_size];
    message_callback = on_message;
    mqtt_init(&mqtt, this, tx_buf, tx_size, rx_buf, rx_size, ::on_message);
    mqtt.publish_response_callback_state = this;
  }

  virtual ~XBeeMQTTAdapterDef() override {
    TL_NOTICE("Destroying");
    delete[] tx_buf;
    delete[] rx_buf;
  }

  virtual bool incoming_to_outgoing(
      Frame const& incoming, int outgoing_space, Frame* outgoing) override {
    if (auto* stat = incoming.decode_as<SocketStatus>()) {
      if (stat->socket == socket && stat->status != SocketStatus::CONNECTED) {
        TL_PROBLEM("Socket error: %s", stat->status_text());
        socket = -1;  // XBee closes socket automatically on error
      }
    }

    if (auto* stat = incoming.decode_as<SocketCloseResponse>()) {
      if (stat->socket == socket && stat->status == SocketCloseResponse::OK) {
        TL_SPAM("Socket %d closed", socket);
        socket = -1;
      }
    }

    if (auto* stat = incoming.decode_as<TransmitStatus>()) {
      if (stat->frame_id == 'Q') {
        if (stat->status == 0) {
          TL_SPAM(">>>> XBee confirmed transmission");
        } else if (socket >= 0) {
          TL_PROBLEM("Transmit error: %s", stat->status_text());
          if (outgoing) {
            TL_SPAM("Closing socket %d", socket);
            auto* close = outgoing->setup_as<SocketClose>(0);
            close->frame_id = 'Q';  // Our signature
            close->socket = socket;
            socket = -1;
            return true;
          }
        }
      }
    }

    write_data = nullptr;
    write_filled = write_capacity = 0;
    if (outgoing && socket >= 0) {
      auto *send = outgoing->setup_as<SocketSend>(0);
      send->frame_id = 'Q';  // Our signature
      send->socket = socket;
      write_data = send->data;
      write_capacity = std::max(0, outgoing_space - wire_size_of<SocketSend>());
    }

    read_data = nullptr;
    read_consumed = read_received = 0;
    int receive_size;
    if (auto* receive = incoming.decode_as<SocketReceive>(&receive_size)) {
      if (receive->socket == socket) {
        TL_SPAM("<< %d bytes received from XBee", receive_size);
        read_data = receive->data;
        read_received = receive_size;
      }
    }

    mqtt_sync(&mqtt);

    if (read_consumed != read_received) {
      TL_PROBLEM(
          "%d/%d bytes unconsumed in frame",
          read_received, read_received - read_consumed);
    }

    if (write_filled > 0) {
      TL_SPAM(">> %d bytes sending to XBee", write_filled);
      outgoing->payload_size += write_filled;
      return true;
    } else {
      return false;
    }
  }

  virtual void use_socket(int socket) override {
    if (socket != this->socket) {
      TL_NOTICE("Init with socket #%d", socket);
      this->socket = socket;
      if (socket >= 0) {
        mqtt_reinit(&mqtt, this, tx_buf, tx_buf_size, rx_buf, rx_buf_size);
      }
    }
  }

  virtual int active_socket() const override { return socket; }

  virtual mqtt_client* client() override { return &mqtt; }

  void on_message(mqtt_response_publish const& publish) {
    TL_SPAM("Incoming: %.*s", publish.topic_name_size, publish.topic_name);
    if (message_callback) message_callback(publish);
  }

  ssize_t pal_sendall(void const* buf, size_t len) {
    if (socket < 0) return MQTT_ERROR_SOCKET_ERROR;
    int const send_size = std::min<int>(len, write_capacity - write_filled);
    if (send_size <= 0) return 0;
    memcpy(write_data + write_filled, buf, send_size);
    write_filled += send_size;
    TL_SPAM(">>> %d/%d bytes MQTT client => XBee", send_size, len);
    return send_size;
  }

  ssize_t pal_recvall(void* buf, size_t len) {
    if (socket < 0) return MQTT_ERROR_SOCKET_ERROR;
    int const recv_size = std::min<int>(len, read_received - read_consumed);
    if (recv_size <= 0) return 0;
    memcpy(buf, read_data + read_consumed, recv_size);
    read_consumed += recv_size;
    TL_SPAM("<<< %d/%d bytes MQTT client <= XBee", recv_size, len);
    return recv_size;
  }

 private:
  friend ssize_t mqtt_pal_sendall(
      mqtt_pal_socket_handle h, void const* buf, size_t len, int flags);
  friend ssize_t mqtt_pal_recvall(
      mqtt_pal_socket_handle h, void* buf, size_t len, int flags);

  uint8_t const* read_data = nullptr;
  uint8_t* write_data = nullptr;
  int read_consumed = 0, read_received = 0;
  int write_filled = 0, write_capacity = 0;

  mqtt_client mqtt = {};
  uint8_t* tx_buf = nullptr, *rx_buf = nullptr;
  int tx_buf_size = 0, rx_buf_size = 0;
  int socket = -1;

  std::function<void(mqtt_response_publish const&)> message_callback = nullptr;
};

XBeeMQTTAdapter* make_xbee_mqtt_adapter(
    int tx_size, int rx_size,
    std::function<void(mqtt_response_publish const&)> const& on_message) {
  return new XBeeMQTTAdapterDef(tx_size, rx_size, on_message);
}

extern "C" {
  mqtt_pal_time_t MQTT_PAL_TIME() {
    return static_cast<mqtt_pal_time_t>(millis() / 1000);
  }

  static void on_message(
      void** state, struct mqtt_response_publish* publish) {
    reinterpret_cast<XBeeMQTTAdapterDef*>(*state)->on_message(*publish);
  }

  ssize_t mqtt_pal_sendall(
      mqtt_pal_socket_handle h, void const* buf, size_t len, int flags) {
    return reinterpret_cast<XBeeMQTTAdapterDef*>(h)->pal_sendall(buf, len);
  }

  ssize_t mqtt_pal_recvall(
      mqtt_pal_socket_handle h, void* buf, size_t len, int flags) {
    return reinterpret_cast<XBeeMQTTAdapterDef*>(h)->pal_recvall(buf, len);
  }
}
