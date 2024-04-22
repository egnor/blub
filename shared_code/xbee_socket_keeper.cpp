#include "xbee_socket_keeper.h"

#include <stdlib.h>
#include <string.h>

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
    TL_NOTICE("Starting: host=\"%s\", port=%d, proto=%d", host, port, proto);
  }

  ~XBeeSocketKeeperDef() {
    TL_NOTICE("Destroying");
    free(host);
  }

  virtual void on_incoming(XBeeAPI::Frame const& frame) override {
    if (auto* reply = frame.decode_as<SocketCreateResponse>()) {
      if (next_step == CREATE_WAIT && reply->frame_id == 'K') {
        if (reply->status == SocketCreateResponse::OK) {
          TL_SPAM("Socket created (%d)", reply->socket);
          socket_id = reply->socket;
          ++generation_count;
          next_step = CONNECT;
        } else {
          TL_PROBLEM("Socket create failed: %s", reply->status_text());
          socket_id = -1;
          next_step = IDLE;
        }
      }
      return;
    }

    if (auto* status = frame.decode_as<SocketStatus>()) {
      if (status->socket == socket_id) {
        if (status->status == SocketStatus::CONNECTED) {
          TL_SPAM("Socket connected (%d) OK", socket_id);
          next_step = IDLE;
        } else {
          TL_PROBLEM("Socket connect failed: %s", status->status_text());
          socket_id = -1;  // Non-CONNECTED SocketStatus means it's closed
          next_step = IDLE;
        }
      }
      return;
    }

    if (auto* status = frame.decode_as<SocketClose>()) {
      if (status->socket == socket_id) {
        TL_SPAM("Socket closed (%d)", socket_id);
        socket_id = -1;
        next_step = IDLE;
      }
      return;
    }

    if (auto* status = frame.decode_as<ModemStatus>()) {
      TL_SPAM("Modem status %s", status->status_text());
      network_up = (status->status == ModemStatus::REGISTERED);
      return;
    }

    int extra_size;
    if (auto* reply = frame.decode_as<ATCommandResponse>(&extra_size)) {
      if (!memcmp(reply->command, "AI", 2) &&
          reply->status == ATCommandResponse::OK &&
          extra_size == 1) {
        TL_SPAM("Association indication %d", reply->data[0]);
        network_up = (reply->status == 0x00);
      }
    }
  }

  virtual bool maybe_make_outgoing(int space, XBeeAPI::Frame* frame) override {
    switch (next_step) {
      case IDLE:
        if (socket_id < 0 && network_up &&
            space >= wire_size_of<SocketCreate>()) {
          auto* create = frame->setup_as<SocketCreate>();
          create->frame_id = 'K';
          create->protocol = proto;
          next_step = CREATE_WAIT;
          TL_SPAM("Sending SocketCreate (proto=%d)", proto);
          return true;
        }
        return false;

      case CONNECT:
        if (socket_id < 0) {
          next_step = IDLE;
        } else if (space >= wire_size_of<SocketConnect>(host_size)) {
          auto* connect = frame->setup_as<SocketConnect>();
          connect->frame_id = 0;  // No immedate ack needed, will get status
          connect->socket = socket_id;
          connect->dest_port = port;
          connect->address_type = SocketConnect::TEXT;
          memcpy(connect->address, host, host_size);
          next_step = CONNECT_WAIT;
          TL_SPAM(
              "Sending SocketConnect (socket=%d host=\"%s\" port=%d)",
              socket_id, host, port);
          return true;
        }
    }

    return false;
  }

  virtual int socket() const override {
    return next_step == IDLE ? socket_id : -1;
  }

  virtual int generation() const override { return generation_count; }

 private:
  char* host;
  int host_size;
  int port;
  XBeeAPI::SocketCreate::Protocol proto;

  bool network_up = false;
  int socket_id = -1;
  int generation_count = 0;

  enum { IDLE, CREATE_WAIT, CONNECT, CONNECT_WAIT } next_step = IDLE;
};

XBeeSocketKeeper* make_xbee_socket_keeper(
    char const* host, int port, XBeeAPI::SocketCreate::Protocol proto) {
  return new XBeeSocketKeeperDef(host, port, proto);
}
