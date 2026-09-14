// Unit tests for CellModemClient (cell_modem_client_lib/cell_modem_client.h)
//
// TODO: cert mismatch on startup (verify cert re-write)
// TODO: MQTT publish rejected with ERROR (verify busy clear, no auto retry)
// TODO: MQTT publish rejected with #XDATAMODE: -1 (should disocnnect)

#include "cell_modem_client.h"

#include <Arduino.h>
#include <etl/format.h>
#include <SHA256.h>

#include <blub_mqtt_config.h>
#include <fake_serial.h>
#include <verifiers.h>

char const* const ok_logging_config = "DETAIL";

static OkLoggingContext OK_CONTEXT("cell_modem_client_test");

constexpr int MODEM_ENABLE_PIN = 13;

static MqttServerConfig fake_mqtt_config() {
  MqttServerConfig config;
  config.host = "fake-server";
  config.port = 8883;
  config.user = "fake-user";
  config.password = "fake-pass";
  config.cert = "-----FAKE CERT-----\r\nABCDEF\r\n-----END CERT-----\r\n";
  config.cert_sha256 = "Fake Cert Hash";
  return config;
}

static bool fake_modem_reply(FakeSerial* serial) {
  // Just enough response logic to get through initialization
  bool success = true;
  if (serial->write_buf.starts_with("AT+CGMM")) {
    serial->read_buf = "Fake Hardware\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT+CGMR")) {
    serial->read_buf = "Fake Revision\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT+CGSN=2")) {
    serial->read_buf = "+CGSN: \"1122222233333344\"\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT+CGPADDR")) {
    serial->read_buf = "+CGPADDR: 0,\"12.34.56.78\"\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT%CMNG=1,")) {
    serial->read_buf = "%CMNG: 0,0,\"Fake Cert Hash\"\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT%XMONITOR")) {
    serial->read_buf = "%XMONITOR: 5,"
      "\"\",\"\",\"111222\",\"1234\",7,12,\"12345678\",123,1234,12,12,"
      "\"\",\"00000000\",\"00000000\",\"00000000\"\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT#XMQTTCON?")) {
    serial->read_buf = "#XMQTTCON: 1\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT#XMQTTCON=0")) {
    serial->read_buf = "OK\r\n#XMQTTEVT: 1,0\r\n";  // Disconnect
  } else if (serial->write_buf.starts_with("AT#XMQTTCON=1")) {
    serial->read_buf = "OK\r\n#XMQTTEVT: 0,0\r\n";  // CONNACK
  } else if (serial->write_buf.starts_with("AT#XMQTTSUB=")) {
    serial->read_buf = "OK\r\n#XMQTTEVT: 7,0\r\n";  // SUBACK
  } else if (serial->write_buf.starts_with("AT#XSMVER")) {
    serial->read_buf = "#XSMVER: \"Fake SM\",\"Fake NCS\",\"Blub\"\r\nOK\r\n";
  } else if (serial->write_buf.starts_with("AT")) {
    serial->read_buf = "OK\r\n";  // nod and smile
  } else {
    OK_ERROR("#TEST-FAIL# Bad client output: [%s]", serial->write_buf.c_str());
    success = false;
  }
  serial->write_buf.clear();
  return success;
}

static bool run_client_setup(
  etl::unique_ptr<CellModemClient> const& client, FakeSerial* serial
) {
  for (int loop = 0; loop < 100; ++loop) {
    auto const status = client->poll();
    if (!serial->write_buf.empty()) {
      if (!fake_modem_reply(serial)) return false;
    } else if (status.mqtt_ready) {
      OK_NOTE("CellModemClient setup complete (loop=%d)", loop);
      return true;
    }
    delay(10);
  }
  OK_ERROR("#TEST-FAIL# CellModemClient setup overrun");
  return false;
}

static void test_blub_cert_sha256() {
  OK_NOTE("\n#TEST# test_blub_cert_sha256");
  SHA256 sha256;
  sha256.update(blub_mqtt_config.cert.data(), blub_mqtt_config.cert.size());
  uint8_t digest[32];
  sha256.finalize(digest, sizeof(digest));
  for (int i = 0; i < sizeof(digest); ++i) {
    etl::string<3> digest_byte;
    etl::format_to(digest_byte, "{:02X}", digest[i]);
    auto const stored_byte = blub_mqtt_config.cert_sha256.substr(i * 2, 2);
    if (!VERIFY_A_OP_B_STR(digest_byte, ==, stored_byte))
      OK_ERROR("Mismatch in blub_mqtt_cert_sha256[%d]", i);
  }
}

