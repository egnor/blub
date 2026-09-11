// Driver for serial interface to a Nordic Serial Modem
// to check radio status and send/receive MQTT messages

#pragma once

#include <etl/span.h>
#include <etl/string.h>
#include <memory>

namespace arduino { class HardwareSerial; }

struct MqttServerConfig {
  etl::string_view cert, cert_sha256;
  etl::string_view host, user, password;
  int port;
};

struct MqttMessage {
  etl::string_view topic, payload;
};

struct CellModemStatus {
  // hardware identification
  etl::string<32> hardware;
  etl::array<etl::string<32>, 4> versions;  // baseband, SDK, app, customer
  etl::string<16> imeisv;

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

  // MQTT status
  bool mqtt_ready = false, mqtt_publish_busy = false;
};

class CellModemClient {
 public:
  virtual ~CellModemClient() = default;
  virtual CellModemStatus const& poll() = 0;

  // Requires poll().mqtt_ready && !poll().mqtt_publish_busy; strings must
  // remain valid until poll().mqtt_publish_busy is false again
  virtual void publish(MqttMessage const& msg) = 0;
};

etl::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial,
  MqttServerConfig const& mqtt_config,
  etl::span<etl::string_view const> mqtt_subs
);
