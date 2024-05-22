#include <array>
#include <cmath>
#include <optional>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_INA228.h>

#include "src/blub_station.h"
#include "src/little_status.h"
#include "src/tagged_logging.h"
#include "src/xbee_api.h"
#include "src/xbee_mqtt_adapter.h"
#include "src/xbee_radio.h"
#include "src/xbee_socket_keeper.h"
#include "src/xbee_status_monitor.h"

static const TaggedLoggingContext TL_CONTEXT("power_station");

static XBeeStatusMonitor* xbee_monitor = nullptr;
static XBeeSocketKeeper* socket_keeper = nullptr;
static XBeeMQTTAdapter* mqtt = nullptr;

static long last_socket_millis = 0;
static long next_mqtt_millis = 0;
static long next_screen_millis = 0;
static long mqtt_connect_millis = 0;

struct meter {
  int i2c_address;
  char const* name;
  std::optional<Adafruit_INA228> driver;
};

std::array<meter, 3> meters{{
  {INA228_I2CADDR_DEFAULT, "Load"},
  {INA228_I2CADDR_DEFAULT + 1, "PPT"},
  {INA228_I2CADDR_DEFAULT + 4, "Panel"},
}};

static void on_mqtt_incoming(mqtt_response_publish const& message) {
  TL_NOTICE("MQTT incoming: %.*s", message.topic_name_size, message.topic_name);
}

static void poll_xbee() {
  static XBeeAPI::Frame in, out;
  while (xbee_radio->poll_for_frame(&in)) {
    xbee_monitor->on_incoming(in);
    socket_keeper->on_incoming(in);
    if (mqtt->incoming_to_outgoing(in, xbee_radio->outgoing_space(), &out))
      xbee_radio->add_outgoing(out);
  }

  in.clear();
  while (mqtt->incoming_to_outgoing(in, xbee_radio->outgoing_space(), &out))
    xbee_radio->add_outgoing(out);
  while (xbee_monitor->maybe_make_outgoing(xbee_radio->outgoing_space(), &out))
    xbee_radio->add_outgoing(out);
  while (socket_keeper->maybe_make_outgoing(xbee_radio->outgoing_space(), &out))
    xbee_radio->add_outgoing(out);

  if (socket_keeper->socket() != mqtt->active_socket()) {
    mqtt->use_socket(socket_keeper->socket());
    mqtt_connect(
        mqtt->client(), "BLUB Power Station",
        nullptr, nullptr, 0,
        "blub", "blub",
        MQTT_CONNECT_CLEAN_SESSION, 400);
    mqtt_connect_millis = millis();
  }

  if (mqtt->active_socket() >= 0 && mqtt->client()->error != MQTT_OK) {
    TL_PROBLEM("MQTT error: %s", mqtt_error_str(mqtt->client()->error));
    socket_keeper->reconnect();
  }

  if (socket_keeper->socket() >= 0) {
    last_socket_millis = millis();
  } else if ((millis() - last_socket_millis) > 10 * 60 * 1000) {
    TL_PROBLEM("No socket for 10 minutes, rebooting");
    status_screen->line_printf(0, "\f9\bNO SOCKET - REBOOTING");
    delay(1000);
    rp2040.reboot();
  }
}

static void update_screen() {
  int ln = 0;
  char line[80] = "";
  for (auto& meter : meters) {
    if (meter.driver) {
      sprintf(line + strlen(line), "\t\f9\b%s\b", meter.name);
      TL_NOTICE(
          "%s: %.1fV %.1fmA [%.3fmVs] %.0fmW %.3fJ %.1fC", meter.name,
          meter.driver->readBusVoltage() * 1e-3f,
          meter.driver->readCurrent(),
          meter.driver->readShuntVoltage(),
          meter.driver->readPower(),
          meter.driver->readEnergy(),
          meter.driver->readDieTemp());
    } else {
      TL_NOTICE("%s: not detected at startup", meter.name);
    }
  }
  status_screen->line_printf(ln++, "%s", line + 1);

  strcpy(line, "");
  for (auto& meter : meters) {
    if (!meter.driver) continue;
    float sign = meter.driver->readCurrent() < 0 ? -1 : 1;
    float power = meter.driver->readPower() * 1e-3f * sign;
    sprintf(line + strlen(line), "\t\f12%.2fW", power);
  }
  status_screen->line_printf(ln++, "%s", line + 1);

  strcpy(line, "");
  for (auto& meter : meters) {
    if (!meter.driver) continue;
    auto const milliamps = meter.driver->readCurrent();
    if (milliamps >= -999 && milliamps <= 999) {
      sprintf(line + strlen(line), "\t\f8%.1f\f6V\2\f8%.0f\f6mA",
              meter.driver->readBusVoltage() * 1e-3, milliamps);
    } else {
      sprintf(line + strlen(line), "\t\f8%.1f\f6V\2\f8%.1f\f6A",
              meter.driver->readBusVoltage() * 1e-3, milliamps * 1e-3);
    }
  }
  status_screen->line_printf(ln++, "%s", line + 1);
  status_screen->line_printf(ln++, "\f3 ");

  auto const& xst = xbee_monitor->status();
  if (!xst.hardware_ver) {
    status_screen->line_printf(ln++, "\f9No XBee status");
  } else {
    status_screen->line_printf(
        ln++, "\f9\bCell\b %s %s %s",
        xst.network_operator,
        xst.technology == XBeeStatusMonitor::UNKNOWN_TECH
            ? "" : xst.technology_text(),
        xst.operating_apn);

    sprintf(line, "\f9");
    if (xst.received_power != 0 || xst.received_quality != 0) {
      sprintf(
          line + strlen(line), "P%.1f Q%.1f ",
          xst.received_power, xst.received_quality);
    }
    if (xst.assoc_status == XBeeStatusMonitor::CONNECTED) {
      auto const& ip = xst.ip_address;
      sprintf(line + strlen(line), "%d.%d.%d.%d ", ip[0], ip[1], ip[2], ip[3]);
    } else {
      sprintf(line + strlen(line), "%s ", xst.assoc_text());
    }
    status_screen->line_printf(ln++, "%s", line);
  }
  status_screen->line_printf(ln++, "\f3 ");

  if (mqtt->active_socket() < 0) {
    status_screen->line_printf(ln++, "\f9\bMQTT\b socket not connected");
  } else if (mqtt->client()->error != MQTT_OK) {
    char const* error = mqtt_error_str(mqtt->client()->error);
    if (strncmp(error, "MQTT_", 5)) error += 5;
    status_screen->line_printf(ln++, "\f9\bMQTT\b %s", error);
  } else if (mqtt->client()->typical_response_time < 0) {
    status_screen->line_printf(ln++, "\f9\bMQTT\b protocol connecting...");
  } else {
    auto const typ = mqtt->client()->typical_response_time;
    status_screen->line_printf(ln++, "\f9\bMQTT\b OK ping=%.2fs", typ);
  }
}