static void test_modem_client_setup() {
  OK_NOTE("\n#TEST# test_modem_client_setup");
  FakeSerial serial;

  etl::vector<etl::string_view, 2> subs({"topic1", "topic2"});
  auto const client = make_cell_modem_client(
    &serial, MODEM_ENABLE_PIN, fake_mqtt_config(), subs
  );

  // Verify the specific initialization and poll cycle
  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "");
  VERIFY_A_OP_B_INT(gpio_get_out_level(MODEM_ENABLE_PIN), ==, LOW);

  for (int i = 0; i < 15 && serial.write_buf.empty(); ++i) {
    client->poll();
    delay(10);
  }
  VERIFY_A_OP_B_INT(gpio_get_out_level(MODEM_ENABLE_PIN), ==, HIGH);
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT\r");
  fake_modem_reply(&serial);

  for (int i = 0; i < 15 && serial.write_buf.empty(); ++i) {
    client->poll();
    delay(10);
  }
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CGMM\r");
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CGMR\r");
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT#XSMVER\r");
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CGSN=2\r");
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT%CMNG=1,0,0\r");  // check certs
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CMEE=1\r");  // ext. errors on
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT%XPDNCFG=1\r");  // always-on IP
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CFUN=1\r");  // radio on
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CEREG=1\r");  // reg notify on
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CGEREP=1\r");  // data notify on
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT%XMONITOR\r");  // check radio
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT+CGPADDR\r");  // check IP
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT#XMQTTCON?\r");  // check MQTT
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT#XMQTTCON=0\r");  // disconnect
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(
    serial.write_buf, ==,
    "AT#XMQTTCFG=\"1122222233333344\",60,1\r"  // configure MQTT
  );
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(
    serial.write_buf, ==,
    "AT#XMQTTCON=1,\"fake-user\",\"fake-pass\",\"fake-server\",8883,0\r"
  );
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT#XMQTTSUB=\"topic1\",0\r");
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "AT#XMQTTSUB=\"topic2\",0\r");
  fake_modem_reply(&serial);

  client->poll();
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "");  // idle

  // Status after initial setup
  auto const& status = client->poll();
  VERIFY_A_OP_B_STR(status.hardware, ==, "Fake Hardware");
  VERIFY_A_OP_B_STR(status.imeisv, ==, "1122222233333344");
  VERIFY_A_OP_B_STR(status.versions[0], ==, "Fake Revision");
  VERIFY_A_OP_B_STR(status.versions[1], ==, "Fake SM");
  VERIFY_A_OP_B_STR(status.versions[2], ==, "Fake NCS");
  VERIFY_A_OP_B_STR(status.versions[3], ==, "Blub");
  VERIFY_A_OP_B_INT(status.running, >, 0);
  VERIFY_A_OP_B_INT(status.registered, >, 0);
  VERIFY_A_OP_B_INT(status.roaming, >, 0);
  VERIFY_A_OP_B_INT(status.failed, ==, 0);
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
  VERIFY_A_OP_B_INT(status.ip_attached, >, 0);
  VERIFY_A_OP_B_INT(status.ip_addr, ==, 0x0C22384E);
  VERIFY_A_OP_B_INT(status.mqtt_ready, >, 0);
}

static void test_mqtt_publish() {
  OK_NOTE("\n#TEST# test_mqtt_publish");
  FakeSerial serial;
  auto const client = make_cell_modem_client(
    &serial, MODEM_ENABLE_PIN, fake_mqtt_config(), {}
  );
  if (!run_client_setup(client, &serial)) return;

  auto const& st1 = client->poll();
  if (!VERIFY_A_OP_B_INT(st1.mqtt_publish_busy, ==, 0)) return;
  client->publish({.topic = "test-topic", .payload = "test-payload"});

  auto const& st2 = client->poll();
  VERIFY_A_OP_B_INT(st2.mqtt_publish_busy, >, 0);
  VERIFY_A_OP_B_STR(
    serial.write_buf, ==, "AT#XMQTTPUB=\"test-topic\",\"\",0,0,12\r"
  );
  fake_modem_reply(&serial);

  auto const& st3 = client->poll();
  VERIFY_A_OP_B_INT(st3.mqtt_publish_busy, >, 0);
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "test-payload");
  serial.write_buf.clear();
  serial.read_buf = "#XDATAMODE: 0\r\n";

  auto const& st4 = client->poll();
  VERIFY_A_OP_B_INT(st4.mqtt_publish_busy, ==, 0);  // after #XDATAMODE: 0
  VERIFY_A_OP_B_STR(serial.write_buf, ==, "");  // idle after message send
}

void setup() {
  Serial1.begin(115200);
  ok_logging_stream = &Serial1;
  pinMode(MODEM_ENABLE_PIN, OUTPUT);
  OK_NOTE("#BEGIN-TESTS#");
  test_blub_cert_sha256();
  test_modem_client_setup();
  test_mqtt_publish();
  OK_NOTE("#END-TESTS#");
}

void loop() {}
