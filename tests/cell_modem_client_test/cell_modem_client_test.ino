// Unit tests for CellModemClient (cell_modem_client_lib/cell_modem_client.h)

#include "cell_modem_client.h"

#include <Arduino.h>
#include <etl/string.h>
#include <etl/vector.h>

#include <fake_serial.h>
#include <verifiers.h>

char const* const ok_logging_config = "DETAIL";

static OkLoggingContext OK_CONTEXT("cell_modem_client_test");

constexpr int EN_PIN = 13;

static MqttServerConfig mqtt_config() {
  MqttServerConfig config;
  config.host = "fake-server";
  config.port = 8883;
  config.user = "fake-user";
  config.password = "fake-pass";
  config.cert = "-----FAKE CERT-----\r\nABCDEF\r\n-----END CERT-----\r\n";
  config.cert_sha256 = "Fake Cert Hash";
  return config;
}

//
// Fake modem helpers
//

// Commands seen by fake_modem_reply(), '|'-separated, for later assertions
static etl::string<1024> fake_log;
static bool fake_connected = false;

static bool fake_modem_reply(FakeSerial* serial) {
  // Just enough response logic to get through initialization
  bool success = true;
  auto const command = etl::trim_from_view_left(serial->write_buf, "\r\n\"+");
  fake_log.append(command.data(), etl::min(command.size(), size_t(24)));
  fake_log.push_back('|');
  if (command.starts_with("AT+CGMM")) {
    serial->read_buf = "Fake Hardware\r\nOK\r\n";
  } else if (command.starts_with("AT+CGMR")) {
    serial->read_buf = "Fake Revision\r\nOK\r\n";
  } else if (command.starts_with("AT+CGSN=2")) {
    serial->read_buf = "+CGSN: \"1122222233333344\"\r\nOK\r\n";
  } else if (command.starts_with("AT+CGPADDR")) {
    serial->read_buf = "+CGPADDR: 0,\"12.34.56.78\"\r\nOK\r\n";
  } else if (command.starts_with("AT%CMNG=1,")) {
    serial->read_buf = "%CMNG: 0,0,\"Fake Cert Hash\"\r\nOK\r\n";
  } else if (command.starts_with("AT%XMONITOR")) {
    serial->read_buf = "%XMONITOR: 5,"
      "\"\",\"\",\"111222\",\"1234\",7,12,\"12345678\",123,1234,12,12,"
      "\"\",\"00000000\",\"00000000\",\"00000000\"\r\nOK\r\n";
  } else if (command.starts_with("AT#XMQTTCON?")) {
    serial->read_buf = fake_connected
      ? "#XMQTTCON: 1,\"1122222233333344\",\"fake-server\",8883,0\r\nOK\r\n"
      : "#XMQTTCON: 0\r\nOK\r\n";
  } else if (command.starts_with("AT#XMQTTCON=0")) {
    serial->read_buf = fake_connected
      ? "OK\r\n#XMQTTEVT: 1,0\r\n"  // Disconnect
      : "ERROR\r\n";  // Not connected
    fake_connected = false;
  } else if (command.starts_with("AT#XMQTTCON=1")) {
    serial->read_buf = "OK\r\n#XMQTTEVT: 0,0\r\n";  // CONNACK
    fake_connected = true;
  } else if (command.starts_with("AT#XMQTTSUB=")) {
    serial->read_buf = "OK\r\n#XMQTTEVT: 7,0\r\n";  // SUBACK
  } else if (command.starts_with("AT#XSMVER")) {
    serial->read_buf = "#XSMVER: \"Fake SM\",\"Fake NCS\",\"Blub\"\r\nOK\r\n";
  } else if (command.starts_with("AT")) {
    serial->read_buf = "OK\r\n";  // nod and smile
  } else {
    serial->read_buf = "";  // ignore, let the caller put something in
    success = false;
  }
  serial->write_buf.clear();
  return success;
}

// Polls until the client writes something (or wait_ms elapses), verifies
// it's the expected command, then queues a reply for the next poll.
static bool expect(
  etl::unique_ptr<CellModemClient> const& cm, FakeSerial* serial,
  etl::string_view expect, int wait_ms = 500
) {
  for (int ms = 0; serial->write_buf.empty() && ms < wait_ms; ms += 10) {
    cm->poll();
    delay(10);
  }
  bool const ok = VERIFY_A_OP_B_STR(serial->write_buf, ==, expect);
  fake_modem_reply(serial);  // ok to fail; caller can write serial->read_buf
  return ok;
}

