#include "cell_modem_client.h"

#include <Arduino.h>
#include <fake_serial.h>
#include <verifiers.h>

char const* const ok_logging_config = "DETAIL";

static OkLoggingContext OK_CONTEXT("cell_modem_client_test");

static void test_modem_client_setup() {
  OK_NOTE("#TEST# test_modem_client_setup");
  etl::string<1024> write_buf;
  FakeSerial fake_serial(0, "", &write_buf);
  auto const client = make_cell_modem_client(&fake_serial, "mqtt-serv");
  client->poll();

  //
  // Initialization and poll cycle
  //

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

  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CMEE=1\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT%XPDNCFG=1\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CFUN=1\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CEREG=3\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGEREP=1\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT%XMONITOR\r\n");
  write_buf.clear();
  fake_serial.read_buf = "%XMONITOR: 5,"
    "\"\",\"\",\"310260\",\"417B\",7,12,\"02C80005\",211,5035,49,31,"
    "\"\",\"11100000\",\"11100000\",\"01001001\"\r\nOK\r\n";
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CGPADDR\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";
  client->poll();

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

  // Unsolicited registration update (+CEREG) and status change
  VERIFY_A_OP_B_STR(write_buf, ==, "");
  fake_serial.read_buf = "+CEREG: 5,\"417B\",\"02C80006\",7\r\n";
  auto const& status2 = client->poll();
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
  VERIFY_A_OP_B_INT(status.op_mcc, ==, 0);  // reset with cell change
  VERIFY_A_OP_B_INT(status.op_mnc, ==, 0);  // reset with cell change
  VERIFY_A_OP_B_INT(status.cell_tac, ==, 0x417B);
  VERIFY_A_OP_B_INT(status.cell_phys_id, ==, 0);  // reset with cell change
  VERIFY_A_OP_B_INT(status.cell_id, ==, 0x02C80006);
  VERIFY_A_OP_B_INT(status.radio_earfcn, ==, 0);  // reset with cell change
  VERIFY_A_OP_B_INT(status.radio_tech, ==, 7);
  VERIFY_A_OP_B_INT(status.radio_band, ==, 0);  // reset with cell change
  VERIFY_A_OP_B_INT(status.radio_rsrp, ==, -0x8000);  // reset with cell change
  VERIFY_A_OP_B_INT(status.radio_snr, ==, -0x8000);  // reset with cell change
}

void setup() {
  Serial1.begin(115200);
  ok_logging_stream = &Serial1;
  OK_NOTE("#BEGIN-TESTS#");
  test_modem_client_setup();
  OK_NOTE("#END-TESTS#");
}

void loop() {}
