#include <Arduino.h>

#include "blub_station.h"
#include "tagged_logging.h"
#include "little_status.h"
#include "xbee_api.h"
#include "xbee_radio.h"
#include "xbee_status_monitor.h"

#include "mqtt.h"

static const TaggedLoggingContext TL_CONTEXT("xbee_test");

static XBeeStatusMonitor* monitor = nullptr;

static long last_loop_millis = 0;

void loop() {
  using namespace XBeeAPI;
  static Frame frame;

  auto const loop_millis = millis();

  while (xbee_radio->poll_for_frame(&frame)) {
    monitor->on_incoming(frame);
  }

  int const space = xbee_radio->outgoing_space();
  while (monitor->maybe_make_outgoing(space, &frame)) {
    xbee_radio->enqueue_outgoing(frame);
  }

  auto const& st = monitor->status();
  if (!xbee_radio->raw_serial()) {
    status_screen->line_printf(1, "\f9No XBee found");
  } else if (!st.hardware_ver) {
    status_screen->line_printf(1, "\f9XBee API starting");
  } else {
    status_screen->line_printf(
        1, "\f9XBee %04x / %05x", st.hardware_ver, st.firmware_ver);
    status_screen->line_printf(
        2, "\f9%s / %s / %s",
        st.network_operator, st.operating_apn,
        st.technology == XBeeStatusMonitor::UNKNOWN_TECH
            ? "" : st.technology_text());
    if (st.assoc_status == XBeeStatusMonitor::CONNECTED) {
      auto const& ip = st.ip_address;
      status_screen->line_printf(
          3, "\f9P%.1f Q%.1f %d.%d.%d.%d",
          st.received_power, st.received_quality,
          ip[0], ip[1], ip[2], ip[3]);
    } else {
      status_screen->line_printf(
          3, "\f9P%.1f Q%.1f %s",
          st.received_power, st.received_quality, st.assoc_text());
    }
  }

  if (last_loop_millis > 0) {
    auto const delay = loop_millis - last_loop_millis;
    if (delay > 2)
      TL_NOTICE("loop time %ld ms", loop_millis - last_loop_millis);
  }

  last_loop_millis = loop_millis;
}

void setup() {
  while (!Serial.dtr() && millis() < 2000) delay(10);
  blub_station_init("BLUB XBee Test");
  monitor = make_xbee_status_monitor();
}
