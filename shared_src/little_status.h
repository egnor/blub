// Conveniently update text on small mono screens (via U8g2)

#pragma once

extern "C" struct u8g2_struct;

class LittleStatus {
 public:
  virtual ~LittleStatus() = default;
  virtual void line_printf(int line, char const* format, ...) = 0;
  virtual u8g2_struct* raw_driver() const = 0;
};

LittleStatus* make_little_status(u8g2_struct* driver);
