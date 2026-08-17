#include "cell_modem_client.h"

#include <Arduino.h>
#include <fake_serial.h>
#include <verifiers.h>

static OkLoggingContext OK_CONTEXT("cell_modem_client_test");

static void test_modem_client_setup() {
  OK_NOTE("#TEST# test_modem_client_setup");
  etl::string<1024> write_buf;
  FakeSerial fake_serial(0, "", &write_buf);
  auto const client = make_cell_modem_client(&fake_serial, "mqtt-serv");

  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGMM\r\n");
  write_buf.clear();
  fake_serial.read_buf = "Fake Hardware\r\nOK\r\n";

  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGSN=2\r\n");
  write_buf.clear();
  fake_serial.read_buf = "+CGSN: \"490154203237518\"\r\nOK\r\n";

  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGMR\r\n");
  write_buf.clear();
  fake_serial.read_buf = "Fake Revision\r\nOK\r\n";

  client->poll();
  VERIFY_A_OP_B_STR(write_buf, ==, "AT#XSMVER\r\n");
  write_buf.clear();
  fake_serial.read_buf = "#XSMVER: \"Fake SM\",\"Fake NCS\",\"Fake Blub\"\r\n";

  auto const& status = client->poll();
  VERIFY_A_OP_B_STR(status.hardware, ==, "Fake Hardware");
  VERIFY_A_OP_B_STR(status.imeisv, ==, "490154203237518");
  VERIFY_A_OP_B_STR(status.versions[0], ==, "Fake Revision");
  VERIFY_A_OP_B_STR(status.versions[1], ==, "Fake SM");
  VERIFY_A_OP_B_STR(status.versions[2], ==, "Fake NCS");
  VERIFY_A_OP_B_STR(status.versions[3], ==, "Fake Blub");
}

void setup() {
  Serial1.begin(115200);
  ok_logging_stream = &Serial1;
  OK_NOTE("#BEGIN-TESTS#");
  test_modem_client_setup();
  OK_NOTE("#END-TESTS#");
}

void loop() {}
