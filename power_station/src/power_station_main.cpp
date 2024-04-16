#include <array>

#include <Arduino.h>
#include <tusb.h>

#include <Adafruit_INA228.h>

#include "blub_station.h"
#include "little_status.h"
#include "chatty_logging.h"

struct meter {
  int i2c_address;
  char const* name;
  Adafruit_INA228 driver;
};

std::array<meter, 1> meters = {
  {INA228_I2CADDR_DEFAULT, "Load"},
};

void loop() {
  CL_NOTE("LOOP");

  status_screen->line_printf(0, "\f9POWER STATION");
  status_screen->line_printf(1, "\f3 ");

  char line[80] = "";
  for (auto& meter : meters) {
    sprintf(line + strlen(line), "\t\f9\b%s", meter.name);
    CL_NOTE("%s: %.1fV %.6fA [%.6fVs] %.6fW %.6fJ %.1fC", meter.name,
            meter.driver.readBusVoltage() * 1e-3,
            meter.driver.readCurrent() * 1e-3,
            meter.driver.readShuntVoltage() * 1e-3,
            meter.driver.readPower() * 1e-3,
            meter.driver.readEnergy(),
            meter.driver.readDieTemp());
  }
  status_screen->line_printf(2, "%s", line + 1);

  strcpy(line, "");
  for (auto& meter : meters) {
    float power = meter.driver.readPower() * 1e-3;
    sprintf(line + strlen(line), "\t\f12%.3fW", power);
  }
  status_screen->line_printf(3, "%s", line + 1);

  strcpy(line, "");
  for (auto& meter : meters) {
    sprintf(line + strlen(line), "\t\f8%.1f\f6V \f8%.1f\f6mA",
            meter.driver.readBusVoltage() * 1e-3,
            meter.driver.readCurrent());
  }
  status_screen->line_printf(4, "%s", line + 1);

  delay(500);
}

void setup() {
  blub_station_init("BLUB power station");
  for (auto& meter : meters) {
    if (meter.driver.begin(meter.i2c_address)) {
      CL_NOTE("%s meter at 0x%x", meter.name, meter.i2c_address);
      meter.driver.setShunt(0.015, 10.0);
      meter.driver.setCurrentConversionTime(INA228_TIME_4120_us);
    } else {
      CL_FATAL("No %s at 0x%x", meter.name, meter.i2c_address);
    }
  }
}
