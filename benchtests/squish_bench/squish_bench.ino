#include <Adafruit_ADS1X15.h>
#include <Arduino.h>
#include <cmath>
#include <etl/array.h>
#include <ok_little_layout.h>
#include <ok_logging.h>
#include <ok_micro_dock.h>
#include <U8g2lib.h>

#include <blub_clock_util.h>
#include <blub_warnings.h>

using namespace etl::chrono;
using namespace etl::chrono_literals;

static const OkLoggingContext OK_CONTEXT("squish_bench");

struct Squish {
  int adc;
  int input;
  float last_G;
};

static etl::array<Adafruit_ADS1115, 2> adcs;

static etl::array<Squish, 6> squishes{{
  {0, 0},
  {0, 1},
  {0, 2},
  {0, 3},
  {1, 0},
  {1, 1},
}};

static steady_clock::time_point last_loop_time = {};
static steady_clock::time_point next_sensor_time = {};
static steady_clock::time_point next_print_time = {};

void loop() {
  auto const loop_time = steady_clock::now();
  if (last_loop_time > steady_clock::time_point{}) {
    auto const delay = loop_time - last_loop_time;
    if (delay > 5_ms) OK_NOTE("loop time %lld ms", raw_count<millis64>(delay));
  }
  last_loop_time = loop_time;

  if (loop_time > next_sensor_time) {
    next_sensor_time = loop_time + 10_ms;
    for (auto& squish : squishes) {
      auto const count = adcs[squish.adc].readADC_SingleEnded(squish.input);
      auto const gauss = std::abs(count * 187.5e-6f - 1.65f) * (1.0f / 4.0e-3f);
      squish.last_G = gauss;
    }
  }

  if (loop_time > next_print_time) {
    next_print_time = loop_time + 250_ms;
    for (auto& squish : squishes) {
      Serial.printf(" %6.2f", squish.last_G);
    }
    Serial.println();
  }
}

void setup() {
  ok_serial_begin();
  OK_NOTE("BLUB Squish Bench Test");
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f7\1Squish Bench");

  for (int a = 0; a < adcs.size(); ++a) {
    int const addr = 0x48 + a;
    OK_NOTE("ADS1115[0x%02x] starting", addr);
    while (!adcs[a].begin(addr)) {
      OK_ERROR("ADS1115[0x%02x] initialization failed...", addr);
      delay(500);
    }
  }
}
