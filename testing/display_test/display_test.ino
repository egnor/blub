#include <Arduino.h>

#include <ok_logging.h>
#include <ok_little_layout.h>
#include <ok_micro_dock.h>

static const OkLoggingContext OK_CONTEXT("display_test");
extern char const* const ok_logging_config = "*=DETAIL";

void loop() {
  OK_NOTE("LOOP");
  ok_dock_layout->line_printf(0, "\f9Hello World!");
  ok_dock_layout->line_printf(
    1, "\f9[%d] [%d] [%d]",
    ok_dock_button(0),
    ok_dock_button(1),
    ok_dock_button(2)
  );
  delay(20);
}

void setup() {
  ok_serial_begin();
  OK_NOTE("SETUP");
  OK_FATAL_IF(!ok_dock_init_feather_v8());
}
