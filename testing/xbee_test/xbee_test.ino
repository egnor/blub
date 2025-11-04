#include <Arduino.h>

#include "src/blub_station.h"
#include "src/ok_logging.h"
#include "src/xbee_api.h"
#include "src/xbee_mqtt_adapter.h"
#include "src/xbee_radio.h"
#include "src/xbee_socket_keeper.h"
#include "src/xbee_status_monitor.h"

static const OkLoggingContext OK_CONTEXT("xbee_test");

static XBeeStatusMonitor* monitor = nullptr;
static XBeeSocketKeeper* keeper = nullptr;
static XBeeMQTTAdapter* mqtt = nullptr;

static long last_loop_millis = 0;

void on_message(mqtt_response_publish const& message) {
  OK_NOTE("MQTT message %.*s", message.topic_name_size, message.topic_name);
}

void loop() {
  using namespace XBeeAPI;
  static Frame in, out;

  auto const loop_millis = millis();

  while (xbee_radio->poll_for_frame(&in)) {
    monitor->on_incoming(in);
    keeper->on_incoming(in);
    if (mqtt->incoming_to_outgoing(in, xbee_radio->outgoing_space(), &out)) {
      xbee_radio->add_outgoing(out);
    }
  }

  in.clear();
  while (mqtt->incoming_to_outgoing(in, xbee_radio->outgoing_space(), &out)) {
    xbee_radio->add_outgoing(out);
  }
  while (monitor->maybe_make_outgoing(xbee_radio->outgoing_space(), &out)) {
    xbee_radio->add_outgoing(out);
  }
  while (keeper->maybe_make_outgoing(xbee_radio->outgoing_space(), &out)) {
    xbee_radio->add_outgoing(out);
  }

  if (keeper->socket() != mqtt->active_socket()) {
    mqtt->use_socket(keeper->socket());
    mqtt_connect(
        mqtt->client(), "BLUB XBee Test",
        nullptr, nullptr, 0,
        "blub", "blub",
        MQTT_CONNECT_CLEAN_SESSION, 400);
    mqtt_publish(
        mqtt->client(), "blub/test", "Hello World!", 12, MQTT_PUBLISH_QOS_1);
  }

  auto const& st = monitor->status();
  if (!xbee_radio->raw_serial()) {
    status_layout->line_printf(1, "\f9No XBee found");
  } else if (!st.hardware_ver) {
    status_layout->line_printf(1, "\f9XBee API starting");
  } else {
    status_layout->line_printf(
        1, "\f9XBee %04x / %05x", st.hardware_ver, st.firmware_ver);
    status_layout->line_printf(
        2, "\f9%s / %s / %s",
        st.network_operator, st.operating_apn,
        st.technology == XBeeStatusMonitor::UNKNOWN_TECH
            ? "" : st.technology_text());
    if (st.assoc_status == XBeeStatusMonitor::CONNECTED) {
      auto const& ip = st.ip_address;
      status_layout->line_printf(
          3, "\f9P%.1f Q%.1f %d.%d.%d.%d",
          st.received_power, st.received_quality,
          ip[0], ip[1], ip[2], ip[3]);
    } else {
      status_layout->line_printf(
          3, "\f9P%.1f Q%.1f %s",
          st.received_power, st.received_quality, st.assoc_text());
    }
  }

  if (last_loop_millis > 0) {
    auto const delay = loop_millis - last_loop_millis;
    if (delay > 2)
      OK_NOTE("loop time %ld ms", loop_millis - last_loop_millis);
  }

  last_loop_millis = loop_millis;
}

void setup() {
  while (!Serial.dtr() && millis() < 2000) delay(10);
  blub_station_init("BLUB XBee Test");
  monitor = make_xbee_status_monitor();
  keeper = make_xbee_socket_keeper(
      "egnor-2020.ofb.net", 1883, XBeeAPI::SocketCreate::Protocol::TCP);
  mqtt = make_xbee_mqtt_adapter(512, 512, on_message);
}
