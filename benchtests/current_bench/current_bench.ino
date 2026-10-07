#include <Adafruit_INA228.h>
#include <Arduino.h>
#include <etl/chrono.h>
#include <etl/format.h>
#include <etl/vector.h>
#include <ok_logging.h>
#include <ok_little_layout.h>
#include <ok_micro_dock.h>

#include <blub_clock_util.h>
#include <blub_warnings.h>

using namespace etl::chrono;
using namespace etl::chrono_literals;

using namespace etl::chrono;
using namespace etl::chrono_literals;

static const OkLoggingContext OK_CONTEXT("current_bench");

struct sensor {
  int i2c_address;
  Adafruit_INA228 ina;
};

static etl::vector<sensor, 4> sensors;

static steady_clock::time_point last_loop_time = {};
static steady_clock::time_point next_print_time = {};
static steady_clock::time_point next_publish_time = {};

void loop() {
  auto const loop_time = steady_clock::now();
  if (last_loop_time > steady_clock::time_point{}) {
    auto const delay = loop_time - last_loop_time;
    if (delay > 1_ms) OK_NOTE("loop time %lld ms", raw_count<millis64>(delay));
  }

  last_loop_time = loop_time;
}

void setup() {
  ok_serial_begin();
  OK_NOTE("BLUB Current Sensor Bench Test");
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f10\1Current Bench");

  for (int offset = 0; offset < 16 && !sensors.full(); ++offset) {
    int const i2c_address = INA228_I2CADDR_DEFAULT + offset;
    sensors.emplace_back();
    sensors.back().i2c_address = i2c_address;
    if (sensors.back().ina.begin(i2c_address)) {
      OK_NOTE("Found INA228 at 0x%02X", i2c_address);
    } else {
      OK_NOTE("No INA228 at 0x%02X", i2c_address);
      sensors.pop_back();
    }
  }
}
