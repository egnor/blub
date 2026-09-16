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

  // packet network status
  bool ip_attached = false;
  uint32_t ip_addr = 0;

  // MQTT status
  bool mqtt_ready = false;
  bool mqtt_publish_busy = false;
  bool mqtt_receive_ready = false;

  // stats counters
  int hard_resets = 0, radio_resets = 0, mqtt_resets = 0;
  int app_errors = 0, serial_errors = 0, timeout_errors = 0;
  int modem_errors = 0, mqtt_errors = 0;
};

class CellModemClient {
 public:
  virtual ~CellModemClient() = default;

  // Call frequently (every few ms); the reference stays valid
  virtual CellModemStatus const& poll() = 0;

  // Requires poll().mqtt_ready && !poll().mqtt_publish_busy; string views
  // must remain valid until poll().mqtt_publish_busy is false again.
  virtual void publish(MqttMessage) = 0;

  // Requires poll().mqtt_receive_ready; views are valid until the next poll().
  virtual MqttMessage receive() = 0;
};

// serial - UART I/O to modem (caller must .begin())
// enable_pin - if >= 0, driven LOW to reset the modem for startup & recovery
// mqtt_config - MQTT server to use, string views must remain valid
// mqtt_subs - MQTT topic subscriptions, span & views must remain valid
etl::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial, int enable_pin,
  MqttServerConfig const& mqtt_config,
  etl::span<etl::string_view const> mqtt_subs
);
