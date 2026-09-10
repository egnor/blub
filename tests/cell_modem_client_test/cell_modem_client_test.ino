#include "cell_modem_client.h"

#include <Arduino.h>
#include <etl/format.h>
#include <SHA256.h>

#include <blub_mqtt_config.h>
#include <fake_serial.h>
#include <verifiers.h>

char const* const ok_logging_config = "DETAIL";

static OkLoggingContext OK_CONTEXT("cell_modem_client_test");

static void test_blub_cert_sha256() {
  OK_NOTE("#TEST# test_blub_cert_sha256");
  SHA256 sha256;
  sha256.update(blub_mqtt_config.cert.data(), blub_mqtt_config.cert.size());
  uint8_t digest[32];
  sha256.finalize(digest, sizeof(digest));
  for (int i = 0; i < sizeof(digest); ++i) {
    OK_NOTE("blub_mqtt_cert_sha256[%d]", i);
    etl::string<3> digest_byte;
    etl::format_to(digest_byte, "{:02X}", digest[i]);
    auto const stored_byte = blub_mqtt_config.cert_sha256.substr(i * 2, 2);
    VERIFY_A_OP_B_STR(digest_byte, ==, stored_byte);
  }
}

static void test_modem_client_setup() {
  OK_NOTE("#TEST# test_modem_client_setup");
  etl::string<8192> write_buf;
  FakeSerial fake_serial(0, "", &write_buf);
  MqttServerConfig server;
  server.host = "server";
  server.port = 8883;
  server.user = "user";
  server.password = "pass";
  server.cert = "-----FAKE CERT-----\r\nABCDEF\r\n-----END CERT-----\r\n";
  server.cert_sha256 = "0123456789ABCDEF";

  etl::vector<etl::string_view, 2> subs({"topic1", "topic2"});
  auto const client = make_cell_modem_client(&fake_serial, server, subs);

  //
  // Initialization and poll cycle
  //

  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGMM\r\n");
  write_buf.clear();

  fake_serial.read_buf = "Fake Hardware\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGMR\r\n");
  write_buf.clear();

  fake_serial.read_buf = "Fake Revision\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT#XSMVER\r\n");
  write_buf.clear();

  fake_serial.read_buf = "#XSMVER: \"Fake SM\",\"Fake NCS\",\"Blub\"\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGSN=2\r\n");
  write_buf.clear();

  fake_serial.read_buf = "+CGSN: \"490154203237518\"\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT%CMNG=1,0,0\r\n");  // check certs
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";  // no certs initially
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CFUN=4\r\n");  // radio off (cert update)
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT%CMNG=3,0,0\r\n");  // delete cert
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(
    write_buf, ==,
    "AT%CMNG=0,0,0,\""  // write cert
    "-----FAKE CERT-----\r\nABCDEF\r\n-----END CERT-----\r\n"
    "\"\r\n"
  );
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT%CMNG=1,0,0\r\n");  // check certs again
  write_buf.clear();

  fake_serial.read_buf = "%CMNG: 0,0,\"0123456789ABCDEF\"\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CMEE=1\r\n");  // extended errors on
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT%XPDNCFG=1\r\n");  // always-on IP
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CFUN=1\r\n");  // radio on
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CEREG=1\r\n");  // reg notify on
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGEREP=1\r\n");  // packet notify on
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT%XMONITOR\r\n");  // get radio status
  write_buf.clear();

  fake_serial.read_buf = "%XMONITOR: 5,"
    "\"\",\"\",\"310260\",\"417B\",7,12,\"02C80005\",211,5035,49,31,"
    "\"\",\"11100000\",\"11100000\",\"01001001\"\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGPADDR\r\n");  // get IP status
  write_buf.clear();

  fake_serial.read_buf = "+CGPADDR: 0,\"10.83.129.137\"\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT#XMQTTCON?\r\n");  // get MQTT status
  write_buf.clear();

  fake_serial.read_buf = "#XMQTTCON: 0\r\nOK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT#XMQTTCON=0\r\n");  // explicit disconnect
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(
    write_buf, ==,
    "AT#XMQTTCFG=\"490154203237518\",60,1\r\n"  // configure MQTT client
  );
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(
    write_buf, ==,
    "AT#XMQTTCON=1,\"user\",\"pass\",\"server\",8883,0\r\n"  // connect to MQTT
  );
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "");  // waiting for CONNACK

  fake_serial.read_buf = "#XMQTTEVT: 0,0\r\n";  // MQTT CONNACK
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT#XMQTTSUB=\"topic1\",0\r\n");
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "");  // waiting for SUBACK

  fake_serial.read_buf = "#XMQTTEVT: 7,0\r\n";  // MQTT SUBACK
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT#XMQTTSUB=\"topic2\",0\r\n");
  write_buf.clear();

  fake_serial.read_buf = "OK\r\n";
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "");  // waiting for SUBACK

  fake_serial.read_buf = "#XMQTTEVT: 7,0\r\n";  // MQTT SUBACK
  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "");  // idle

  // Initial status after first poll cycle
  auto const& status = client->poll();
  VERIFY_A_OP_B_STR(status.hardware, ==, "Fake Hardware");
  VERIFY_A_OP_B_STR(status.imeisv, ==, "490154203237518");
  VERIFY_A_OP_B_STR(status.versions[0], ==, "Fake Revision");
  VERIFY_A_OP_B_STR(status.versions[1], ==, "Fake SM");
  VERIFY_A_OP_B_STR(status.versions[2], ==, "Fake NCS");
  VERIFY_A_OP_B_STR(status.versions[3], ==, "Blub");
  VERIFY_A_OP_B_INT(status.running, >, 0);
  VERIFY_A_OP_B_INT(status.registered, >, 0);
  VERIFY_A_OP_B_INT(status.roaming, >, 0);
  VERIFY_A_OP_B_INT(status.failed, ==, 0);
  VERIFY_A_OP_B_INT(status.op_mcc, ==, 310);
  VERIFY_A_OP_B_INT(status.op_mnc, ==, 260);
  VERIFY_A_OP_B_INT(status.cell_tac, ==, 0x417B);
  VERIFY_A_OP_B_INT(status.cell_phys_id, ==, 211);
  VERIFY_A_OP_B_INT(status.cell_id, ==, 0x02C80005);
  VERIFY_A_OP_B_INT(status.radio_earfcn, ==, 5035);
  VERIFY_A_OP_B_INT(status.radio_tech, ==, 7);
  VERIFY_A_OP_B_INT(status.radio_band, ==, 12);
  VERIFY_A_OP_B_INT(status.radio_rsrp, ==, -92);
  VERIFY_A_OP_B_INT(status.radio_snr, ==, +6);
  VERIFY_A_OP_B_INT(status.ip_attached, >, 0);
  VERIFY_A_OP_B_INT(status.ip_addr, ==, 0x0A538189);
  VERIFY_A_OP_B_INT(status.mqtt_connected, >, 0);
  VERIFY_A_OP_B_INT(status.mqtt_subscribed, >, 0);
}

void setup() {
  Serial1.begin(115200);
  ok_logging_stream = &Serial1;
  OK_NOTE("#BEGIN-TESTS#");
  test_blub_cert_sha256();
  test_modem_client_setup();
  OK_NOTE("#END-TESTS#");
}

void loop() {}
