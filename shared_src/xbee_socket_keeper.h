// Keeps an XBee socket connected to a designated Internet host.

#pragma once

#include "xbee_api.h"

class XBeeSocketKeeper {
 public:
  virtual ~XBeeSocketKeeper() = default;
  virtual void on_incoming(XBeeAPI::Frame const&) = 0;
  virtual bool maybe_make_outgoing(int space, XBeeAPI::Frame*) = 0;

  virtual int socket() const = 0;  // -1 if not connected
  virtual void reconnect() = 0;    // Disconnect and reconnect
};

XBeeSocketKeeper* make_xbee_socket_keeper(
    char const* host, int port, XBeeAPI::SocketCreate::Protocol proto);
