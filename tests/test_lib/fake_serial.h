#pragma once

#include <Arduino.h>
#include <etl/circular_buffer.h>
#include <etl/memory.h>
#include <stdint.h>

class FakeSerial: public arduino::HardwareSerial {
 public:
  FakeSerial(unsigned long baud, etl::string_view rbuf, etl::istring* wbuf)
    : baud(baud), read_buf(rbuf), write_buf(wbuf) {}
  virtual ~FakeSerial() = default;

  unsigned long baud = 0;
  etl::string_view read_buf;
  etl::istring *write_buf = nullptr;

  void begin(unsigned long baud) override { this->baud = baud; }
  void begin(unsigned long baud, uint16_t conf) override { this->baud = baud; }
  void end() override { this->baud = 0; }
  operator bool() { return baud != 0; }

  int available() override { return read_buf.size(); }
  int peek() override { return read_buf.empty() ? -1 : read_buf.front(); }
  int read() override {
    int const v = peek();
    if (v >= 0) read_buf.remove_prefix(1);
    return v;
  }

  int availableForWrite() override {
    return write_buf ? write_buf->available() : 0;
  }

  size_t write(uint8_t c) override {
    if (!availableForWrite()) return 0;
    write_buf->push_back(c);
    return 1;
  }

  void flush() override {}
};