// Polls for a while, verifying the client sends nothing
static bool expect_idle(
  etl::unique_ptr<CellModemClient> const& cm, FakeSerial* serial, int ms
) {
  for (int t = 0; serial->write_buf.empty() && t < ms; t += 10) {
    cm->poll();
    delay(10);
  }
  return VERIFY_A_OP_B_STR(serial->write_buf, ==, "");
}

// Polls with the fake modem answering until stop(status) or wait_ms elapses
static bool run_until(
  etl::unique_ptr<CellModemClient> const& cm, FakeSerial* serial, int wait_ms,
  bool (*stop)(CellModemStatus const&)
) {
  for (int ms = 0; ms < wait_ms; ms += 10) {
    auto const& status = cm->poll();
    if (!serial->write_buf.empty()) {
      if (!fake_modem_reply(serial)) return false;
    } else if (stop(status)) {
      return true;
    }
    delay(10);
  }
  OK_ERROR("#TEST-FAIL# run_until overrun (%dms)", wait_ms);
  return false;
}

static bool run_until_ready(
  etl::unique_ptr<CellModemClient> const& cm, FakeSerial* serial
) {
  fake_connected = false;
  auto const ready = [](CellModemStatus const& s) { return s.mqtt_ready; };
  return run_until(cm, serial, 3000, ready);
}

static bool en_pin_low() {
  return gpio_get_dir(EN_PIN) == 1 && gpio_get_out_level(EN_PIN) == LOW;
}

//
// Tests
//

static void test_modem_client_setup() {
  OK_NOTE("\n#TEST# test_modem_client_setup");
  FakeSerial serial;
  etl::vector<etl::string_view, 2> subs({"topic1", "topic2"});
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), subs);

  // Verify the specific initialization and poll cycle
  cm->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "");
  VERIFY_TRUE(en_pin_low());

  expect(cm, &serial, "\r+++\"\rAT\r");
  VERIFY_A_OP_B_INT(gpio_get_dir(EN_PIN), ==, 0);
  VERIFY_TRUE(gpio_is_pulled_up(EN_PIN));

  expect(cm, &serial, "AT+CGMM\r");
  expect(cm, &serial, "AT+CGMR\r");
  expect(cm, &serial, "AT#XSMVER\r");
  expect(cm, &serial, "AT+CGSN=2\r");
  expect(cm, &serial, "AT%CMNG=1,0,0\r");
  expect(cm, &serial, "AT%XPDNCFG=1\r");  // always-on IP
  expect(cm, &serial, "AT+CFUN=1\r");  // radio on
  expect(cm, &serial, "AT+CEREG=1\r");  // reg notify on
  expect(cm, &serial, "AT+CGEREP=1\r");  // data notify on
  expect(cm, &serial, "AT%XMONITOR\r");  // check radio
  expect(cm, &serial, "AT+CGPADDR\r");  // check IP
  expect(cm, &serial, "AT#XMQTTCON?\r");  // check MQTT
  expect(cm, &serial, "AT#XMQTTCFG=\"1122222233333344\",60,1\r");
  expect(
    cm, &serial,
    "AT#XMQTTCON=1,\"fake-user\",\"fake-pass\",\"fake-server\",8883,0\r"
  );
  expect(cm, &serial, "AT#XMQTTSUB=\"topic1\",0\r");
  expect(cm, &serial, "AT#XMQTTSUB=\"topic2\",0\r");
  expect_idle(cm, &serial, 500);
  VERIFY_TRUE(fake_connected);

  // Status after initial setup
  auto const& status = cm->poll();
  VERIFY_A_OP_B_STR(status.hardware, ==, "Fake Hardware");
  VERIFY_A_OP_B_STR(status.imeisv, ==, "1122222233333344");
  VERIFY_A_OP_B_STR(status.versions[0], ==, "Fake Revision");
  VERIFY_A_OP_B_STR(status.versions[1], ==, "Fake SM");
  VERIFY_A_OP_B_STR(status.versions[2], ==, "Fake NCS");
  VERIFY_A_OP_B_STR(status.versions[3], ==, "Blub");
  VERIFY_TRUE(status.running);
  VERIFY_TRUE(status.registered);
  VERIFY_TRUE(status.roaming);
  VERIFY_TRUE(!status.failed);
  VERIFY_A_OP_B_INT(status.op_mcc, ==, 111);
  VERIFY_A_OP_B_INT(status.op_mnc, ==, 222);
  VERIFY_A_OP_B_INT(status.cell_tac, ==, 0x1234);
  VERIFY_A_OP_B_INT(status.cell_phys_id, ==, 123);
  VERIFY_A_OP_B_INT(status.cell_id, ==, 0x12345678);
  VERIFY_A_OP_B_INT(status.radio_earfcn, ==, 1234);
  VERIFY_A_OP_B_INT(status.radio_tech, ==, 7);
  VERIFY_A_OP_B_INT(status.radio_band, ==, 12);
  VERIFY_A_OP_B_INT(status.radio_rsrp, ==, -129);
  VERIFY_A_OP_B_INT(status.radio_snr, ==, -13);
  VERIFY_TRUE(status.ip_attached);
  VERIFY_A_OP_B_INT(status.ip_addr, ==, 0x0C22384E);
  VERIFY_TRUE(status.mqtt_ready);
  VERIFY_TRUE(!status.mqtt_publish_busy);
  VERIFY_TRUE(!status.mqtt_receive_ready);

  // Periodic status poll, 10s later
  expect(cm, &serial, "AT%XMONITOR\r", 11000);
  expect(cm, &serial, "AT+CGPADDR\r");
  expect(cm, &serial, "AT#XMQTTCON?\r");
  expect_idle(cm, &serial, 500);
  VERIFY_TRUE(cm->poll().mqtt_ready);
}

