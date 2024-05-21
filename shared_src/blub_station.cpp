#include "blub_station.h"

#include <Arduino.h>
#include <tusb.h>
#include <U8g2lib.h>
#include <Wire.h>

#include "tagged_logging.h"
#include "little_status.h"
#include "xbee_radio.h"

static const TaggedLoggingContext TL_CONTEXT("blub_station");

static constexpr int SCREEN_I2C_ADDR = 0x3D;
static constexpr int SCREEN_NRESET_PIN = 10;

static constexpr int XBEE_NRESET_PIN = 11;
static constexpr int TO_XBEE_PIN = 12;
static constexpr int FROM_XBEE_PIN = 13;

static u8g2_t screen_driver;
LittleStatus* status_screen = nullptr;
XBeeRadio* xbee_radio = nullptr;

class DummyStatus : public LittleStatus {
  public:
    virtual void line_printf(int line, char const* format, ...) override {}
    virtual u8g2_t* raw_driver() const override { return nullptr; }
};

class DummyXBee : public XBeeRadio {
  public:
    virtual int outgoing_space() const override { return 0; }
    virtual void add_outgoing(XBeeAPI::Frame const&) override {}
    virtual bool poll_for_frame(XBeeAPI::Frame*) override { return false; }
    virtual HardwareSerial* raw_serial() const override { return nullptr; }
};

void blub_station_init(char const* name) {
  Serial.begin(115200);  // Debug console
  set_logging_output(&Serial);

  // If there's a USB host, wait a bit for serial connection before starting
  auto const start = millis();
  bool usb_host_seen = false;
  do {
    usb_host_seen = usb_host_seen || tud_connected();
  } while (
    (usb_host_seen && !Serial.dtr() && (millis() - start < 5000)) ||
    (!usb_host_seen && (millis() - start < 500))
  );

  TL_NOTICE("💡 %s", name);

  TL_SPAM("Reset and wake screen and/or XBee (if present)");
  pinMode(SCREEN_NRESET_PIN, OUTPUT);
  pinMode(XBEE_NRESET_PIN, OUTPUT);
  digitalWrite(SCREEN_NRESET_PIN, LOW);
  digitalWrite(XBEE_NRESET_PIN, LOW);
  delay(10);

  pinMode(FROM_XBEE_PIN, INPUT);
  pinMode(TO_XBEE_PIN, OUTPUT);
  digitalWrite(TO_XBEE_PIN, HIGH);

  digitalWrite(SCREEN_NRESET_PIN, HIGH);
  digitalWrite(XBEE_NRESET_PIN, HIGH);
  delay(100);

  TL_SPAM("I2C setup (SCL=%d SDA=%d)", SCL, SDA);
  if (!Wire.setSDA(SDA) || !Wire.setSCL(SCL)) {
    TL_FATAL("Bad I2C pins (SCL=%d SDA=%d)", SCL, SDA);
  }

  Wire.begin();
  Wire.beginTransmission(SCREEN_I2C_ADDR);
  auto const i2c_status = Wire.endTransmission();
  if (i2c_status == 2) {
    TL_NOTICE("No screen found (I2C addr 0x%x)", SCREEN_I2C_ADDR);
    status_screen = new DummyStatus();
  } else if (i2c_status != 0) {
    TL_PROBLEM("Error %d probing screen (0x%x)", i2c_status, SCREEN_I2C_ADDR);
    status_screen = new DummyStatus();
  } else {
    TL_NOTICE("🖥️ Screen found (I2C addr 0x%x)", SCREEN_I2C_ADDR);
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
    status_screen->line_printf(1, "\f9Running...");
  }

  // If the XBee is present, it will force the radio-to-CPU line high
  pinMode(FROM_XBEE_PIN, INPUT_PULLDOWN);
  delay(1);
  if (digitalRead(FROM_XBEE_PIN) == HIGH) {
    TL_NOTICE("📻 XBee found (from=%d to=%d)", FROM_XBEE_PIN, TO_XBEE_PIN);
    if (!Serial1.setPinout(TO_XBEE_PIN, FROM_XBEE_PIN)) {
      TL_FATAL("XBee pinout error (TX=%d RX=%d)", TO_XBEE_PIN, FROM_XBEE_PIN);
    }
    if (!Serial1.setFIFOSize(512)) TL_FATAL("XBee FIFO error");
    xbee_radio = make_xbee_radio(&Serial1);
  } else {
    TL_NOTICE("No XBee found (from=%d)", FROM_XBEE_PIN);
    xbee_radio = new DummyXBee();
  }
}
