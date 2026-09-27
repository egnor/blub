// TODO: report IMEI/IMSI/ICCID (first, add the latter two to CellModemStatus)
// TODO: report RP2040's reset reason

#pragma GCC diagnostic error "-Wformat=2"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_INA228.h>
#include <cmath>
#include <etl/array.h>
#include <etl/chrono.h>
#include <etl/optional.h>
#include <etl/string.h>
#include <ok_little_layout.h>
#include <ok_logging.h>
#include <ok_micro_dock.h>
#include <tusb.h>

#include <blub_clock_util.h>
#include <blub_mqtt_config.h>
#include <cell_modem_client.h>

using namespace etl::chrono;
using namespace etl::chrono_literals;

static const OkLoggingContext OK_CONTEXT("base_station");

struct meter {
  int i2c_address;
  etl::string<8> name;
  etl::optional<Adafruit_INA228> driver;
};

static etl::array<meter, 4> meters{{
  {INA228_I2CADDR_DEFAULT, "Out"},
  {INA228_I2CADDR_DEFAULT + 1, "PPT"},
  {INA228_I2CADDR_DEFAULT + 4, "Panel"},
  {INA228_I2CADDR_DEFAULT + 5, "Local"},
}};

static etl::unique_ptr<CellModemClient> cell_modem;
static etl::string<512> pub_buffer;

static steady_clock::time_point last_loop_time = {};
static steady_clock::time_point next_mqtt_time = {};
static steady_clock::time_point next_screen_time = {};

static void update_screen() {
  int ln = 0;
  OK_NOTE("\n☀️ Power Station");
  for (auto& meter : meters) {
    if (meter.driver) {
      auto const V = meter.driver->readBusVoltage() * 1e-3f;
      auto const mA = meter.driver->readCurrent();
      auto const mW = meter.driver->readPower();
      auto const J = meter.driver->readEnergy();
      auto const C = meter.driver->readDieTemp();
      ok_dock_layout->line_printf(
        ln++, "\f6%s\t%.1fV\t%.1fmA",
        meter.name.c_str(), V, mA
      );
      OK_NOTE(
        "%s: %.1fV %.1fmA %.0fmW %.3fJ %.1fC",
        meter.name.c_str(), V, mA, mW, J, C
      );
    } else {
      ok_dock_layout->line_printf(ln++, "\f6s\t-\t-", meter.name.c_str());
      OK_NOTE("%s: missing at startup", meter.name.c_str());
    }
  }

  if (!cell_modem) {
    ok_dock_layout->line_printf(ln++, "\f6No cell modem");
  } else {
    auto const& status = cell_modem->status();
    char const* ip_stat =
      !status.ip_attached ? "" : !status.mqtt_ready ? " IP" :
        status.mqtt_publish_busy ? " MQ*" : " MQ";
    char const* usb_stat =
      !tud_connected() ? "" : tud_suspended() ? " US" :
        !tud_mounted() ? " U" : Serial.dtr() ? " UMS" : " UM";

    if (!status.running) {
      ok_dock_layout->line_printf(ln++, "\f6Rad-off%s", usb_stat);
      OK_NOTE("Cell: Off");
    } else if (!status.registered) {
      ok_dock_layout->line_printf(ln++, "\f6Search%s", usb_stat);
      OK_NOTE("Cell: Searching");
    } else {
      ok_dock_layout->line_printf(
        ln++, "\f6%+d %+ddB%s%s",
        status.radio_rsrp, status.radio_snr, ip_stat, usb_stat
      );
      if (status.ip_attached) {
        uint8_t a[4];
        for (int i = 0; i < 4; ++i) a[i] = status.ip_addr >> (8 * (3 - i));
        OK_NOTE(
          "Cell: %+ddBm/%+ddB %d.%d.%d.%d%s",
          status.radio_rsrp, status.radio_snr, a[0], a[1], a[2], a[3], ip_stat
        );
      } else {
        OK_NOTE("Cell: %+ddBm/%+ddB !IP", status.radio_rsrp, status.radio_snr);
      }
    }

    OK_NOTE(
      "USB: %s%s%s%s",
      tud_connected() ? "Conn" : "Disconn",
      tud_suspended() ? " SUSP" : "",
      tud_mounted() ? " mount" : "",
      Serial.dtr() ? " serial+DTR" : ""
    );
  }
}

