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

#include <blub_clock_util.h>
#include <blub_mqtt_config.h>
#include <cell_modem_client.h>

using namespace etl::chrono;
using namespace etl::chrono_literals;

static const OkLoggingContext OK_CONTEXT("power_station");

struct meter {
  int i2c_address;
  etl::string_view name;
  etl::optional<Adafruit_INA228> driver;
};

static etl::array<meter, 4> meters{{
  {INA228_I2CADDR_DEFAULT, "Out"},
  {INA228_I2CADDR_DEFAULT + 1, "PPT"},
  {INA228_I2CADDR_DEFAULT + 4, "Panel"},
  {INA228_I2CADDR_DEFAULT + 5, "Local"},
}};

static etl::unique_ptr<CellModemClient> cell_modem;
static CellModemStatus const* cell_status = nullptr;
static etl::string<512> pub_buffer;

static steady_clock::time_point last_loop_time = {}; 
static steady_clock::time_point next_mqtt_time = {};
static steady_clock::time_point next_screen_time = {};

static void update_screen() {
  int ln = 0;
  ok_dock_layout->line_printf(ln++, "\f8\b\tV\tmA");
  OK_NOTE("\n☀️ Power Station");
  for (auto& meter : meters) {
    if (meter.driver) {
      auto const V = meter.driver->readBusVoltage() * 1e-3f;
      auto const mA = meter.driver->readCurrent();
      auto const mW = meter.driver->readPower();
      auto const J = meter.driver->readEnergy();
      auto const C = meter.driver->readDieTemp();
      ok_dock_layout->line_printf(
        ln++, "\f8.*s\t%.1f\t%.1f",
        meter.name.size(), meter.name.data(), V, mA
      );
      OK_NOTE(
        "%.*s: %.1fV %.1fmA %.0fmW %.3fJ %.1fC",
        meter.name.size(), meter.name.data(), V, mA, mW, J, C
      );
    } else {
      ok_dock_layout->line_printf(
        ln++, "\f8.*s\t-\t-", meter.name.size(), meter.name.data()
      );
      OK_NOTE("%.*s: missing at startup", meter.name.size(), meter.name.data());
    }
  }

  if (!cell_status) {
    ok_dock_layout->line_printf(ln++, "\f8No cell");
    OK_NOTE("Cell radio: No status");
  } else if (!cell_status->running) {
    ok_dock_layout->line_printf(ln++, "\f8Radio off");
    OK_NOTE("Cell radio: Off");
  } else if (!cell_status->registered) {
    ok_dock_layout->line_printf(ln++, "\f8Searching");
    OK_NOTE("Cell radio: Searching");
  } else if (!cell_status->ip_attached) {
    ok_dock_layout->line_printf(ln++, "\f8No IP");
    OK_NOTE("Cell radio: Registered, no IP");
  } else {
    uint8_t a[4];
    for (int i = 0; i < 4; ++i) a[i] = cell_status->ip_addr >> (8 * (3 - i));
    ok_dock_layout->line_printf(
      ln++, "\f8%d.%d.%d.%d %s", a[0], a[1], a[2], a[3],
      cell_status->mqtt_ready ? "M" : "-"
    );
    OK_NOTE(
      "Cell radio: IP %d.%d.%d.%d %s", a[0], a[1], a[2], a[3],
      cell_status->mqtt_ready ? "+MQTT" : "!MQTT"
    );
  }
}

static void update_mqtt() {
  if (!cell_status || !cell_status->mqtt_ready) return;
  if (cell_status->mqtt_publish_busy) return;

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

  auto json_cell = doc["cell_radio"];
  json_cell["op"] = cell_status->op_mcc * 1000 + cell_status->op_mnc;
  json_cell["tech"] = cell_status->radio_tech;
  json_cell["sig"] = cell_status->radio_rsrp;
  json_cell["snr"] = cell_status->radio_snr;

  pub_buffer.clear();
  auto const len = serializeJson(doc, pub_buffer.data(), pub_buffer.max_size());
  pub_buffer.uninitialized_resize(len);
  OK_NOTE("💬 MQTT publish (%db)", pub_buffer.size());
  cell_modem->publish({"blub/power_station", pub_buffer});
}

void loop() {
  rp2040.wdt_reset();
  auto const loop_time = steady_clock::now();
  if (last_loop_time != steady_clock::time_point{}) {
    auto const delay = loop_time - last_loop_time;
    if (delay > 10_ms) OK_ERROR("Loop took %lldms", raw_count<millis64>(delay));
  }

  cell_status = cell_modem->poll();
  while (cell_status->mqtt_receive_ready) {
    cell_modem->receive();
    cell_status = cell_modem->poll();
  }

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
  ok_serial_begin();
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f8Power Station");

  for (auto& meter : meters) {
    meter.driver.emplace();
    if (meter.driver->begin(meter.i2c_address)) {
      OK_NOTE("\"%s\" meter at 0x%x", meter.name, meter.i2c_address);
      meter.driver->setShunt(0.015, 10.0);
      meter.driver->setCurrentConversionTime(INA228_TIME_4120_us);
    } else {
      OK_ERROR("No \"%s\" meter at 0x%x", meter.name, meter.i2c_address);
      meter.driver.reset();
    }
  }

  Serial1.setFIFOSize(2048);
  Serial1.begin(115200);
  cell_modem = make_cell_modem_client(&Serial1, 25, blub_mqtt_config, {});
  next_mqtt_time = next_screen_time = steady_clock::now();
  rp2040.wdt_begin(5000);  // 5 second on-chip hardware watchdog (pet in loop())
}
