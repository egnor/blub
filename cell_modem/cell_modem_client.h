// Driver for serial interface to a Nordic Serial Modem
// to check radio status and send/receive MQTT messages

#pragma once

#include <etl/string.h>
#include <memory>

namespace arduino { class HardwareSerial; }

struct CellModemStatus {
  bool running: 1, online: 1, roaming: 1, denied: 1, failed: 1;
  etl::string<32> hardware;
  etl::string<32> imeisv;
  etl::string<32> versions[4];  // baseband, nordic SDK, serial app, customer
};

class CellModemClient {
 public:
  virtual ~CellModemClient() = default;
  virtual CellModemStatus const& poll() = 0;
};

etl::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial,
  etl::string_view mqtt_server
);
