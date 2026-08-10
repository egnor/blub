// Driver for serial interface to a Nordic Serial Modem
// to check radio status and send/receive MQTT messages

#pragma once

namespace arduino { class HardwareSerial; }

class CellModemClient {
 public:
  virtual ~CellModemClient() = default;
};

CellModemClient* make_cell_modem_client(arduino::HardwareSerial* serial);
