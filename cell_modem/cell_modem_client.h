// Driver for serial interface to a Nordic Serial Modem
// to check radio status and send/receive MQTT messages

#pragma once

namespace arduino { class HardwareSerial; }

struct CellModemStatus {
  char const* versions[3];  // serial modem, nordic SDK, customer (us)
};

class CellModemClient {
 public:
  virtual ~CellModemClient() = default;
  virtual CellModemStatus const& poll() = 0;
};

CellModemClient* make_cell_modem_client(
  arduino::HardwareSerial* serial,
  char const* mqtt_server
);
