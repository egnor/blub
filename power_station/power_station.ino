#include <array>
#include <optional>

#include <Arduino.h>

#include <Adafruit_INA228.h>

#include "src/blub_station.h"
#include "src/little_status.h"
#include "src/tagged_logging.h"

static const TaggedLoggingContext TL_CONTEXT("power_station");

struct meter {
  int i2c_address;
  char const* name;
  std::optional<Adafruit_INA228> driver;
};

std::array<meter, 3> meters({
  {INA228_I2CADDR_DEFAULT, "Load"},
  {INA228_I2CADDR_DEFAULT + 1, "PPT"},
  {INA228_I2CADDR_DEFAULT + 4, "Panel"},
});

void loop() {
  TL_SPAM("LOOP");

  status_screen->line_printf(0, "\f9POWER STATION");
  status_screen->line_printf(1, "\f3 ");

  char line[80] = "";
  for (auto& meter : meters) {
    if (meter.driver) {
      sprintf(line + strlen(line), "\t\f9\b%s\b", meter.name);
      TL_NOTICE(
          "%s: %.1fV %.1fmA [%.3fmVs] %.0fmW %.3fJ %.1fC", meter.name,
          meter.driver->readBusVoltage() * 1e-3,
          meter.driver->readCurrent(),
          meter.driver->readShuntVoltage(),
          meter.driver->readPower(),
          meter.driver->readEnergy(),
          meter.driver->readDieTemp());
    } else {
      TL_NOTICE("%s: not detected at startup", meter.name);
    }
  }
  status_screen->line_printf(2, "%s", line + 1);

  strcpy(line, "");
  for (auto& meter : meters) {
    if (!meter.driver) continue;
    float sign = meter.driver->readCurrent() < 0 ? -1 : 1;
    float power = meter.driver->readPower() * 1e-3 * sign;
    sprintf(line + strlen(line), "\t\f12%.2fW", power);
  }
  status_screen->line_printf(3, "%s", line + 1);

  strcpy(line, "");
  for (auto& meter : meters) {
    if (!meter.driver) continue;
    auto const milliamps = meter.driver->readCurrent();
    if (milliamps >= -999 && milliamps <= 999) {
      sprintf(line + strlen(line), "\t\f8%.1f\f6V\2\f8%.0f\f6mA",
              meter.driver->readBusVoltage() * 1e-3, milliamps);
    } else {
      sprintf(line + strlen(line), "\t\f8%.1f\f6V\2\f8%.1f\f6A",
              meter.driver->readBusVoltage() * 1e-3, milliamps * 1e-3);
    }
  }
  status_screen->line_printf(4, "%s", line + 1);

  delay(500);
}

void setup() {
  blub_station_init("BLUB power station");
  for (auto& meter : meters) {
    meter.driver.emplace();
    if (meter.driver->begin(meter.i2c_address)) {
      TL_NOTICE("\"%s\" meter at 0x%x", meter.name, meter.i2c_address);
      meter.driver->setShunt(0.015, 10.0);
      meter.driver->setCurrentConversionTime(INA228_TIME_4120_us);
    } else {
      TL_PROBLEM("No \"%s\" meter at 0x%x", meter.name, meter.i2c_address);
      meter.driver.reset();
    }
  }
}