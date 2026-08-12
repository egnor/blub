// Driver for serial interface to a Nordic Serial Modem
// to check radio status and send/receive MQTT messages

#pragma once

#include <etl/string.h>
#include <memory>

namespace arduino { class HardwareSerial; }

struct CellModemStatus {
  etl::string<32> hardware[2];  // manufacturer, model
  etl::string<32> serials[2];  // imeisv, 2did
  etl::string<32> versions[4];  // baseband, nordic SDK, serial app, customer
};

class CellModemClient {
 public:
  virtual ~CellModemClient() = default;
  virtual CellModemStatus const& poll() = 0;
};

std::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial,
  etl::string_view mqtt_server
);
