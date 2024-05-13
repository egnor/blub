#include "xbee_socket_keeper.h"

#include <stdlib.h>
#include <string.h>

#include <Arduino.h>

#include "tagged_logging.h"

static const TaggedLoggingContext TL_CONTEXT("xbee_socket_keeper");

using namespace XBeeAPI;

class XBeeSocketKeeperDef : public XBeeSocketKeeper {
 public:
  XBeeSocketKeeperDef(
      char const* host, int port, XBeeAPI::SocketCreate::Protocol proto) {
    this->host = strdup(host);
    this->host_size = strlen(host);
    this->port = port;
    this->proto = proto;
    TL_NOTICE("Starting: %s:%d (proto=%d)", host, port, proto);
  }

  ~XBeeSocketKeeperDef() {
    TL_NOTICE("Destroying");
    free(host);
  }

  virtual void on_incoming(XBeeAPI::Frame const& frame) override {
    if (auto* reply = frame.decode_as<SocketCreateResponse>()) {
      if (next_step == CREATE_WAIT && reply->frame_id == 'K') {
        if (reply->status == SocketCreateResponse::OK) {
          TL_SPAM("Socket #%d created", reply->socket);
          socket_id = reply->socket;
          next_step = CONNECT;
        } else {
          TL_PROBLEM("Socket creation failed: %s", reply->status_text());
          socket_id = -1;
          next_step = READY;
        }
      }
      return;
    }

    if (auto* reply = frame.decode_as<SocketConnectResponse>()) {
      if (reply->socket == socket_id) {
        if (reply->status == SocketConnectResponse::STARTED) {
          TL_SPAM("Socket #%d connecting", socket_id);
          // This is temporary, wait for CONNECTED SocketStatus
        } else {
          TL_PROBLEM("Connection aborted: %s", reply->status_text());
          next_step = CLOSE;
        }
      }
    }

    if (auto* status = frame.decode_as<SocketStatus>()) {
      if (status->socket == socket_id) {
        if (status->status == SocketStatus::CONNECTED) {
          TL_NOTICE("Socket #%d connected OK", socket_id);
          next_step = READY;
        } else {
          TL_PROBLEM("Connection failed: %s", status->status_text());
          socket_id = -1;  // Non-CONNECTED SocketStatus means it's closed
          next_step = READY;
        }
      }
      return;
    }

    if (auto* reply = frame.decode_as<SocketCloseResponse>()) {
      if (reply->socket == socket_id &&
          reply->status == SocketCloseResponse::OK) {
        TL_NOTICE("Socket #%d closed", socket_id);
        socket_id = -1;
        next_step = READY;
      }
      return;
    }

    if (auto* status = frame.decode_as<ModemStatus>()) {
      network_up = (status->status == ModemStatus::REGISTERED);
      TL_SPAM(
          "Modem status %s (network %s)", status->status_text(),
          network_up ? "UP" : "DOWN");
      return;
    }

    int extra_size;
    if (auto* reply = frame.decode_as<ATCommandResponse>(&extra_size)) {
      if (!memcmp(reply->command, "AI", 2) &&
          reply->status == ATCommandResponse::OK &&
          extra_size == 1) {
        network_up = (reply->data[0] == 0x00);
        TL_SPAM(
            "Assoc status 0x%02x (network %s)", reply->data[0],
            network_up ? "UP" : "DOWN");
      }
    }
  }

  virtual bool maybe_make_outgoing(int space, XBeeAPI::Frame* frame) override {
    switch (next_step) {
      case READY:
        if (socket_id < 0 && network_up &&
            space >= wire_size_of<SocketCreate>()) {
          auto const now = millis();
          if (now > next_retry_millis) {
            next_retry_millis = now + 3000;
            auto* create = frame->setup_as<SocketCreate>();
            create->frame_id = 'K';
            create->protocol = proto;
            next_step = CREATE_WAIT;
            TL_SPAM("Creating socket proto=%d", proto);
            return true;
          }
        }
        break;

      case CONNECT:
        if (socket_id < 0) {
          next_step = READY;
        } else if (space >= wire_size_of<SocketConnect>(host_size)) {
          auto* connect = frame->setup_as<SocketConnect>(host_size);
          connect->frame_id = 1;
          connect->socket = socket_id;
          connect->dest_port = port;
          connect->address_type = SocketConnect::TEXT;
          memcpy(connect->address, host, host_size);
          next_step = CONNECT_WAIT;
          TL_NOTICE(
              "Connecting #%d to %.*s:%d",
              socket_id, host_size, connect->address, port);
          return true;
        }
        break;

      case CLOSE:
        if (socket_id < 0) {
          next_step = READY;
        } else if (space >= wire_size_of<SocketClose>()) {
          auto* close = frame->setup_as<SocketClose>();
          close->frame_id = 1;
          close->socket = socket_id;
          next_step = CLOSE_WAIT;
          TL_NOTICE("Closing socket #%d", socket_id);
          return true;
        }
        break;
    }

    return false;
  }

  virtual int socket() const override {
    return next_step == READY ? socket_id : -1;
  }

  virtual void reconnect() override {
    if (next_step == READY && socket_id >= 0) {
      next_step = CLOSE;
    }
  }

 private:
  char* host;
  int host_size;
  int port;
  XBeeAPI::SocketCreate::Protocol proto;

  bool network_up = false;
  long next_retry_millis = 0;
  int socket_id = -1;

  enum {
    READY, CREATE_WAIT, CONNECT, CONNECT_WAIT, CLOSE, CLOSE_WAIT
  } next_step = READY;
};

XBeeSocketKeeper* make_xbee_socket_keeper(
    char const* host, int port, XBeeAPI::SocketCreate::Protocol proto) {
  return new XBeeSocketKeeperDef(host, port, proto);
}