static void test_cert_rewrite() {
  OK_NOTE("\n#TEST# test_cert_rewrite");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});

  expect(cm, &serial, "\r+++\"\rAT\r");
  expect(cm, &serial, "AT+CGMM\r");
  expect(cm, &serial, "AT+CGMR\r");
  expect(cm, &serial, "AT#XSMVER\r");
  expect(cm, &serial, "AT+CGSN=2\r");

  // Mismatched cert: radio off, erase, write, re-check
  expect(cm, &serial, "AT%CMNG=1,0,0\r");
  serial.read_buf = "%CMNG: 0,0,\"Wrong Hash\"\r\nOK\r\n";
  expect(cm, &serial, "AT+CFUN=4\r");
  expect(cm, &serial, "AT%CMNG=3,0,0\r");
  for (int ms = 0; serial.write_buf.empty() && ms < 500; ms += 10) {
    cm->poll();
    delay(10);
  }
  etl::string_view const write(serial.write_buf);
  VERIFY_TRUE(write.starts_with("AT%CMNG=0,0,0,\"-----FAKE CERT-----\r\n"));
  VERIFY_TRUE(write.ends_with("-----END CERT-----\r\n\"\r"));
  serial.write_buf.clear();
  serial.read_buf = "OK\r\n";

  // Still wrong after the rewrite (empty slot): give up, don't loop on NVM
  expect(cm, &serial, "AT%CMNG=1,0,0\r");
  expect(cm, &serial, "AT%XPDNCFG=1\r");
  expect(cm, &serial, "AT+CFUN=1\r");
}

static void test_mqtt_publish() {
  OK_NOTE("\n#TEST# test_mqtt_publish");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});
  if (!run_until_ready(cm, &serial)) return;

  auto const& st1 = cm->poll();
  if (!VERIFY_TRUE(!st1.mqtt_publish_busy)) return;
  cm->publish({.topic = "test-topic", .payload = "test-payload"});
  VERIFY_TRUE(st1.mqtt_publish_busy);  // modified in reference

  expect(cm, &serial, "AT#XMQTTPUB=\"test-topic\",\"\",1,0,12\r");
  expect(cm, &serial, "test-payload");
  serial.read_buf = "#XDATAMODE: 0\r\n";  // data sent, waiting for PUBACK

  auto const& st2 = cm->poll();
  VERIFY_TRUE(st2.mqtt_publish_busy);  // after #XDATAMODE: 0
  expect_idle(cm, &serial, 100);  // idle until PUBACK

  serial.read_buf = "#XMQTTEVT: 3,0\r\n";  // PUBACK
  auto const& st3 = cm->poll();
  VERIFY_TRUE(!st3.mqtt_publish_busy);  // after PUBACK
  VERIFY_TRUE(st3.mqtt_ready);
  expect_idle(cm, &serial, 100);
}

