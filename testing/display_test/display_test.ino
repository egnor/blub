#include <Arduino.h>

#include <ok_logging.h>
#include <U8g2lib.h>
#include <Wire.h>

static const OkLoggingContext OK_CONTEXT("display_test");
extern char const* const ok_logging_config = "*=DETAIL";

using ScreenDriver = U8G2_SSD1306_64X32_1F_F_HW_I2C;
ScreenDriver* screen;

void loop() {
  OK_NOTE("LOOP");

  delay(300);
  screen->clearBuffer();
  screen->drawStr(10, 10, "Hello World!");
  screen->sendBuffer();

  delay(300);
  screen->clearBuffer();
  screen->drawStr(10, 10, "World Hello!");
  screen->sendBuffer();
}

void setup() {
  Serial.begin(115200);
  OK_NOTE("SETUP");
  Wire.begin();
  screen = new ScreenDriver(U8G2_R2);
  OK_FATAL_IF(!screen->begin());
  screen->clearDisplay();
  screen->setPowerSave(0);
  screen->setFont(u8g2_font_ncenB08_tr);
  screen->setDrawColor(1);
}
