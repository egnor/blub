#include "xbee_mqtt_adapter.h"

#include "tagged_logging.h"
#include "xbee_mqtt_pal.h"

TaggedLoggingContext TL_CONTEXT("xbee_mqtt_adapter");

using namespace XBeeAPI;

extern "C" { static void on_message(void**, struct mqtt_response_publish *); }

class XBeeMQTTAdapterDef : public XBeeMQTTAdapter {
 public:
  XBeeMQTTAdapterDef(int tx_size, int rx_size) {
    tx_buf = new uint8_t[tx_buf_size = tx_size];
    rx_buf = new uint8_t[rx_buf_size = rx_size];
    mqtt_init(&mqtt, this, tx_buf, tx_size, rx_buf, rx_size, on_message);
    mqtt.publish_response_callback_state = this;
  }

  virtual ~XBeeMQTTAdapterDef() override {
    delete[] tx_buf;
    delete[] rx_buf;
  }

  virtual bool handle_and_maybe_emit_frame(
      Frame const& handle, int available, Frame* emit) override {
    if (server_to_close.dest_port != 0) {
      auto *close = emit->setup_as<TransmitIP>(0);
      *close = server_to_close;
      close->frame_id = 0;
      close->options = TransmitIP::CLOSE_SOCKET;
      server_to_close = {};
      return true;  // Ignore incoming data until we reopen the socket
    }

    write_data = nullptr;
    write_filled = write_capacity = 0;
    if (emit && server.dest_port != 0) {
      auto *transmit = emit->setup_as<TransmitIP>(0);
      *transmit = server;
      transmit->frame_id = 0;
      write_data = transmit->data;
      write_capacity = available - wire_size_of<TransmitIP>();
    }

    read_data = nullptr;
    read_consumed = read_received = 0;
    int receive_size;
    if (auto* receive = handle.decode_as<ReceiveIP>(&receive_size)) {
      if (!memcmp(receive->source_ip, server.dest_ip, 4) &&
          receive->source_port == server.dest_port &&
          receive->protocol == server.protocol) {
        read_data = receive->data;
        read_received = receive_size;
      }
    }

    mqtt_sync(&mqtt);

    if (read_consumed != read_received) {
      TL_PROBLEM(
          "%d/%d bytes leftover in frame",
          read_received, read_received - read_consumed);
    }

    if (write_filled > 0) emit->payload_size += write_filled;
    write_data = nullptr;
    write_filled = write_capacity = 0;

    return (emit->payload_size > wire_size_of<TransmitIP>());
  }

  virtual void connect_network(
      TransmitIP const& server,
      std::function<void(mqtt_response_publish const&)> callback) override {
    server_to_close = this->server;
    this->server = server;
    this->message_callback = callback;
    mqtt_reinit(&mqtt, this, tx_buf, tx_buf_size, rx_buf, rx_buf_size);
  }

  virtual mqtt_client* client() override { return &mqtt; }

 private:
  friend void on_message(void**, struct mqtt_response_publish *);
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

  TransmitIP server_to_close = {};
  TransmitIP server = {};
  std::function<void(mqtt_response_publish const&)> message_callback = nullptr;
};

XBeeMQTTAdapter* make_xbee_mqtt_adapter(int tx_size, int rx_size) {
  return new XBeeMQTTAdapterDef(tx_size, rx_size);
}

extern "C"  {
  static void on_message(
      void** state, struct mqtt_response_publish *publish) {
    auto* self = reinterpret_cast<XBeeMQTTAdapterDef*>(*state);
    if (self->message_callback) self->message_callback(*publish);
  }

  ssize_t mqtt_pal_sendall(
      mqtt_pal_socket_handle h, void const* buf, size_t len, int flags) {
    auto* self = reinterpret_cast<XBeeMQTTAdapterDef*>(h);
    return 0;
  }

  ssize_t mqtt_pal_recvall(
      mqtt_pal_socket_handle h, void* buf, size_t len, int flags) {
    auto* self = reinterpret_cast<XBeeMQTTAdapterDef*>(h);
    return 0;
  }
}