static void update_mqtt() {
  if (!cell_modem) return;
  const auto& status = cell_modem->status();
  if (!status.mqtt_ready || status.mqtt_publish_busy) return;

  JsonDocument doc;
  doc["uptime"] = std::round(raw_count<secd>(steady_clock::now()) * 10) * 0.1;

  auto json_meters = doc["power"];
  for (auto& meter : meters) {
    if (!meter.driver) continue;
    auto json_meter = json_meters[meter.name];
    json_meter["V"] = std::round(meter.driver->readBusVoltage()) * 1e-3;
    json_meter["A"] = std::round(meter.driver->readCurrent()) * 1e-3;
    json_meter["J"] = std::round(meter.driver->readEnergy() * 10) * 0.1;
    json_meter["C"] = std::round(meter.driver->readDieTemp() * 10) * 0.1;
  }

  auto json_cell = doc["cell"];
  json_cell["op"] = status.op_mcc * 1000 + status.op_mnc;
  json_cell["tech"] = status.radio_tech;
  json_cell["sig"] = status.radio_rsrp;
  json_cell["snr"] = status.radio_snr;
  auto json_cell_counts = json_cell["counts"];
  copyArray(status.counters.data(), status.counters.size(), json_cell_counts);

  auto json_usb = doc["usb"];
  if (tud_connected()) json_usb["con"] = true;
  if (tud_mounted()) json_usb["mnt"] = true;
  if (tud_suspended()) json_usb["sus"] = true;
  if (Serial.dtr()) json_usb["dtr"] = true;

  pub_buffer.clear();
  auto const len = serializeJson(doc, pub_buffer.data(), pub_buffer.max_size());
  pub_buffer.uninitialized_resize(len);
  OK_NOTE("💬 MQTT publish (%db)", pub_buffer.size());
  cell_modem->publish({"blub/base_station", pub_buffer});
}

void loop() {
  rp2040.wdt_reset();
  auto const loop_time = steady_clock::now();
  if (last_loop_time != steady_clock::time_point{}) {
    auto const delay = loop_time - last_loop_time;
    if (delay > 10_ms) OK_ERROR("Loop took %lldms", raw_count<millis64>(delay));
  }

  cell_modem->poll();
  while (cell_modem->status().mqtt_receive_ready) cell_modem->receive();

  if (loop_time >= next_screen_time) {
    next_screen_time = loop_time + 500_ms;
    update_screen();
  }

  if (loop_time >= next_mqtt_time) {
    next_mqtt_time += 10_s;
    update_mqtt();
  }

  delay(1);
}

void setup() {
  OkLoggingSerialOptions ser_opt = {.connect_wait_millis = 0};
  ok_serial_begin(ser_opt);
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f8Power Station");

  for (auto& meter : meters) {
    meter.driver.emplace();
    if (meter.driver->begin(meter.i2c_address)) {
      OK_NOTE("\"%s\" meter @ 0x%x", meter.name.c_str(), meter.i2c_address);
      meter.driver->setShunt(0.015, 10.0);
      meter.driver->setCurrentConversionTime(INA228_TIME_4120_us);
    } else {
      OK_ERROR("No \"%s\" meter @ 0x%x", meter.name.c_str(), meter.i2c_address);
      meter.driver.reset();
    }
  }

  Serial1.setFIFOSize(2048);
  Serial1.begin(115200);
  cell_modem = make_cell_modem_client(&Serial1, 25, blub_mqtt_config, {});
  next_mqtt_time = next_screen_time = steady_clock::now();
  rp2040.wdt_begin(5000);  // 5 second on-chip hardware watchdog (pet in loop())
}
