#include <Arduino.h>
#include <etl/chrono.h>
#include <etl/vector.h>
#include <ok_logging.h>
#include <ok_little_layout.h>
#include <ok_micro_dock.h>

#include <blub_mqtt_config.h>
#include <cell_modem_client.h>

using namespace etl::chrono;
using namespace etl::chrono_literals;

extern char const* const ok_logging_config = "ok_little_layout:NOTE,DETAIL";
static const OkLoggingContext OK_CONTEXT("cell_bench");

etl::unique_ptr<CellModemClient> cell_modem;

static steady_clock::time_point last_loop_time = {};
static steady_clock::time_point next_print_time = {};

void loop() {
  auto const loop_time = etl::chrono::steady_clock::now();
  if (last_loop_time.time_since_epoch().count() > 0) {
    auto const delay = loop_time - last_loop_time;
    if (delay > 1_ms) {
      auto const msec = duration_cast<duration<long, etl::milli>>(delay);
      OK_NOTE("loop time %ld ms", msec.count());
    }
  }

  auto const status = cell_modem->poll();

  uint8_t ip[4] = { 0, 0, 0, 0 };
  if (!status.running) {
    ok_dock_layout->line_printf(1, "\f9Radio Off");
  } else if (status.failed) {
    ok_dock_layout->line_printf(1, "\f9Failed (%d)", status.reject_cause);
  } else if (!status.registered) {
    ok_dock_layout->line_printf(1, "\f9Searching...");
  } else if (!status.ip_attached) {
    ok_dock_layout->line_printf(1, "\f9No IP");
  } else {
    for (int i = 0; i < 4; ++i) ip[i] = status.ip_addr >> (8 * (3 - i));
    ok_dock_layout->line_printf(
      1, "\f9%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]
    );
  }

  if (loop_time > next_print_time) {
    next_print_time = loop_time + 1_s;
    if (status.hardware.empty()) {
      OK_NOTE("🚫 hardware N/A");
    } else {
      OK_NOTE("\n📟 %s %s\n", status.hardware.c_str(), status.imeisv.c_str());
      OK_NOTE(
        "🏷️ %s %s %s %s",
        status.versions[0].c_str(),
        status.versions[1].c_str(),
        status.versions[2].c_str(),
        status.versions[3].c_str()
      );
    }
    if (!status.running) {
      OK_NOTE("🚫 radio off");
    } else if (status.failed) {
      OK_NOTE("❌️ failed/rejected (%d)", status.reject_cause);
    } else if (!status.registered) {
      OK_NOTE("🔎 searching...");
    } else {
      OK_NOTE(
        "%s op:%d-%d cell:%04x-%08x-%d",
        status.roaming ? "🌎 roam" : "🏠️ home", status.op_mcc, status.op_mnc,
        status.cell_tac, status.cell_id, status.cell_phys_id
      );
      OK_NOTE(
        "📶 tech:%d band:%d-%d signal:%ddB snr:%ddB",
        status.radio_tech, status.radio_band, status.radio_earfcn,
        status.radio_rsrp, status.radio_snr
      );
    }
    if (status.ip_attached) {
      OK_NOTE(
        "🌐 IP: %d.%d.%d.%d %s", ip[0], ip[1], ip[2], ip[3],
        status.mqtt_subscribed ? "🗨️ MQTT subscribed" :
        status.mqtt_connected ? "💬 MQTT connecting" : "⛓️‍💥 MQTT disconnected"
      );
    } else {
      OK_NOTE("⭕ No IP attached");
    }
  }

  last_loop_time = loop_time;
}

void setup() {
  ok_serial_begin();
  OK_NOTE("BLUB Cell Modem Bench Test");
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f10\1Cell Bench");

  for (int c = 5; c >= 0; --c) {
    OK_NOTE("⏳ Startup delay %dsec...", c);
    ok_dock_layout->line_printf(1, "\f9Start %d...", c);
    delay(1000);
  }

  Serial1.setTX(12);
  Serial1.setRX(13);
  Serial1.begin(115200);
  static const etl::string_view subs[] = {"cell_bench"};
  cell_modem = make_cell_modem_client(&Serial1, blub_mqtt_config, subs);
}
