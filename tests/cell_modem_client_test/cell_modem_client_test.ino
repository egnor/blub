#include <Arduino.h>

#include "cell_modem_client.h"

void setup() {
    Serial1.begin(115200);
    Serial1.println("BEGIN-TEST");

    Serial1.println("END-TEST");
}

void loop() {}
