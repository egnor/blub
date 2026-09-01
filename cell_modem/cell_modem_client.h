// Driver for serial interface to a Nordic Serial Modem
// to check radio status and send/receive MQTT messages

#pragma once

#include <etl/string.h>
#include <memory>

namespace arduino { class HardwareSerial; }

struct CellModemStatus {
  // hardware identification
  etl::string<32> hardware;
  etl::string<32> versions[4];  // baseband, nordic SDK, serial app, customer
  etl::string<32> imeisv;

  // radio / registration status
  bool running = false, registered = false, roaming = false, failed = false;
  uint16_t op_mcc = 0, op_mnc = 0, cell_tac = 0, cell_phys_id = 0;
  uint32_t cell_id = 0;
  uint16_t radio_earfcn = 0;
  uint8_t radio_tech = 0, radio_band = 0;
  int16_t radio_rsrp = -0x8000, radio_snr = -0x8000;
  uint8_t reject_cause = 0;

  // packet network status
  bool ip_attached = false;
  uint32_t ip_addr = 0;
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
