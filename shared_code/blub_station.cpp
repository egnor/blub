#include "blub_station.h"

#include <Arduino.h>
#include <tusb.h>

// This file only uses u8g2.h, but pio won't find the library based on that?
#include <U8g2lib.h>
#include <Wire.h>

#include "chatty_logging.h"
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

void blub_station_init(char const* name) {
  Serial.begin(115200);  // Debug console
  set_chatty_output(&Serial);

  // If there's a USB host, wait a bit for serial connection before starting
  auto const start = millis();
  bool usb_host_seen = false;
  do {
    usb_host_seen = usb_host_seen || tud_connected();
  } while (
    (usb_host_seen && !Serial.dtr() && (millis() - start < 5000)) ||
    (!usb_host_seen && (millis() - start < 500))
  );

  CL_NOTE("💡 %s", name);

  CL_SPAM("Waking screen (if present)");
  pinMode(SCREEN_NRESET_PIN, OUTPUT);
  digitalWrite(SCREEN_NRESET_PIN, HIGH);
  delay(10);

  CL_SPAM("I2C setup (SCL=%d SDA=%d)", SCL, SDA);
  if (!Wire.setSDA(SDA) || !Wire.setSCL(SCL)) {
    CL_FATAL("I2C setup failed (SCL=%d SDA=%d)", SCL, SDA);
    while (true) {}
  }

  Wire.begin();
  Wire.beginTransmission(SCREEN_I2C_ADDR);
  auto const i2c_status = Wire.endTransmission();
  if (i2c_status == 2) {
    CL_PROBLEM("No screen found (I2C addr 0x%x)", SCREEN_I2C_ADDR);
    status_screen = new DummyStatus();
  } else if (i2c_status != 0) {
    CL_PROBLEM("Error %d probing screen (0x%x)", i2c_status, SCREEN_I2C_ADDR);
    status_screen = new DummyStatus();
  } else {
    CL_SPAM("Screen found (I2C addr 0x%x)", SCREEN_I2C_ADDR);
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
