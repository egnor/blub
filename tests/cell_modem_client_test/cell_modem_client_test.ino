#include "cell_modem_client.h"

#include <Arduino.h>
#include <fake_serial.h>
#include <verifiers.h>

static OkLoggingContext OK_CONTEXT("cell_modem_client_test");

void setup() {
    Serial1.begin(115200);
    Serial1.println("BEGIN-TEST");
    ok_logging_stream = &Serial1;
    OK_ERROR("Starting setup");

    etl::string<1024> write_buf;
    FakeSerial fake_serial(0, "", &write_buf);
    auto client = make_cell_modem_client(&fake_serial, "mqtt-serv");

    client->poll();
    VERIFY_A_OP_B_STR(trim_view_whitespace(write_buf), ==, "AT+CGMM");

    Serial1.println("END-TEST");
}

void loop() {}
