#include <Arduino.h>

#include <ok_logging.h>
#include <ok_little_layout.h>
#include <ok_micro_dock.h>

static const OkLoggingContext OK_CONTEXT("display_test");
extern char const* const ok_logging_config = "*=DETAIL";

void loop() {
  uint8_t const b[] = {
    ok_dock_button(0),
    ok_dock_button(1),
    ok_dock_button(2)
  };
  OK_NOTE("LOOP, layout=%p, b=%d/%d/%d", ok_dock_layout, b[0], b[1], b[2]);
  if (ok_dock_layout) {
    ok_dock_layout->line_printf(0, "\f11\bHello Dock!");
    ok_dock_layout->line_printf(1, "\f9Buttons %d %d %d", b[0], b[1], b[2]);
  }
  delay(300);
}

void setup() {
  ok_serial_begin();
  OK_NOTE("SETUP");
  for (int i = 5; i >= 0; --i) {
    OK_NOTE("Starting in %d...", i);
    delay(1000);
  }
  OK_FATAL_IF(!ok_dock_init_feather_v8());
}
