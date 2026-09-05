#include <Arduino.h>

#include <ok_logging.h>
#include <ok_little_layout.h>
#include <ok_micro_dock.h>

#include <cell_modem_client.h>

extern char const* const ok_logging_config = "DETAIL";
static const OkLoggingContext OK_CONTEXT("cell_bench");

etl::unique_ptr<CellModemClient> cell_modem;

static long last_loop_millis = 0;

void loop() {
  auto const loop_millis = millis();
  if (last_loop_millis > 0) {
    auto const delay = loop_millis - last_loop_millis;
    if (delay > 2)
      OK_NOTE("loop time %ld ms", loop_millis - last_loop_millis);
  }

  cell_modem->poll();

  last_loop_millis = loop_millis;
}

void setup() {
  ok_serial_begin();
  OK_NOTE("BLUB Cell Modem Bench Test");
  ok_dock_init_feather_v8();
  ok_dock_layout->line_printf(0, "\v\f10\1Cell Bench");
  Serial1.setTX(12);
  Serial1.setRX(13);
  Serial1.begin(115200);
  cell_modem = make_cell_modem_client(&Serial1, "mqtt.eacs.io");
}
