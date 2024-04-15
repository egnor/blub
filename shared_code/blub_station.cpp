#include "blub_station.h"

#include <Arduino.h>
#include <Wire.h>

// This file only uses u8g2.h, but pio won't find the library based on that?
#include <ArduinoLog.h>
#include <U8g2lib.h>

#include "little_status.h"

static constexpr int SCREEN_I2C_ADDR = 0x3D;
static constexpr int SCREEN_NRESET_PIN = 10;

static u8g2_t screen_driver;
LittleStatus* status_screen = nullptr;

class DummyStatus : public LittleStatus {
  public:
    virtual void line_printf(int line, char const* format, ...) override {}
    virtual u8g2_t* raw_driver() const override { return nullptr; }
};

static void print_log_prefix(Print* log_output, int level) {
  auto const now = millis();
  log_output->printf("%7.3f ", now * 1e-3f);
  switch (level) {
    case LOG_LEVEL_FATAL: log_output->print("💥 "); break;
    case LOG_LEVEL_ERROR: log_output->print("🔥 "); break;
    case LOG_LEVEL_WARNING: log_output->print("⚠️ "); break;
    case LOG_LEVEL_INFO: break;
    case LOG_LEVEL_TRACE: log_output->print("▫️ "); break;
    case LOG_LEVEL_VERBOSE: log_output->print("🕸️ "); break;
    default: log_output->printf("<%d> ", level); break;
  }
}

static void print_log_suffix(Print* log_output, int level) {
  log_output->println();
}

void blub_station_init(char const* name) {
  Serial.begin(115200);  // Debug console
  Log.begin(LOG_LEVEL_TRACE, &Serial, false);
  Log.setPrefix(print_log_prefix);
  Log.setSuffix(print_log_suffix);
  Log.info("💡 BLUB station: %s", name);

  Log.trace("Waking screen (if present)");
  pinMode(SCREEN_NRESET_PIN, OUTPUT);
  digitalWrite(SCREEN_NRESET_PIN, HIGH);
  delay(10);

  Log.trace("I2C setup: SCL=%d SDA=%d", SCL, SDA);
  if (!Wire.setSDA(SDA) || !Wire.setSCL(SCL)) {
    Log.fatal("I2C setup failed (SCL=%d SDA=%d)", SCL, SDA);
    while (true) {}
  }

  Wire.begin();
  Wire.beginTransmission(SCREEN_I2C_ADDR);
  auto const i2c_status = Wire.endTransmission();
  if (i2c_status == 2) {
    Log.warning("No status screen found (I2C %x)", SCREEN_I2C_ADDR);
    status_screen = new DummyStatus();
  } else if (i2c_status != 0) {
    Log.error("Error %d probing screen (I2C %x)", i2c_status, SCREEN_I2C_ADDR);
    status_screen = new DummyStatus();
  } else {
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(
        &screen_driver, U8G2_R0,
        u8x8_byte_arduino_hw_i2c, u8x8_gpio_and_delay_arduino
    );

    // See https://github.com/olikraus/u8g2/issues/2425
    u8x8_SetPin_HW_I2C(&screen_driver.u8x8, SCREEN_NRESET_PIN);
    u8g2_SetI2CAddress(&screen_driver, SCREEN_I2C_ADDR << 1);
    u8g2_InitDisplay(&screen_driver);
    u8g2_SetPowerSave(&screen_driver, 0);
    status_screen = make_little_status(&screen_driver);
    status_screen->line_printf(0, "\f9\b%s", name);
  }
}
