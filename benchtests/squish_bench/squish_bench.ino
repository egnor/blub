#include <Adafruit_ADS1X15.h>
#include <algorithm>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cmath>
#include <etl/array.h>
#include <etl/vector.h>
#include <ok_little_layout.h>
#include <ok_logging.h>
#include <ok_micro_dock.h>
#include <U8g2lib.h>

#include <blub_clock_util.h>
#include <blub_warnings.h>

using namespace etl::chrono;
using namespace etl::chrono_literals;

static OkLoggingContext const OK_CONTEXT("squish_bench");

static float constexpr STEP_ON_GAUSS = 2.0f;
static float constexpr STEP_OFF_GAUSS = 1.0f;
static float constexpr BASELINE_MIX = 0.001f;  // updated 10ms

struct Squish {
  int reader;
  int channel;
  float volts = -1.0f;
  float gauss = -1.0f;
  float baseline = -1.0f;
  bool step = false;
  steady_clock::time_point step_off_time = {};
};

struct Reader {
  int address;
  etl::array<int, 4> channels = {-1, -1, -1, -1};
  Adafruit_ADS1115 adc;
  int active_channel = -1;
};

static etl::array readers{ Reader{0x48}, Reader{0x49} };

static etl::array squishes{
  Squish{0, 0}, Squish{0, 1}, Squish{0, 2},
  Squish{1, 0}, Squish{1, 1}, Squish{1, 2},
};

int vcc_reader = 1, vcc_channel = 3;

static steady_clock::time_point last_loop_time = {};
static steady_clock::time_point next_sensor_time = {};
static steady_clock::time_point next_print_time = {};

void loop() {
  auto const loop_time = steady_clock::now();
  if (last_loop_time > steady_clock::time_point{}) {
    auto const delay = loop_time - last_loop_time;
    if (delay > 5_ms) OK_ERROR("loop time %lld ms", raw_count<millis64>(delay));
  }
  last_loop_time = loop_time;

  for (auto& r : readers) {
    if (r.active_channel >= 0) {
      if (!r.adc.conversionComplete()) continue;
      r.channels[r.active_channel] = r.adc.getLastConversionResults();
    }
    r.active_channel = (r.active_channel + 1) % r.channels.size();
    r.adc.startADCReading(MUX_BY_CHANNEL[r.active_channel], false);
  }

  bool step_change = false;
  if (loop_time > next_sensor_time) {
    next_sensor_time = std::max(loop_time + 5_ms, next_sensor_time + 10_ms);
    for (auto& sq : squishes) {
      auto const counts = readers[sq.reader].channels[sq.channel];
      if (counts < 0) continue;
      sq.volts = counts * 187.5e-6f;
      sq.gauss = std::abs(sq.volts - 1.65f) * (1.0f / 4.0e-3f);
      if (sq.baseline < 0.0f) {
        OK_NOTE("ADC%d.%d base init %.1fG", sq.reader, sq.channel, sq.gauss);
        sq.baseline = sq.gauss;
      }
      if (sq.step && loop_time > sq.step_off_time + 10_s) {
        OK_ERROR(
          "ADC%d.%d step timeout (%.1fs) %.1fG", sq.reader, sq.channel,
          raw_count<secf>(loop_time - sq.step_off_time), sq.gauss
        );
        sq.baseline = sq.gauss;
      }
      auto const step_offset = sq.step ? STEP_OFF_GAUSS : STEP_ON_GAUSS;
      auto const step = sq.gauss >= sq.baseline + step_offset;
      if (!step) {
        auto constexpr f = BASELINE_MIX;
        sq.baseline = (1.0f - f) * sq.baseline + f * sq.gauss;
        sq.step_off_time = loop_time;
      }
      if (step != sq.step) {
        sq.step = step;
        step_change = true;
      }
    }
  }

  if (step_change || loop_time > next_print_time) {
    next_print_time = loop_time + 100_ms;

    JsonDocument doc;
    doc["t"] = raw_count<secf>(loop_time);
    doc["vcc"] = readers[vcc_reader].channels[vcc_channel] * 187.5e-6f;
    auto sq_outs = doc["sq"].to<JsonArray>();
    for (auto& sq : squishes) {
      auto sq_out = sq_outs.add<JsonObject>();
      if (sq.gauss >= 0.0f) {
        sq_out["V"] = sq.volts;
        sq_out["G"] = sq.gauss;
        sq_out["bG"] = sq.baseline;
        sq_out["dG"] = sq.gauss - sq.baseline;
        sq_out["s"] = sq.step;
      }
    }

    Serial.print("$/squish_bench|");
    serializeJson(doc, Serial);
    Serial.println();
  }
}

void setup() {
  ok_serial_begin();
  OK_NOTE("BLUB Squish Bench Test");
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f7\1Squish Bench");

  for (auto& r : readers) {
    OK_NOTE("ADS1115[0x%02x] starting", r.address);
    while (!r.adc.begin(r.address)) {
      OK_ERROR("ADS1115[0x%02x] initialization failed...", r.address);
      delay(500);
    }
    r.adc.setDataRate(RATE_ADS1115_475SPS);
  }
}