static void update_mqtt() {
  JsonDocument doc;
  doc["uptime"] = std::round(millis() * 1e-2f) * 0.1;

  auto json_meters = doc["power"];
  for (auto& meter : meters) {
    if (!meter.driver) continue;
    auto json_meter = json_meters[meter.name];
    json_meter["V"] = std::round(meter.driver->readBusVoltage()) * 1e-3;
    json_meter["A"] = std::round(meter.driver->readCurrent()) * 1e-3;
    json_meter["J"] = std::round(meter.driver->readEnergy() * 10) * 0.1;
    json_meter["C"] = std::round(meter.driver->readDieTemp() * 10) * 0.1;
  }

  auto const& xst = xbee_monitor->status();
  auto json_cell = doc["cell_radio"];
  json_cell["assoc"] = xst.assoc_text();
  json_cell["op"] = xst.network_operator;
  json_cell["APN"] = xst.operating_apn;
  json_cell["tech"] = xst.technology_text();
  json_cell["RSRP"] = xst.received_power;
  json_cell["RSRQ"] = xst.received_quality;

  char message[512];
  auto const size = serializeJson(doc, message, sizeof(message) - 1);
  mqtt_publish(
      mqtt->client(), "blub/power_station",
      message, size, MQTT_PUBLISH_QOS_1);
}

void loop() {
  rp2040.wdt_reset();
  poll_xbee();

  int const now = millis();
  if (now >= next_screen_millis) {
    next_screen_millis += 500;
    update_screen();
  }

  if (now >= next_mqtt_millis) {
    next_mqtt_millis += 30000;
    update_mqtt();
  }

  // Reboot before millis rollover
  if (now > 0x7FFFFFFF - 1000) {
    TL_NOTICE("Rebooting before millis rollover!");
    status_screen->line_printf(0, "\f9\bROLLOVER - REBOOTING");
    delay(1000);
    rp2040.reboot();
  }

  delay(1);
}

void setup() {
  blub_station_init("POWER STA. INIT");

  for (auto& meter : meters) {
    meter.driver.emplace();
    if (meter.driver->begin(meter.i2c_address)) {
      TL_NOTICE("\"%s\" meter at 0x%x", meter.name, meter.i2c_address);
      meter.driver->setShunt(0.015, 10.0);
      meter.driver->setCurrentConversionTime(INA228_TIME_4120_us);
    } else {
      TL_PROBLEM("No \"%s\" meter at 0x%x", meter.name, meter.i2c_address);
      meter.driver.reset();
    }
  }

  if (!xbee_radio->raw_serial()) {
    TL_PROBLEM("No XBee found, rebooting");
    status_screen->line_printf(0, "\f9\bNO XBEE - REBOOTING");
    delay(1000);
    rp2040.reboot();
  }

  xbee_monitor = make_xbee_status_monitor();
  socket_keeper = make_xbee_socket_keeper(
      "egnor-2020.ofb.net", 1883, XBeeAPI::SocketCreate::Protocol::TCP);
  mqtt = make_xbee_mqtt_adapter(512, 512, on_mqtt_incoming);

  next_mqtt_millis = next_screen_millis = last_socket_millis = millis();
  rp2040.wdt_begin(5000);  // 5 second on-chip hardware watchdog (pet in loop())
}
