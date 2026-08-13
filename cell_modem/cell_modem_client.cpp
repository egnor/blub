#include "cell_modem_client.h"

#include <Arduino.h>
#include <memory>
#include <ok_logging.h>

static const OkLoggingContext OK_CONTEXT("cell_modem_client");

class CellModemClientDef : public CellModemClient {
 public:
  CellModemClientDef(HardwareSerial* s, etl::string_view mqtt)
    : serial(s), mqtt_server(mqtt) {}

  CellModemStatus const& poll() override {
    return status;
  }

 private:
  HardwareSerial* const serial;
  etl::string<128> const mqtt_server;

  CellModemStatus status;
};

std::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial, etl::string_view mqtt_server
) {
  OK_FATAL_IF(serial == nullptr);
  return std::make_unique<CellModemClientDef>(serial, mqtt_server);
}
