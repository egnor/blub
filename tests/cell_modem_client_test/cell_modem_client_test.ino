#include "cell_modem_client.h"

#include <Arduino.h>
#include <fake_serial.h>
#include <verifiers.h>

char const* const ok_logging_config = "DETAIL";

static OkLoggingContext OK_CONTEXT("cell_modem_client_test");

static void test_modem_client_setup() {
  OK_NOTE("#TEST# test_modem_client_setup");
  etl::string<8192> write_buf;
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

  VERIFY_A_OP_B_STR(write_buf, ==, "AT%CMNG=1,0,0\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";  // no certs initially
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT+CFUN=4\r\n");
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";  // no certs initially
  client->poll();

  VERIFY_A_OP_B_STR(
    write_buf, ==,
    "AT%CMNG=0,0,0,\"-----BEGIN CERTIFICATE-----\r\n"
    "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\r\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\r\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\r\n"
    "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\r\n"
    "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\r\n"
    "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\r\n"
    "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\r\n"
    "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\r\n"
    "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\r\n"
    "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\r\n"
    "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\r\n"
    "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\r\n"
    "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\r\n"
    "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\r\n"
    "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\r\n"
    "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\r\n"
    "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\r\n"
    "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\r\n"
    "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\r\n"
    "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\r\n"
    "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\r\n"
    "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\r\n"
    "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\r\n"
    "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\r\n"
    "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\r\n"
    "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\r\n"
    "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\r\n"
    "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\r\n"
    "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\r\n"
    "-----END CERTIFICATE-----\r\n"
    "\"\r\n"
  );
  write_buf.clear();
  fake_serial.read_buf = "OK\r\n";
  client->poll();

  VERIFY_A_OP_B_STR(write_buf, ==, "AT%CMNG=1,0,0\r\n");
  write_buf.clear();
  fake_serial.read_buf =
    "%CMNG: 0,0,\"4C99356C265EE06C0AE0502E74D38231263513726D001CFE28EA25E70AF2CC7F\"\r\n"
    "OK\r\n";
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
