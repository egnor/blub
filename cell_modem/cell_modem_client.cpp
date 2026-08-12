#include "cell_modem_client.h"

#include <memory>

#include <Arduino.h>
#include <ok_logging.h>

static const OkLoggingContext OK_CONTEXT("cell_modem_client");

class CellModemClientDef : public CellModemClient {
 public:
  CellModemClientDef(HardwareSerial* s, etl::string_view mqtt)
    : serial(s), mqtt_server(mqtt) {}

 private:
  HardwareSerial* const serial;
  etl::string<128> const mqtt_server;

  CellModemStatus status;
};

std::unique_ptr<CellModemClient> make_cell_modem_client(
  arduino::HardwareSerial* serial, etl::string_view mqtt_server
) {
  OK_FATAL_IF(series == nullptr);
  return std::make_unique<CellModemClient>(serial, mqtt_server);
}
