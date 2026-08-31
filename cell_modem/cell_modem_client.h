// Driver for serial interface to a Nordic Serial Modem
// to check radio status and send/receive MQTT messages

#pragma once

#include <etl/string.h>
#include <memory>

namespace arduino { class HardwareSerial; }

struct CellModemStatus {
  etl::string<32> hardware;
  etl::string<32> imeisv;
  etl::string<32> versions[4];  // baseband, nordic SDK, serial app, customer
                                //
  bool running, online, roaming, failed;
  uint16_t op_mcc, op_mnc, cell_tac, cell_phys_id;
  uint32_t cell_id;
  uint16_t radio_earfcn;
  uint8_t radio_tech, radio_band, radio_rsrp, radio_snr;

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