static void test_mqtt_publish_rejected() {
  OK_NOTE("\n#TEST# test_mqtt_publish_rejected");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});
  if (!run_until_ready(cm, &serial)) return;

  // Publish rejected: the session is gone (firmware reports the teardown)
  cm->publish({.topic = "test-topic", .payload = "test-payload"});
  expect(cm, &serial, "AT#XMQTTPUB=\"test-topic\",\"\",1,0,12\r");
  serial.read_buf = "ERROR\r\n#XMQTTEVT: 1,-128\r\n";
  auto const& st1 = cm->poll();
  VERIFY_TRUE(!st1.mqtt_publish_busy);  // dropped
  VERIFY_TRUE(!st1.mqtt_ready);  // reconnecting

  // Reconnects (after backoff) without retrying the publish
  fake_log.clear();
  auto const ready = [](CellModemStatus const& s) { return s.mqtt_ready; };
  run_until(cm, &serial, 60000, ready);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTPUB"), ==, etl::istring::npos);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTCFG"), !=, etl::istring::npos);
  VERIFY_TRUE(!cm->poll().mqtt_publish_busy);
}

static void test_mqtt_publish_data_failed() {
  OK_NOTE("\n#TEST# test_mqtt_publish_data_failed");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});
  if (!run_until_ready(cm, &serial)) return;

  // Publish accepted but the data send fails: drop it and reconnect
  cm->publish({.topic = "test-topic", .payload = "test-payload"});
  expect(cm, &serial, "AT#XMQTTPUB=\"test-topic\",\"\",1,0,12\r");
  expect(cm, &serial, "test-payload");
  serial.read_buf =  "#XDATAMODE: -1\r\n";
  auto const& st1 = cm->poll();
  VERIFY_TRUE(!st1.mqtt_publish_busy);  // dropped
  VERIFY_TRUE(!st1.mqtt_ready);  // reconnecting
  expect(cm, &serial, "AT#XMQTTCON=0\r");
  expect(cm, &serial, "AT#XMQTTCFG=\"1122222233333344\",60,1\r");

  fake_log.clear();
  auto const ready = [](CellModemStatus const& s) { return s.mqtt_ready; };
  run_until(cm, &serial, 60000, ready);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTPUB"), ==, etl::istring::npos);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTCON=1"), !=, etl::istring::npos);
}

static void test_mqtt_receive() {
  OK_NOTE("\n#TEST# test_mqtt_receive");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});
  if (!run_until_ready(cm, &serial)) return;

  serial.read_buf =
    "#XMQTTMSG: 10,13\r\ntest/topic\r\nHello, World!\r\n#XMQTTEVT: 2,0\r\n";
  auto const& st1 = cm->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "");  // still idle
  VERIFY_TRUE(st1.mqtt_receive_ready);
  auto const m1 = cm->receive();
  VERIFY_A_OP_B_STR(m1.topic, ==, "test/topic");
  VERIFY_A_OP_B_STR(m1.payload, ==, "Hello, World!");

  auto const& st2 = cm->poll();  // consumes the trailing #XMQTTEVT
  VERIFY_TRUE(!st2.mqtt_receive_ready);
  expect_idle(cm, &serial, 100);

  // Payloads may contain anything, including line breaks and quotes
  serial.read_buf = "#XMQTTMSG: 1,9\r\nt\r\n\"a\r\nOK\r\n\"\r\n";
  auto const& st3 = cm->poll();
  VERIFY_TRUE(st3.mqtt_receive_ready);
  auto const m3 = cm->receive();
  VERIFY_A_OP_B_STR(m3.topic, ==, "t");
  VERIFY_A_OP_B_STR(m3.payload, ==, "\"a\r\nOK\r\n\"");
  expect_idle(cm, &serial, 100);
}

static void test_mqtt_receive_oversize() {
  OK_NOTE("\n#TEST# test_mqtt_receive_oversize");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});
  if (!run_until_ready(cm, &serial)) return;

  // Too big for the client's buffer: counted through, dropped, stays in sync
  static etl::string<3100> big;
  big = "#XMQTTMSG: 4,3000\r\ntest\r\n";
  big.append(3000, 'x');
  big.append("\r\n#XMQTTEVT: 2,0\r\n");
  serial.read_buf = big;
  auto const& st1 = cm->poll();
  VERIFY_TRUE(!st1.mqtt_receive_ready);
  expect_idle(cm, &serial, 100);
  VERIFY_A_OP_B_INT(serial.read_buf.size(), ==, 0);  // everything consumed

  serial.read_buf = "#XMQTTMSG: 4,4\r\ntest\r\nabcd\r\n";
  auto const& st2 = cm->poll();
  VERIFY_TRUE(st2.mqtt_receive_ready);
  VERIFY_A_OP_B_STR(cm->receive().payload, ==, "abcd");

  // Absurdly big: reset the modem rather than wait for it all
  serial.read_buf = "#XMQTTMSG: 4,70000\r\n";
  auto const reset = [](CellModemStatus const&) { return en_pin_low(); };
  VERIFY_TRUE(run_until(cm, &serial, 1000, reset));
}

