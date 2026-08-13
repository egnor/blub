#include "cell_modem_client.h"

#include <Arduino.h>
#include <fake_serial.h>

void setup() {
    Serial1.begin(115200);
    Serial1.println("BEGIN-TEST");

    FakeSerial fake_serial(1024, 1024);

    auto cell_modem_client = make_cell_modem_client(&fake_serial, "mqtt-serv");

    Serial1.println("END-TEST");
}

void loop() {}