static void test_command_timeout() {
  OK_NOTE("\n#TEST# test_command_timeout");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});

  // Unanswered command: probe the modem again, then restart setup
  expect(cm, &serial, "\r+++\"\rAT\r");
  expect(cm, &serial, "AT+CGMM\r");
  serial.read_buf = "";  // no reply
  expect(cm, &serial, "\r+++\"\rAT\r", 6000);  // after 5s timeout
  expect(cm, &serial, "AT+CGMM\r");
  expect(cm, &serial, "AT+CGMR\r");
  serial.read_buf = "";  // no reply

  // Repeatedly unanswered probes: just keeps trying
  expect(cm, &serial, "\r+++\"\rAT\r", 6000);  // after 5s timeout
  serial.read_buf = "";  // no reply
  expect(cm, &serial, "\r+++\"\rAT\r", 6000);  // after 5s timeout
  serial.read_buf = "";  // no reply
  expect(cm, &serial, "\r+++\"\rAT\r", 6000);  // after 5s timeout
  serial.read_buf = "";  // no reply
}

static void test_modem_restart() {
  OK_NOTE("\n#TEST# test_modem_restart");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});
  if (!run_until_ready(cm, &serial)) return;

  // Unexpected "Ready": the modem rebooted, everything is invalid
  serial.read_buf = "\xffReady\r\n";
  fake_connected = false;
  auto const& st1 = cm->poll();
  VERIFY_TRUE(!st1.running);
  VERIFY_TRUE(!st1.ip_attached);
  VERIFY_TRUE(!st1.mqtt_ready);
  VERIFY_TRUE(st1.failed);

  // Probe, then full setup, then reconnect
  fake_log.clear();
  expect(cm, &serial, "\r+++\"\rAT\r");
  expect(cm, &serial, "AT+CGMM\r");
  auto const ready = [](CellModemStatus const& s) { return s.mqtt_ready; };
  run_until(cm, &serial, 60000, ready);
  VERIFY_A_OP_B_INT(fake_log.find("AT+CFUN=1"), !=, etl::istring::npos);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTCON=0"), ==, etl::istring::npos);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTCON=1"), !=, etl::istring::npos);
  VERIFY_TRUE(!cm->poll().failed);
}

static void test_mqtt_disconnect_event() {
  OK_NOTE("\n#TEST# test_mqtt_disconnect_event");
  FakeSerial serial;
  auto const cm = make_cell_modem_client(&serial, EN_PIN, mqtt_config(), {});
  if (!run_until_ready(cm, &serial)) return;

  // Firmware-initiated disconnect: confirm with a poll, then reconnect
  serial.read_buf = "#XMQTTEVT: 1,-113\r\n";
  fake_connected = false;
  cm->poll();
  expect(cm, &serial, "AT#XMQTTCON?\r");
  auto const& st1 = cm->poll();
  VERIFY_TRUE(!st1.mqtt_ready);
  expect(cm, &serial, "AT#XMQTTCFG=\"1122222233333344\",60,1\r");

  fake_log.clear();
  auto const ready = [](CellModemStatus const& s) { return s.mqtt_ready; };
  run_until(cm, &serial, 60000, ready);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTCON=0"), ==, etl::istring::npos);
  VERIFY_A_OP_B_INT(fake_log.find("AT#XMQTTCON=1"), !=, etl::istring::npos);
}

void setup() {
  Serial1.begin(115200);
  ok_logging_stream = &Serial1;
  pinMode(EN_PIN, INPUT_PULLUP);
  OK_NOTE("#BEGIN-TESTS#");
  test_modem_client_setup();
  test_cert_rewrite();
  test_mqtt_publish();
  test_mqtt_publish_rejected();
  test_mqtt_publish_data_failed();
  test_mqtt_receive();
  test_mqtt_receive_oversize();
  test_command_timeout();
  test_modem_restart();
  test_mqtt_disconnect_event();
  OK_NOTE("#END-TESTS#");
}

void loop() {}
